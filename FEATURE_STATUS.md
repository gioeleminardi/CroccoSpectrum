# Feature status

Updated: 2026-10-06. Target: C++20 + Qt 6 Widgets on amd64 Linux.
Application: CroccoSpectrum 0.1.1.

This table describes the current implementation. Test environments, evidence,
and coverage limitations are recorded in [Validation](docs/VALIDATION.md).

| Feature | Status | Notes |
| --- | --- | --- |
| Desktop application and command-line tools | Implemented | Native executable and CLI use croccospectrum / croccospectrum-cli |
| Application icon and logo | Implemented | Embedded Qt resources; window/Linux launcher icon; full logo in About |
| Preference compatibility after rename | Implemented | Reads legacy preferences on first launch, retains original file; session formats unchanged |
| Raw real / single complex I/Q stream | Implemented | One real-valued stream or one interleaved complex stream per recording |
| IQ and QI order, both byte orders | Implemented | Import interpretation must match the writer |
| Signed/unsigned 8/16/24/32/64-bit | Implemented | Packed 24-bit; double analysis precision for 64-bit inputs |
| IEEE FP16/FP32/FP64 | Implemented | FP16 conversion needs no hardware FP16 support |
| Large recordings, bounded positional reads | Implemented | No whole-file allocation; 32 MiB maximum read call |
| Positive/fractional sample rates | Implemented | Derived floating arithmetic must remain representable |
| Default spectrum and waterfall | Implemented | Large waterfall views are explicitly sampled previews |
| User-selected power-of-two FFT | Implemented | 256 through 1,048,576 |
| Window, overlap, DC removal, conjugation | Implemented | Hann, rectangular, Hamming, Blackman-Harris |
| dBFS / PSD and real one-sided scaling | Implemented | Explicit full-scale convention; no calibrated dBm |
| Exact selected/whole-file spectral average | Implemented | Linear mean; invalid/boundary windows counted |
| Interval max hold | Implemented | Separate result from average |
| Full exact waterfall overview | Implemented | Explicit full pass; bounded temporal/frequency maxima plus average |
| Waveform and exact min/max envelopes | Implemented | Full-selection scan is an explicit command |
| Color minimum/maximum, auto range, palettes | Implemented | Recolors snapshots without FFT work |
| Docked/floating analysis panels | Implemented | Qt docking, persisted workspace |
| Sample/time seek, time and frequency zoom/pan | Implemented | 64-bit sample text fields; linked frequency views |
| Cursor readouts and shared time selection | Implemented | Exact indices, sample-count time, optional UTC at millisecond resolution |
| Optional capture UTC and retune-aware frequency axes | Implemented | Unknown/gapped UTC is not inferred; mixed-frequency aggregates remain baseband |
| Preferences and reproducible sessions | Implemented | Versioned, atomic JSON; indices stored as decimal strings |
| SigMF core recording import | Implemented subset | Single stream; required extensions and embedded later headers rejected |
| Bookmarks/notes | Implemented | Persist in sessions |
| Numeric CSV, plot PNG, raw range export | Implemented | PNG metadata; byte-exact raw + sidecar; cancellable worker writes; refuses overwrite |
| Background cancellation and stale-result protection | Implemented | One worker; one replaceable pending request |
| Bounded in-memory preview cache | Implemented | 64 MiB LRU, interpretation/DSP/range/source identity keys |
| Headless CLI | Implemented | Inspection, spectra, averages, waveform, exports |
| GitHub/Gitea automatic builds and downloadable artifacts | Implemented | All targets in Release/Debug/ASAN; packages, sources/checksums, reports; runner/retention details in docs/CI.md |
| Self-contained portable folder / AppImage | Implemented | Bundled libraries/plugins/font; tar folder and no-FUSE AppImage extraction path |
| Ubuntu 24.04/26.04 and Fedora 44 package checks | Smoke checks passed | Clean offline containers; Fedora native X11/Wayland smoke; coverage in docs/VALIDATION.md |
| Large-file benchmarks | Measured locally | Dense 400 MB / 4 GB / 40 GB; benchmark report records scope and hardware |
| Numerical verification | Passed | 167 independent NumPy/SciPy checks; core and GUI integration checks |
| Sanitizers and parser stress | Passed | ASAN/UBSAN; TSAN with instrumented Qt; 301 metadata stress cases + decoder stress |
