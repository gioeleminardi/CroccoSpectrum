# Performance evidence — 2026-10-06

Reference machine: AMD Ryzen 9 9950X3D (16 cores/32 threads), 32 GiB RAM,
Samsung 9100 PRO NVMe, local Btrfs, Fedora 44 KDE. Release application built
with GCC 13.3 on Ubuntu 24.04; bundled Qt 6.11.2 and FFTW 3.3.10. DSP uses one
worker and single-threaded FFTW. These results are measurements on this machine,
not guaranteed performance on other storage/CPUs or encodings.

Dense synthetic int16 complex I/Q at 100 MS/s, known +6.25 MHz tone plus noise.
Sizes are decimal bytes. The allocated sizes match the logical sizes: these
are dense fixtures, separate from the 100 GiB sparse address test. Every checked
spectrum/average finds the tone within one bin and power within 0.1 dB.
The complete machine-readable run is [benchmarks.json](benchmarks.json).

| Workload | Advised cold | Warm | Peak process RSS |
| --- | --- | --- | --- |
| Average 400 MB, Hann 4096, 50% overlap | 1.029 s / 388.5 MB/s | 1.012 s / 395.1 MB/s | ~22.5 MiB |
| Average 4 GB, same settings | 10.160 s / 393.7 MB/s | 10.007 s / 399.7 MB/s | ~22.5 MiB |
| Spectrum at end of dense 40 GB | 17 ms process wall time | 3.4 ms process wall time | ~20.2 MiB |
| GUI open/preview/paint, 400 MB | 138 ms total | — | See GUI stress run |
| GUI open/preview/paint, 4 GB | 131 ms total | — | See GUI stress run |
| GUI open/preview/paint, 40 GB | 150 ms total | — | See GUI stress run |

GUI timings include the smoke command's intentional 100 ms paint/layout settling
delay and are offscreen. The file cache was warmed before these GUI measurements.
The initial preview reads bounded, distributed windows, not the whole file.
There was no full 40 GB average measurement in this run.

“Advised cold” means `fsync` then `POSIX_FADV_DONTNEED` on this fixture only.
This is advisory eviction, not proof that every layer of the storage stack was
cold. The following complete pass warms the fixture for the warm measurement.
Whole-average throughput corresponds to about 0.97–1.00 recording seconds per
wall-clock second for this workload; arbitrary FFT lengths/overlaps and FP64
inputs need their own measurements.

A 60.071-second offscreen GUI mixed-operation run completed 499 cycles:
first preview 16 ms, seek-result p95 113 ms, peak RSS 73.1 MiB. The run alternates
FFTs/ranges/colors, replaces whole-file averages with local previews and checks
that the displayed result belongs to the latest range/settings. This is a short
stress test; it does not measure long-duration stability or native-desktop latency.

The largest FFT (1,048,576), single int16 complex window, took 36.8 ms of CLI
analysis time with 78.5 MiB peak RSS. Its independent tone/normalization test also
passes. GUI/cache memory for many largest-FFT snapshots is a separate workload.

CPU sampling with `perf` attributed about 27% of baseline scan samples to
`hypot`, 25% to decoding, and 5.5% to integer scale calculation. A guarded ordinary
power calculation and a portable signed-int16 decoder path improved the same
400 MB average from ~133 MB/s to ~395 MB/s. Extreme floating-point PSD retains
scaled magnitude arithmetic and has dedicated regression cases. No fast-math,
GPU, architecture-specific SIMD requirement, or approximate averaging was added.

The reference system uses NVMe storage; a separate sustained sequential-read
ceiling was not measured. Network/HDD blocked-I/O cancellation, native interaction
p95, recorder-produced files, and long-duration stability were not measured in
this run.

## Waterfall upgrade regression checks — 2026-10-09

Fedora 44 Release build, GCC 16.2.1, Qt 6.11.2, FFTW 3.3.10; offscreen rendering.
The updated soak harness uses a dense 400 MB synthetic I/Q fixture, waits for
its initial whole-recording minimap, and then changes FFTs, analysis ranges,
colors, and independent waterfall viewports. FFTs include 1,048,576 points.

