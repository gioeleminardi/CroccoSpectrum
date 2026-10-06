#!/usr/bin/env python3
"""Independent encoding/PSD/Welch oracle; not needed by packaged applications."""
import json
import pathlib
import subprocess
import sys
import tempfile

try:
    import numpy as np
    from scipy import signal
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
