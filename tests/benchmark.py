#!/usr/bin/env python3
"""Dense-file benchmark of the automatic parallel FFT scan policy.

No global cache dropping or privileged operation. DONTNEED advises eviction of
this fixture only; report it as an advisory cold run, not proven uncached I/O.
Generate tones plus noisy low bits so transparent filesystem compression does
not turn a repeating zero fixture into a misleading storage benchmark.
CPU/wall ratios include all child-process threads, startup and JSON output.
"""
import argparse
import array
import datetime
import json
import math
import os
import pathlib
import platform
import random
import resource
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--cli", type=pathlib.Path, required=True)
parser.add_argument("--gui", type=pathlib.Path, required=True)
parser.add_argument("--output", type=pathlib.Path, required=True)
parser.add_argument("--max-bytes", type=int, default=40_000_000_000)
parser.add_argument("--fft-sizes", type=int, nargs="+", default=[4096, 65536])
parser.add_argument("--repeats", type=int, default=3, help="Warm runs per workload")
args = parser.parse_args()
if args.max_bytes < 400_000_000 or args.repeats < 1:
    parser.error("--max-bytes must be at least 400000000 and --repeats at least 1")
if any(size < 256 or size > 1048576 or size & (size - 1) for size in args.fft_sizes):
    parser.error("FFT sizes must be powers of two from 256 to 1048576")
cli, gui = args.cli.resolve(), args.gui.resolve()
rows = []
cpu_model = next(line.split(":", 1)[1].strip() for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name"))
report = dict(
    recorded_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    cli_version=subprocess.check_output([str(cli), "--version"], text=True).strip(),
    gui_version=subprocess.run([str(gui), "--version"], env=dict(os.environ, QT_QPA_PLATFORM="offscreen"),
        capture_output=True, text=True, check=True).stdout.strip(),
    host=dict(platform=platform.platform(), cpu_model=cpu_model, logical_cpus=os.cpu_count(),
        allowed_cpus=sorted(os.sched_getaffinity(0)), memory_bytes=os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")),
    settings=dict(kind="complex", encoding="signed", bits=16, endian="little", order="IQ",
        sample_rate=100_000_000, window="hann", overlap=50, scale="spectrum",
        remove_dc=False, conjugate=False, fft_sizes=args.fft_sizes, warm_repeats=args.repeats,
        max_bytes=args.max_bytes, fixture_seed=42, fixture_block_frames=262144),
    method="Automatic analysis workers; each FFTW transform is single-threaded. One fixture-advised cold run then repeated warm runs per workload. CLI launches use a small shell parent to avoid inheriting Python's larger peak RSS. CPU seconds and disk input blocks are child-process rusage deltas including the waited-for CLI; CPU/wall uses complete process wall time, including the shell, startup and JSON serialization. GUI smoke launches are offscreen and include a 100 ms settle delay; cache state is not controlled for GUI runs.",
    results=rows,
)
build_info = cli.parent / "BUILD-INFO.json"
if build_info.is_file():
    report["build_info"] = json.loads(build_info.read_text())
args.output.parent.mkdir(parents=True, exist_ok=True)
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
            for fft in args.fft_sizes:
                workloads = [("spectrum", frames - fft, frames)]
                if size <= 4_000_000_000:
                    workloads.append(("average", 0, frames))
                for command, start, end in workloads:
                    for run in range(args.repeats + 1):
                        cache = "advised_cold" if run == 0 else "warm"
                        if cache == "advised_cold":
                            os.posix_fadvise(file.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                        usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
                        started = time.monotonic()
                        # A fresh shell forks the CLI from its small address space.
                        # Direct Python launches can inflate getrusage peak RSS;
                        # the explicit exit prevents the shell from execing the CLI.
                        result = subprocess.run(["sh", "-c", '"$@"\nexit "$?"', "benchmark", str(cli), command,
                            "--file", str(path), "--start", str(start), "--end", str(end),
                            "--kind", "complex", "--encoding", "signed", "--bits", "16", "--endian", "little", "--order", "IQ",
                            "--sample-rate", "100000000", "--fft", str(fft), "--window", "hann", "--overlap", "50", "--scale", "spectrum"],
                            capture_output=True, text=True, check=True, timeout=300)
                        wall_seconds = time.monotonic() - started
                        usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
                        cpu_seconds = usage_after.ru_utime + usage_after.ru_stime - usage_before.ru_utime - usage_before.ru_stime
                        data = json.loads(result.stdout)
                        power = data["power"]
                        peak = max(range(len(power)), key=power.__getitem__)
                        assert abs(data["frequencies_hz"][peak] - 6_250_000) < 1
                        tone_power_dbfs = 10 * math.log10(power[peak])
                        assert abs(tone_power_dbfs + 6.020599913) < 0.1
                        assert data["complete_pass"]
                        if command == "average":
                            assert int(data["valid_windows"]) == 1 + (frames - fft) // (fft // 2)
                            assert int(data["invalid_windows"]) == int(data["boundary_windows"]) == 0
                        rows.append(dict(bytes=size, allocated_bytes=path.stat().st_blocks * 512, operation=command,
                            fft=fft, cache=cache, repeat=run, elapsed_seconds=data["elapsed_seconds"], process_wall_seconds=wall_seconds,
                            cpu_seconds=cpu_seconds, cpu_wall_ratio=cpu_seconds / wall_seconds,
                            disk_input_blocks=usage_after.ru_inblock - usage_before.ru_inblock, peak_rss_kib=data["peak_rss_kib"],
                            throughput_MB_s=(size / 1e6 / data["elapsed_seconds"]) if command == "average" else None,
                            valid_windows=int(data["valid_windows"]) if command == "average" else 1,
                            tone_frequency_hz=data["frequencies_hz"][peak], tone_power_dbfs=tone_power_dbfs,
                            fixture_extension_seconds=write_seconds))
                    args.output.write_text(json.dumps(report, indent=2) + "\n")
                    print(f"Completed {size:,} bytes / FFT {fft:,} / {command}", flush=True)
            environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=str(path.parent / "config"))
            for run in range(1, args.repeats + 1):
                started = time.monotonic()
                subprocess.run([str(gui), "--accept-defaults", "--smoke-test", str(path)], env=environment, check=True, capture_output=True, timeout=30)
                rows.append(dict(bytes=size, operation="GUI first preview and paint, including 100 ms settle", repeat=run,
                    process_wall_seconds=time.monotonic() - started))
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            print(f"Completed {size:,}-byte fixture", flush=True)
