#!/usr/bin/env python3
"""Explicit dense-file benchmark; uses up to 40 decimal GB temporarily.

No global cache dropping or privileged operation. DONTNEED advises eviction of
this fixture only; report it as an advisory cold run, not proven uncached I/O.
Generate tones plus noisy low bits so transparent filesystem compression does
not turn a repeating zero fixture into a misleading storage benchmark.
"""
import argparse
import array
import json
import math
import os
import pathlib
import random
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--cli", type=pathlib.Path, required=True)
parser.add_argument("--gui", type=pathlib.Path, required=True)
parser.add_argument("--output", type=pathlib.Path, required=True)
parser.add_argument("--max-bytes", type=int, default=40_000_000_000)
args = parser.parse_args()
cli, gui = args.cli.resolve(), args.gui.resolve()
rows = []
randomizer = random.Random(42)
noise = array.array("h")
noise.frombytes(randomizer.randbytes(262144 * 4))
block = array.array("h", (round(16384 * (math.cos if index % 2 == 0 else math.sin)(2 * math.pi * (index // 2) / 16)) + value // 8 for index, value in enumerate(noise))).tobytes()
if os.sys.byteorder != "little":
    raise SystemExit("Benchmark fixture writer currently requires little-endian host")

with tempfile.TemporaryDirectory(prefix="rf-dense-", dir="build") as directory:
    path = pathlib.Path(directory).resolve() / "tone.iq"
    with path.open("wb") as file:
        for size in [400_000_000, 4_000_000_000, 40_000_000_000]:
            if size > args.max_bytes:
                continue
            started = time.monotonic()
            while file.tell() < size:
                file.write(block[:min(len(block), size - file.tell())])
            file.flush()
            os.fsync(file.fileno())
            write_seconds = time.monotonic() - started
            frames = size // 4
            for cache in ["advised_cold", "warm"]:
                if cache == "advised_cold":
                    os.posix_fadvise(file.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                for command, start, end in [("spectrum", frames - 4096, frames)] + ([("average", 0, frames)] if size <= 4_000_000_000 else []):
                    started = time.monotonic()
                    result = subprocess.run([str(cli), command, "--file", str(path), "--start", str(start), "--end", str(end), "--fft", "4096"], capture_output=True, text=True, check=True, timeout=300)
                    wall_seconds = time.monotonic() - started
                    data = json.loads(result.stdout)
                    power = data["power"]
                    peak = max(range(len(power)), key=power.__getitem__)
                    assert abs(data["frequencies_hz"][peak] - 6_250_000) < 1
                    assert abs(10 * math.log10(power[peak]) + 6.020599913) < 0.1
                    rows.append(dict(bytes=size, allocated_bytes=path.stat().st_blocks * 512, operation=command, cache=cache,
                        elapsed_seconds=data["elapsed_seconds"], process_wall_seconds=wall_seconds, peak_rss_kib=data["peak_rss_kib"],
                        throughput_MB_s=(size / 1e6 / data["elapsed_seconds"]) if command == "average" else None,
                        fixture_extension_seconds=write_seconds))
            environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=str(path.parent / "config"))
            started = time.monotonic()
            subprocess.run([str(gui), "--accept-defaults", "--smoke-test", str(path)], env=environment, check=True, capture_output=True, timeout=30)
            rows.append(dict(bytes=size, operation="GUI first preview and paint, including 100 ms settle", process_wall_seconds=time.monotonic() - started))
            args.output.write_text(json.dumps(rows, indent=2) + "\n")
            print(f"Completed {size:,}-byte fixture", flush=True)
print(json.dumps(rows, indent=2))