A separate 10.198-second run completed 46 mixed cycles: first preview **12 ms**,
initial minimap complete **1,179 ms**, viewport-result p95 **105 ms**, selection
preview p95 **126 ms**, and peak process RSS **408.8 MiB**. The largest FFT and
concurrent analysis workers make this workload different from the earlier
60-second run above. Cache/grid limits bound retained data; active FFT buffers
and allocator retention contribute additional process memory.

The existing dense-file benchmark also passed tone/power checks for 400 MB and
4 GB files. GUI initial preview/paint process times were **160 ms** and **143 ms**,
including the intentional 100 ms settling delay. These are local regression
measurements, not native-desktop latency or long-duration stability evidence.

Reproduce the new interaction workload with:

```sh
QT_QPA_PLATFORM=offscreen RF_SOAK_SECONDS=10 build/release/rf-soak-test
```

## Parallel FFT windows — 2026-10-09

Same Ryzen 9 9950X3D reference machine, local NVMe/Btrfs, Fedora 44. Release
working tree based on `ac1bd26`, GCC 16.2.1, Qt 6.11.2, FFTW 3.3.10. Input was
the local `mega.dat`: 4,000,000,000 bytes / one billion signed int16 complex
I/Q frames at 100 MS/s (10 seconds). Hann windows, 50% overlap, no DC removal
or conjugation. Every measured pass reported zero disk input blocks: these are
warm-cache computation measurements, excluding GUI painting and file-open latency.

| Workload | Serial | Parallel |
| --- | --- | --- |
| Exact minimap, whole 4 GB, FFT 65,536, 192 columns × 1,024 rows | 22.22 s | 4.88 s; final-build follow-up 4.31 s |
| Sampled viewport, whole file, FFT 65,536, 1,024 columns × 4,096 rows | 3.114 s | 0.655 s; final-build follow-up 0.641 s |
| Automatic viewport, whole file, FFT 65,536, 1,024 columns × 128 actual rows | 0.104 s | 0.025 s |
| Exact average, first 400 MB, FFT 4,096 | 1.197 s | 0.233 s |
| Exact average, first 400 MB, FFT 65,536 | 2.245 s | 0.490 s |
| Exact overview + average, first 400 MB, FFT 65,536, default 1,024 × 128 grid | 2.508 s | 0.484 s |

A temporary headless C++ harness called `analyzeWaterfall` and `analyzeAverage`
directly. Exact minimap calls included progressive immutable-snapshot copying.
The original serial `Analysis.cpp` and the parallel implementation used the same
recording/DSP code, compiler, and FFTW library. The machine-readable results and
scope are in [fft-benchmarks.json](fft-benchmarks.json). These are individual local
passes and follow-ups, not statistical latency guarantees or cold-storage tests.

The automatic policy selected six workers for FFT 65,536 and seven for FFT 4,096.
Final-build peak RSS was 106.7 MiB for the full exact minimap, 83.8 MiB for the
4,096-row viewport, and 92–106.5 MiB for the prefix average/overview jobs. Parallel
processing uses more memory and aggregate CPU time than the serial implementation;
worker-local buffers and one bounded pending batch per worker keep storage
independent of recording length. Short jobs stay serial, and the memory estimate
also keeps the largest FFT serial. Original numerical calculations and window-order
reductions are retained; no approximation or reduced overlap was introduced.

A separate 10.154-second offscreen GUI mixed-operation run completed 45 cycles:
first preview 16 ms, initial minimap complete 323 ms, viewport-result p95 104 ms,
seek-result p95 127 ms, and peak RSS 517.5 MiB. This workload includes concurrent
controllers, caches, and the 1,048,576-point FFT; its RSS is not the headless
scan's worker-memory estimate. It is a short regression check, not evidence of
native-desktop latency or long-duration stability.
