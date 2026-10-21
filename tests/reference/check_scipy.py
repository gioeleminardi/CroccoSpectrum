#!/usr/bin/env python3
"""Independent encoding/PSD/Welch oracle; not needed by packaged applications."""
import json
import pathlib
import subprocess
import sys
import tempfile

try:
    import numpy as np
    from scipy import signal, stats
except ImportError:
    print("NumPy/SciPy unavailable: independent reference checks skipped")
    sys.exit(77)

binary = pathlib.Path(sys.argv[1]).resolve()
checks = 0


def run(command, path, *options, success=True):
    result = subprocess.run([str(binary), command, "--file", str(path), *options],
                            text=True, capture_output=True, timeout=60)
    if success and result.returncode:
        raise AssertionError(result.stderr)
    if not success:
        assert result.returncode != 0, "invalid input was accepted"
        return
    return json.loads(result.stdout)


with tempfile.TemporaryDirectory(prefix="rf-reference-") as directory:
    path = pathlib.Path(directory) / "fixture.iq"
    # Exact binary fractions avoid quantization ambiguity while testing every
    # decoder combination against Python/NumPy's independently encoded bytes.
    i_values = [-0.75, 0.125, 0.5, -0.25] * 4
    q_values = [0.25, -0.5, 0.0625, 0.75] * 4
    for encoding, widths in [("signed", [8, 16, 24, 32, 64]),
                             ("unsigned", [8, 16, 24, 32, 64]), ("float", [16, 32, 64])]:
        for bits in widths:
            for endian in ["little", "big"]:
                for kind, order in [("real", "IQ"), ("complex", "IQ"), ("complex", "QI")]:
                    values = i_values if kind == "real" else [v for pair in zip(i_values, q_values) for v in (pair if order == "IQ" else pair[::-1])]
                    if encoding == "float":
                        raw = np.asarray(values, dtype=("<" if endian == "little" else ">") + "f" + str(bits // 8)).tobytes()
                    else:
                        midpoint = 2 ** (bits - 1)
                        words = [int(v * midpoint) + (midpoint if encoding == "unsigned" else 0) for v in values]
                        raw = b"".join(v.to_bytes(bits // 8, endian, signed=encoding == "signed") for v in words)
                    path.write_bytes(raw)
                    result = run("inspect", path, "--encoding", encoding, "--bits", str(bits), "--endian", endian, "--kind", kind, "--order", order)
                    expected = np.column_stack([i_values, q_values if kind == "complex" else [0] * 16])
                    np.testing.assert_allclose(result["samples"], expected, atol=1e-15, rtol=1e-15)
                    checks += 1

    rng = np.random.default_rng(20261006)
    fs = 100_000_000.25
    for real in [False, True]:
        data = rng.normal(scale=0.1, size=17003) + (0 if real else 1j * rng.normal(scale=0.1, size=17003))
        path.write_bytes(np.asarray(data, dtype="<f8" if real else "<c16").tobytes())
        common = ["--encoding", "float", "--bits", "64", "--sample-rate", str(fs), "--kind", "real" if real else "complex"]
        for window in ["hann", "rectangular", "hamming", "blackman-harris"]:
            scipy_window = {"rectangular": "boxcar", "blackman-harris": "blackmanharris"}.get(window, window)
            for scale in ["spectrum", "density"]:
                for fft in [256, 4096]:
                    for detrend in [False, True]:
                        options = common + ["--fft", str(fft), "--window", window, "--scale", scale, "--overlap", "50"]
                        if detrend:
                            options.append("--remove-dc")
                        # Identical detrending, periodic windows, one-sided
                        # endpoints, and hop are specified explicitly.
                        frequency, expected = signal.welch(data, fs=fs, window=scipy_window,
                            nperseg=fft, noverlap=fft // 2, detrend="constant" if detrend else False,
                            return_onesided=real, scaling=scale, average="mean")
                        if not real:
                            frequency, expected = np.fft.fftshift(frequency), np.fft.fftshift(expected)
                        result = run("average", path, *options)
                        np.testing.assert_allclose(result["frequencies_hz"], frequency, atol=1e-7, rtol=1e-14)
                        np.testing.assert_allclose(result["power"], expected, atol=1e-20, rtol=2e-10)
                        assert int(result["valid_windows"]) == 1 + (len(data) - fft) // (fft // 2)
                        checks += 1

    # Integrated channel power and SNR use an independent SciPy PSD oracle.
    # Exercise real endpoints, negative complex frequencies, leakage, noise,
    # unequal bandwidths, and nonzero analyzed intervals in both trace modes.
    band_fs, band_fft = 8192.0, 1024
    spacing = band_fs / band_fft
    count = 16 * band_fft
    time = np.arange(count)
    for real in [False, True]:
        tone_bin = 97 if real else -97
        phase = 2 * np.pi * tone_bin * time / band_fft
        tone = 0.25 * (np.cos(phase) if real else np.exp(1j * phase))
        off_phase = 2 * np.pi * (tone_bin + 0.37) * time / band_fft
        off_tone = 0.25 * (np.cos(off_phase) if real else np.exp(1j * off_phase))
        noise = rng.normal(scale=0.015, size=count)
        if not real:
            noise = noise + 1j * rng.normal(scale=0.015, size=count)
        fixtures = [("tone", tone), ("off-bin", off_tone), ("noise", noise),
                    ("tone-noise", tone + noise), ("DC", np.full(count, 0.25)),
                    ("Nyquist", 0.25 * (-1.0) ** time), ("silence", np.zeros(count))]
        full_bounds = (0 if real else -band_fs / 2,
                       band_fs / 2 if real else band_fs / 2 - spacing)
        signal_bounds = (88 * spacing, 106 * spacing) if real else (-106 * spacing, -88 * spacing)
        noise_bounds = (160 * spacing, 215 * spacing)
        for fixture, data in fixtures:
            path.write_bytes(np.asarray(data, dtype="<f8" if real else "<c16").tobytes())
            for window in ["hann", "rectangular", "hamming", "blackman-harris"]:
                scipy_window = {"rectangular": "boxcar", "blackman-harris": "blackmanharris"}.get(window, window)
                for command in ["spectrum", "average"]:
                    start, end = 257, count - 101
                    analyzed_end = start + band_fft if command == "spectrum" else end
                    analyzed = data[start:analyzed_end]
                    if command == "spectrum":
                        frequency, psd = signal.periodogram(analyzed, fs=band_fs,
                            window=scipy_window, detrend=False, return_onesided=real, scaling="density")
                    else:
                        frequency, psd = signal.welch(analyzed, fs=band_fs,
                            window=scipy_window, nperseg=band_fft, noverlap=band_fft // 2,
                            detrend=False, return_onesided=real, scaling="density", average="mean")
                    if not real:
                        frequency, psd = np.fft.fftshift(frequency), np.fft.fftshift(psd)
                    expected = {}
                    for name, bounds in [("selected", full_bounds), ("signal", signal_bounds), ("noise", noise_bounds)]:
                        mask = (frequency >= bounds[0]) & (frequency <= bounds[1])
                        expected[name] = (float(psd[mask].sum() * spacing), int(mask.sum()) * spacing)
                    if fixture in ["tone", "DC", "Nyquist"]:
                        analytic = 0.25 ** 2 * (0.5 if real and fixture == "tone" else 1.0)
                        np.testing.assert_allclose(expected["selected"][0], analytic, atol=1e-13, rtol=1e-12)
                    for scale in ["spectrum", "density"]:
                        options = ["--encoding", "float", "--bits", "64", "--kind", "real" if real else "complex",
                                   "--sample-rate", str(band_fs), "--fft", str(band_fft), "--window", window,
                                   "--scale", scale, "--overlap", "50", "--start", str(start), "--end", str(end)]
                        for option, bounds in [("band", full_bounds), ("signal-band", signal_bounds), ("noise-band", noise_bounds)]:
                            options += ["--" + option, f"{bounds[0]}:{bounds[1]}"]
                        csv_path = pathlib.Path(directory) / f"band-{checks}.csv"
                        if fixture == "tone-noise":
                            options += ["--output", str(csv_path)]
                        result = run(command, path, *options)
                        measured = result["band_measurements"]
                        assert measured["power_unit"] == "dBFS"
                        for name, (integrated, bandwidth) in expected.items():
                            band = measured[name]
                            assert band["available"]
                            np.testing.assert_allclose(band["linear_power"], integrated, atol=1e-17, rtol=5e-10)
                            assert band["bandwidth_hz"] == bandwidth
                            if band["linear_power"] == 0:
                                assert band["power_dbfs"] == "-inf"
                            else:
                                np.testing.assert_allclose(band["power_dbfs"], 10 * np.log10(band["linear_power"]), atol=1e-12, rtol=0)
                                # Different FFT/window implementations can differ at roundoff-only leakage.
                                if integrated > 1e-25:
                                    np.testing.assert_allclose(band["power_dbfs"], 10 * np.log10(integrated), atol=1e-8, rtol=0)
                        occupied = measured["selected"]["occupied_bandwidth"]
                        assert occupied["percent"] == 99
                        assert occupied["noise_treatment"] == "included; no subtraction"
                        if expected["selected"][0] == 0:
                            assert not occupied["available"]
                            assert "reason" in occupied
                        else:
                            # Independent continuous histogram of the SciPy PSD:
                            # constant density in df-wide cells centered on bins.
                            edges = np.append(frequency - spacing / 2, frequency[-1] + spacing / 2)
                            distribution = stats.rv_histogram((psd, edges), density=False)
                            lower, upper = distribution.ppf([0.005, 0.995])
                            assert occupied["available"]
                            np.testing.assert_allclose(
                                [occupied["lower_hz"], occupied["upper_hz"], occupied["bandwidth_hz"]],
                                [lower, upper, upper - lower], atol=1e-7, rtol=1e-10)
                            np.testing.assert_allclose(
                                distribution.cdf(occupied["upper_hz"]) - distribution.cdf(occupied["lower_hz"]),
                                0.99, atol=1e-10, rtol=0)
                        total, signal_bw = expected["signal"]
                        noise_total, noise_bw = expected["noise"]
                        normalized_noise = noise_total * signal_bw / noise_bw
                        net_signal = max(0, total - normalized_noise)
                        snr = measured["snr"]
                        if fixture in ["tone", "DC", "Nyquist"]:
                            pass  # Pure-tone out-of-band roundoff is not a measurable noise floor.
                        elif normalized_noise == 0 and net_signal == 0:
                            assert not snr["available"]
                        else:
                            assert snr["available"]
                            np.testing.assert_allclose(snr["noise_in_signal_band_linear"], normalized_noise, atol=1e-17, rtol=5e-10)
                            np.testing.assert_allclose(snr["net_signal_linear"], net_signal, atol=1e-17, rtol=5e-10)
                            if net_signal == 0:
                                assert snr["snr_db"] == "-inf"
                            elif normalized_noise == 0:
                                assert snr["snr_db"] == "inf"
                            # Roundoff-only leakage has no physically useful SNR.
                            elif fixture in ["noise", "tone-noise", "off-bin"]:
                                np.testing.assert_allclose(snr["snr_db"], 10 * np.log10(net_signal / normalized_noise), atol=1e-8)
                        if fixture == "tone-noise":
                            metadata = json.loads(csv_path.read_text().splitlines()[0][2:])
                            assert metadata["band_measurements"] == measured
                            assert metadata["start"] == str(start) and metadata["end"] == str(analyzed_end)
                            assert metadata["fft_size"] == band_fft and metadata["hop_frames"] == str(band_fft // 2)
                            assert metadata["window"] == {"hann": "Hann", "rectangular": "Rectangular", "hamming": "Hamming", "blackman-harris": "Blackman-Harris"}[window]
                            assert metadata["remove_dc"] is False and metadata["conjugate"] is False
                            assert metadata["unit"] == ("dBFS" if scale == "spectrum" else "dBFS/Hz")
                        checks += 1

    # Reject ambiguous/empty/out-of-range requested bounds; overlap has an explicit status.
    for band in ["100:0", "nan:0", "1:2", "-5000:0", "0:5000"]:
        run("spectrum", path, "--encoding", "float", "--bits", "64", "--kind", "real",
            "--sample-rate", str(band_fs), "--fft", str(band_fft), "--band", band, success=False)
    overlap = run("spectrum", path, "--encoding", "float", "--bits", "64", "--kind", "real",
        "--sample-rate", str(band_fs), "--fft", str(band_fft),
        "--signal-band", "0:80", "--noise-band", "80:160")
    assert not overlap["band_measurements"]["snr"]["available"]
    assert "overlap" in overlap["band_measurements"]["snr"]["reason"]
    checks += 6

    # Every supported power of two, including the largest allocation, must
    # preserve complex tone power and its signed bin location.
    for exponent in range(8, 21):
        fft = 2 ** exponent
        data = 0.25 * np.exp(-2j * np.pi * 19 * np.arange(fft) / fft)
        path.write_bytes(data.astype("<c16").tobytes())
        result = run("spectrum", path, "--encoding", "float", "--bits", "64", "--fft", str(fft), "--window", "rectangular")
        powers = np.asarray(result["power"])
        assert int(np.argmax(powers)) == fft // 2 - 19
        np.testing.assert_allclose(powers.max(), 0.0625, rtol=1e-12)
        checks += 1

    for options in [["--fft", "1000"], ["--fft", "0"], ["--bits", "12"], ["--sample-rate", "nan"],
                    ["--sample-rate", "0"], ["--sample-rate", "-1"], ["--start", "18446744073709551616"], ["--overlap", "100"], ["--fft", "4096.5"], ["--bits", "16.1"], ["--fft", "1e100"], ["--overlap", "50.1"]]:
        run("spectrum", path, *options, success=False)
        checks += 1

print(f"Passed {checks} independent NumPy/SciPy comparisons and invalid-input checks")
