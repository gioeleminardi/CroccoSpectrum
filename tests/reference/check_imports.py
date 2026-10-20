#!/usr/bin/env python3
"""Independent audio/text/compression import oracle; no application Python dependency."""
import bz2
import gzip
import json
import math
import os
import pathlib
import signal
import struct
import subprocess
import sys
import tempfile
import time
import wave

try:
    import numpy as np
    from scipy import signal as scipy_signal
    from scipy.io import wavfile
except ImportError:
    print("NumPy/SciPy unavailable: independent import checks skipped")
    sys.exit(77)

binary = pathlib.Path(sys.argv[1]).resolve()
checks = 0


with tempfile.TemporaryDirectory(prefix="rf-import-reference-") as directory:
    root = pathlib.Path(directory)
    temporary = root / "temporary"
    temporary.mkdir()
    environment = dict(os.environ, TMPDIR=str(temporary))

    def run(path, *options, command="inspect", success=True):
        global checks
        result = subprocess.run([str(binary), command, "--file", str(path), *options],
                                env=environment, text=True, capture_output=True, timeout=60)
        assert not list(temporary.iterdir()), "import leaked a temporary file"
        checks += 1
        if not success:
            assert result.returncode != 0, "invalid input was accepted"
            return
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)

    values = np.array([[-0.75, 0.25, 0.5], [0.125, -0.5, -0.25],
                       [0.5, 0.0625, -0.75], [-0.25, 0.75, 0.125]] * 4)

    def check_audio(path, expected, rate=48000):
        for kind, first, second in [("real", 2, None), ("complex", 3, 1)]:
            options = ["--kind", kind, "--column-i", str(first), "--sample-rate", "123"]
            if second:
                options += ["--column-q", str(second)]
            result = run(path, *options)
            target = np.column_stack((expected[:, first - 1],
                                      expected[:, second - 1] if second else np.zeros(16)))
            np.testing.assert_allclose(result["samples"], target, atol=0, rtol=0)
            assert result["recording"]["sample_rate"] == rate
            assert result["recording"]["bits"] == 64
            assert result["recording"]["encoding"] == "float"
            assert result["recording"]["path"] == str(path)
        run(path, "--column-i", "4", success=False)
        run(path, "--partial", success=False)

    # SciPy independently reads WAV PCM including packed 24-bit and IEEE floats.
    for bits in [8, 16, 24, 32]:
        path = root / f"pcm{bits}.wav"
        integers = (values * 2 ** (bits - 1)).astype(np.int64)
        raw = b"".join((int(value) + (128 if bits == 8 else 0)).to_bytes(
            bits // 8, "little", signed=bits != 8) for value in integers.ravel())
        with wave.open(str(path), "wb") as writer:
            writer.setparams((3, bits // 8, 48000, 0, "NONE", "not compressed"))
            writer.writeframes(raw)
        rate, decoded = wavfile.read(path)
        expected = ((decoded.astype(float) - 128) / 128 if bits == 8 else
                    decoded.astype(float) / 2 ** (31 if bits == 24 else bits - 1))
        check_audio(path, expected, rate)
    for bits in [32, 64]:
        path = root / f"float{bits}.wav"
        wavfile.write(path, 48000, values.astype(f"float{bits}"))
        rate, decoded = wavfile.read(path)
        check_audio(path, decoded, rate)
        result = run(path, "--kind", "complex", "--column-i", "1", "--column-q", "2",
                     "--full-scale", "2")
        np.testing.assert_allclose(result["samples"], values[:, :2] / 2, atol=0, rtol=0)

    path = root / "odd-mono.wav"
    with wave.open(str(path), "wb") as writer:
        writer.setparams((1, 1, 8000, 0, "NONE", "not compressed"))
        writer.writeframes(bytes([32, 128, 192]))
    result = run(path)
    assert result["samples"] == [[-0.75, 0], [0, 0], [0.5, 0]]
    original = path.read_bytes()
    for invalid in [original[:-1], original[:20] + b"\x06\x00" + original[22:]]:
        path.write_bytes(invalid)
        run(path, success=False)
    path = root / "incomplete-frame.wav"
    with wave.open(str(path), "wb") as writer:
        writer.setparams((2, 2, 8000, 0, "NONE", "not compressed"))
        writer.writeframes(b"\0" * 7)
    run(path, success=False)

    # AIFF fixtures are independently encoded from the published IFF layout;
    # Python's aifc reader also verifies PCM frames where that stdlib exists.
    try:
        import aifc
    except ImportError:
        aifc = None

    def chunk(name, payload):
        return name + struct.pack(">I", len(payload)) + payload + b"\0" * (len(payload) % 2)

    def extended(rate):
        mantissa, exponent = math.frexp(rate)
        return struct.pack(">HQ", exponent + 16382, int(mantissa * 2 ** 64))

    path = root / "big-endian.wav"
    fmt = struct.pack(">HHIIHH", 1, 3, 48000, 48000 * 6, 6, 16)
    raw = (values * 32768).astype(">i2").tobytes()
    path.write_bytes(chunk(b"RIFX", b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"data", raw)))
    rate, decoded = wavfile.read(path)
    check_audio(path, decoded.astype(float) / 32768, rate)

    for floating, widths in [(False, [8, 16, 24, 32]), (True, [32, 64])]:
        for bits in widths:
            path = root / (f"{'float' if floating else 'pcm'}{bits}.aiff")
            raw = (values.astype(f">f{bits // 8}").tobytes() if floating else b"".join(
                int(value * 2 ** (bits - 1)).to_bytes(bits // 8, "big", signed=True)
                for value in values.ravel()))
            comm = struct.pack(">HIH", 3, len(values), bits) + extended(48000)
            if floating:
                comm += (b"fl32" if bits == 32 else b"fl64") + b"\0\0"
            body = ((b"AIFC" + chunk(b"FVER", struct.pack(">I", 0xA2805140)))
                    if floating else b"AIFF")
            body += chunk(b"COMM", comm) + chunk(b"SSND", b"\0" * 8 + raw)
            path.write_bytes(chunk(b"FORM", body))
            if not floating and aifc:
                with aifc.open(str(path), "rb") as reader:
                    assert reader.getframerate() == 48000
                    assert reader.getnchannels() == 3
                    reference = reader.readframes(16)
                    decoded = np.array([int.from_bytes(reference[index:index + bits // 8],
                                                        "big", signed=True)
                                        for index in range(0, len(reference), bits // 8)])
                    np.testing.assert_array_equal(decoded.reshape(-1, 3) / 2 ** (bits - 1), values)
            check_audio(path, values)

    # Preserve AIFF's original fractional extended-precision sample rate.
    path = root / "fractional.aiff"
    comm = struct.pack(">HIH", 3, len(values), 16) + extended(48000.5)
    raw = (values * 32768).astype(">i2").tobytes()
    path.write_bytes(chunk(b"FORM", b"AIFF" + chunk(b"JUNK", b"odd") + chunk(b"COMM", comm)
                           + chunk(b"SSND", b"\0" * 8 + raw)))
    check_audio(path, values, 48000.5)
    original = path.read_bytes()
    path.write_bytes(original[:-1])
    run(path, success=False)

    # Numeric columns use NumPy's independent text reader, retaining selected
    # values and explicit rate/full-scale choices rather than guessing units.
    for suffix, separator, option in [("csv", ",", "comma"), ("txt", " ", "whitespace"),
                                       ("tsv", "\t", "tab"), ("asc", ";", "semicolon")]:
        path = root / f"samples.{suffix}"
        np.savetxt(path, values, delimiter=separator, header="I,Q,aux", comments="")
        reference = np.loadtxt(path, delimiter=None if option == "whitespace" else separator,
                               skiprows=1)
        result = run(path, "--kind", "complex", "--column-i", "3", "--column-q", "1",
                     "--delimiter", option, "--skip-rows", "1", "--sample-rate", "1000.25")
        np.testing.assert_array_equal(result["samples"], reference[:, [2, 0]])
        assert result["recording"]["sample_rate"] == 1000.25
        result = run(path, "--kind", "real", "--column-i", "2", "--delimiter", option,
                     "--skip-rows", "1", "--full-scale", "2")
        np.testing.assert_array_equal(np.array(result["samples"])[:, 0], reference[:, 1] / 2)
        run(path, success=False)  # CLI text mapping must be explicit.
        run(path, "--kind", "complex", "--column-i", "1", "--column-q", "1", success=False)

    path = root / "invalid.csv"
    for invalid in ["0.5,\n", "bad,0.5\n", '"0.5",0.25\n', "0.5\n"]:
        path.write_text(invalid)
        run(path, "--kind", "complex", "--column-i", "1", "--column-q", "2", success=False)
    path.write_text("# comment\n\n0.5,inf\n-0.25,nan\n")
    result = run(path, "--kind", "complex", "--column-i", "1", "--column-q", "2")
    assert result["samples"] == [[0.5, None], [-0.25, None]]

    raw = (values[:, :2] * 32768).astype("<i2").tobytes()
    for suffix, compress, decompress in [("gz", gzip.compress, gzip.decompress),
                                          ("bz2", bz2.compress, bz2.decompress)]:
        path = root / f"recording.iq.{suffix}"
        for payload in [compress(raw), compress(raw[:17]) + compress(raw[17:])]:
            path.write_bytes(payload)
            reference = np.frombuffer(decompress(payload), dtype="<i2").reshape(-1, 2) / 32768
            result = run(path, "--sample-rate", "9876.5", "--center-frequency", "915000000")
            np.testing.assert_array_equal(result["samples"], reference)
            assert result["recording"]["sample_rate"] == 9876.5
            assert result["recording"]["center_frequency"] == 915000000
            assert result["recording"]["encoding"] == "signed"
            assert result["recording"]["bits"] == 16
        for payload in [b"", compress(raw)[:-1], compress(raw) + b"garbage",
                        compress(raw)[:-5] + b"\xff" * 5, compress(raw + b"x")]:
            path.write_bytes(payload)
            run(path, success=False)
        path.write_bytes(compress(b"header" + raw + b"tail"))
        result = run(path, "--offset", "6", "--length", str(len(raw)))
        np.testing.assert_array_equal(result["samples"], values[:, :2])

        # Highly compressible input exercises output draining with no input left.
        path.write_bytes(compress(b"\0" * (4 * 1024 * 1024)))
        result = run(path)
        assert int(result["frame_count"]) == 1024 * 1024

    # Check complete DSP passes after conversion against independent Welch PSDs.
    samples = 0.25 * np.sin(2 * np.pi * 733 * np.arange(4096) / 8192)
    frequency, power = scipy_signal.welch(samples, fs=8192, window="hann", nperseg=256,
                                          noverlap=128, detrend=False, scaling="density")
    audio = root / "analysis.wav"
    wavfile.write(audio, 8192, samples)
    text = root / "analysis.csv"
    np.savetxt(text, samples, fmt="%.17g")
    compressed = root / "analysis.raw.bz2"
    compressed.write_bytes(bz2.compress(samples.astype("<f8").tobytes()))
    for path, options in [(audio, []),
                          (text, ["--kind", "real", "--column-i", "1", "--sample-rate", "8192"]),
                          (compressed, ["--kind", "real", "--encoding", "float", "--bits", "64",
                                        "--sample-rate", "8192"])]:
        result = run(path, *options, "--fft", "256", "--scale", "density", command="average")
        np.testing.assert_allclose(result["frequencies_hz"], frequency, atol=0, rtol=0)
        np.testing.assert_allclose(result["power"], power, atol=1e-20, rtol=2e-10)
        assert int(result["valid_windows"]) == 31

    # Conversion memory must stay essentially constant as decoded size grows.
    path = root / "large.iq.gz"
    peaks = []
    # The large expansion exceeds the reference Python process's own RSS: Linux
    # getrusage may inherit that high-water mark across fork/exec.
    for size_mib in [1, 256]:
        with gzip.open(path, "wb") as writer:
            for _ in range(size_mib):
                writer.write(b"\0" * (1024 * 1024))
        result = run(path)
        assert int(result["frame_count"]) == size_mib * 1024 * 1024 // 4
        peaks.append(result["peak_rss_kib"])
    assert peaks[1] - peaks[0] < 16384, ("conversion memory scales with file size", peaks)

    # SIGINT must unwind the import and remove an in-progress temporary file.
    # Use a large expansion so observation does not race completion on fast hosts.
    with gzip.open(path, "wb") as writer:
        for _ in range(512):
            writer.write(b"\0" * (1024 * 1024))
    process = subprocess.Popen([str(binary), "inspect", "--file", str(path)],
                               env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True)
    def conversion_started():
        # Qt can keep its temporary file unnamed (Linux O_TMPFILE).
        for entry in pathlib.Path(f"/proc/{process.pid}/fd").glob("*"):
            try:
                if os.readlink(entry).startswith(str(temporary) + "/"):
                    return True
            except OSError:
                pass
        return False

    try:
        deadline = time.monotonic() + 10
        while not conversion_started() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.005)
        assert conversion_started(), "conversion did not start"
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=10)
        assert process.returncode != 0 and "Cancelled" in stderr, (stdout, stderr)
        assert not list(temporary.iterdir()), "cancelled import leaked a temporary file"
        checks += 1
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()

print(f"{checks} independent import checks passed; conversion peak RSS (KiB): {peaks}")
