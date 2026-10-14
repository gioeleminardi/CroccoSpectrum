# Performance evidence

## Current threaded analysis — 2026-10-14

Reference machine: AMD Ryzen 9 9950X3D (16 cores/32 threads), 32 GiB installed RAM
(30.94 GiB visible to Linux), Samsung 9100 PRO NVMe, local Btrfs with zstd
compression, Fedora 44. Release build `0.4.1-dev.local.g69c4ea4.dirty`, GCC 16.2.1,
system Qt 6.11.2 and FFTW 3.3.10. The full build identity, host, settings and
individual measurements are in [benchmarks.json](benchmarks.json).

Large analysis passes now distribute independent FFT windows across bounded
workers, with reductions consumed in window order. Each FFTW transform remains
single-threaded. For these long average workloads on this host, the policy in
`src/app/Analysis.cpp` selects seven workers at FFT 4,096 and six at FFT 65,536.
Short jobs remain serial; larger FFT buffers can lower the worker count. These
policy counts are derived from the implementation, not a thread-count profiler.

### Exact whole-file averages

Dense synthetic signed int16 complex I/Q at 100 MS/s, little-endian, known
+6.25 MHz tone at approximately −6.02 dBFS plus seeded noise. Hann windows,
50% overlap, spectrum scaling, no DC removal or conjugation. Each workload has
one fixture-advised cold pass followed by **three warm repeats**. Warm columns
show the median and the observed range; throughput uses logical file bytes
divided by the median CLI elapsed time. Peak RSS is the maximum across all four
passes of that workload.

| Whole-file average | Advised cold | Warm median (range) | Warm throughput | Peak process RSS |
| --- | --- | --- | --- | --- |
| 400 MB, FFT 4,096 | 0.340 s | **0.309 s** (0.210–0.314 s) | **1,296 MB/s** | 109.8 MiB |
| 4 GB, FFT 4,096 | 2.320 s | **2.865 s** (2.066–2.969 s) | **1,396 MB/s** | 122.4 MiB |
| 400 MB, FFT 65,536 | 0.453 s | **0.444 s** (0.435–0.495 s) | **901 MB/s** | 110.2 MiB |
| 4 GB, FFT 65,536 | 5.104 s | **4.299 s** (4.228–5.062 s) | **930 MB/s** | 109.8 MiB |

Warm average runs reported zero disk input blocks. Their median process CPU/wall
ratios range from **5.76 to 6.64**, confirming execution across multiple CPUs;
these ratios are average CPU use, not worker counts. CPU seconds include all
threads and are measured separately from elapsed time. The CLI elapsed timer
excludes file opening and final JSON serialization; complete process wall time
also includes the small shell launcher, startup and output serialization.

The harness launches the CLI from a small shell parent before reading its
`getrusage` peak RSS. Direct launches from Python can inherit the launcher's
larger pre-exec memory peak, especially after parsing large numeric arrays.
The reported values include the analyzer's libraries, results, coordinator and
worker buffers; the 128 MiB worker-storage estimate is not a process RSS limit.

### Bounded large-file access and GUI startup

Three warm tail-spectrum repeats and three offscreen GUI smoke launches per
fixture. Spectrum rows measure complete process wall time; GUI cache state is
uncontrolled. GUI times include the intentional 100 ms layout/paint settling
delay and the first preview, rather than a completed whole-recording minimap.

| Workload | Median process wall time | Observed range |
| --- | --- | --- |
| Tail spectrum of 40 GB, FFT 4,096 | 4.04 ms | 3.89–4.42 ms |
| Tail spectrum of 40 GB, FFT 65,536 | 20.75 ms | 20.35–20.87 ms |
| GUI open/preview/paint, 400 MB | 153 ms | 151–155 ms |
| GUI open/preview/paint, 4 GB | 154 ms | 153–154 ms |
| GUI open/preview/paint, 40 GB | 156 ms | 149–285 ms |

The 40 GB tail spectra used up to 16.4 MiB RSS at FFT 4,096 and 20.3 MiB at
FFT 65,536. Their median CLI elapsed times were 0.453 ms and 4.281 ms respectively;
process startup, file opening and JSON output account for the additional wall
time. No full 40 GB average was run. Fixtures were written densely, synced and
removed on completion; they are separate from sparse addressing tests.

### Reproduce and interpret the results

```sh
cmake --preset release -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build --preset release --parallel 2
python3 tests/benchmark.py --cli build/release/croccospectrum-cli \
  --gui build/release/croccospectrum --output build/benchmarks.json \
  --fft-sizes 4096 65536 --repeats 3
```

The default maximum fixture is 40 decimal GB. Add `--max-bytes 4000000000` to
limit the run to 4 GB. The output is a JSON object with metadata and a `results`
array, including tone frequency/power, valid-window counts, CPU/wall, disk input
blocks and RSS for each CLI pass. Each CLI pass checked tone sign/power and
complete coverage; averages reported no invalid or capture-boundary windows.

“Advised cold” means `fsync` followed by `POSIX_FADV_DONTNEED` on this fixture
before each cold workload. It is advisory eviction, not proof of uncached I/O.
There is no global cache drop, CPU pinning, frequency control or exclusive host
reservation. The observed timing ranges reflect this ordinary desktop run;
cold runs can be faster than some warm runs. No other project performance test
ran concurrently with the recorded matrix.

These results cover synthetic int16 recordings, two FFT lengths and one machine.
They do not establish native-desktop latency, long-duration stability, network/HDD
cancellation behavior, exact minimap/overview latency, or the sustained storage
ceiling.
