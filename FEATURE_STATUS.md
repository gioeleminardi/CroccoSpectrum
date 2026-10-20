# Feature status

Updated: 2026-10-20. Target: C++20 + Qt 6 Widgets on amd64 Linux.
Planned release: CroccoSpectrum 0.6.0 (see `CMakeLists.txt`).

This table describes the current implementation. Test environments, evidence,
and coverage limitations are recorded in [Validation](docs/VALIDATION.md).

| Feature | Status | Notes |
| --- | --- | --- |
| Desktop application and command-line tools | Implemented | Native executable and CLI use croccospectrum / croccospectrum-cli |
| Application icon and logo | Implemented | Embedded Qt resources; window/Linux launcher icon; full logo in About |
| Preference compatibility after rename | Implemented | Reads legacy preferences on first launch, retains original file; session formats unchanged |
| Raw real / single complex I/Q stream | Implemented | One real-valued stream or one interleaved complex stream per recording |
| WAV / AIFF / AIFC import | Implemented subset | PCM 8/16/24/32 and float32/64; explicit channel mapping; header rate including fractional AIFF; normalized float64 temporary storage |
| Numeric ASCII / CSV / TSV import | Implemented subset | Explicit columns, real/IQ kind, separator and skipped lines; unquoted numeric fields; bounded rows; float64 temporary storage |
| gzip / bzip2 raw import | Implemented | Bounded streaming decompression; concatenated members; checksum/truncation rejection; temporary storage with cancellation and cleanup |
| IQ and QI order, both byte orders | Implemented | Import interpretation must match the writer |
| Signed/unsigned 8/16/24/32/64-bit | Implemented | Packed 24-bit; double analysis precision for 64-bit inputs |
| IEEE FP16/FP32/FP64 | Implemented | FP16 conversion needs no hardware FP16 support |
| Large recordings, bounded positional reads | Implemented | No whole-file allocation; 32 MiB maximum read call |
| Partial / growing recording import | Implemented | Opt-in import checkbox / CLI --partial; complete frames at open; append-only prefix; reopen for more data; sessions retain snapshot bounds |
| Positive/fractional sample rates | Implemented | Derived floating arithmetic must remain representable |
| Default spectrum and waterfall | Implemented | Physical-pixel viewport detail; visible FFT-bin reduction; large views explicitly sampled |
| Waterfall minimap | Implemented | Progressive exact whole-recording maxima; move/resize viewport independently of analysis range |
| Spectrum hover crosshair | Implemented | Pointer frequency/power axis labels; zoom, absolute frequency and dBFS/Hz supported; exact FFT-bin readout retained |
| Waterfall hover spectrum | Implemented | One-row highlight; exact FFT at row start; initial frame 0; zoom/pan retained; exports follow displayed frame |
| Shared waveform/waterfall cursor and freeze | Implemented | Vertical waveform bar; hover either plot; click either to freeze/resume; waterfall time navigation resets waveform bounds |
| Shift-click spectrum and waterfall measurements | Implemented | Two endpoints; FFT-bin frequency difference and integrated band power in dBFS; row-start duration with exact frame differences; Shift-drag band/markers to move/resize with live updates; Escape clears |
| Signal/noise band power and SNR | Implemented | Explicit bands on displayed FFT or completed average; ENBW-corrected integration; bandwidth-normalized noise subtraction; CSV/PNG metadata and CLI band bounds |
| Automatic spectral peaks | Implemented | Spectrum/average context menu; twelve strongest local maxima in visible/selected range; bin power/frequency, approximate sub-bin estimates and two-peak differences; weak/nearby/boundary limits |
| Average waterfall selection | Implemented | Markers update draft frame fields without applying; exact average over ordered row-start bounds; requires a completed interval with one FFT window |
| User-selected power-of-two FFT | Implemented | 256 through 1,048,576 |
| Window, overlap, DC removal, conjugation | Implemented | Hann, rectangular, Hamming, Blackman-Harris |
| dBFS / PSD and real one-sided scaling | Implemented | Explicit full-scale convention; no calibrated dBm |
| Exact selected/whole-file spectral average | Implemented | Linear mean; invalid/boundary windows counted |
| Interval max hold | Implemented | Separate result from average |
| Full exact waterfall overview | Implemented | Explicit full pass; bounded temporal/frequency maxima plus average |
| Parallel FFT scans | Implemented | Automatic bounded workers for averages, exact overviews/minimaps and larger previews/waterfalls; ordered reductions; short jobs stay serial |
| Waveform and exact min/max envelopes | Implemented | Independent time zoom/pan down to single samples; connected I/Q traces and markers; bounded preview and explicit exact scan of the visible waveform interval |
| Color minimum/maximum, auto range, palettes | Implemented | Recolors snapshots without FFT work; Baudline spans black to #00F9AB |
| Docked/floating analysis panels | Implemented | Qt docking, persisted workspace |
| Sample/time seek, time and frequency zoom/pan | Implemented | 64-bit sample text fields; linked frequency views |
| Keyboard shortcuts cheatsheet | Implemented | Help → Keyboard shortcuts / F1; live menu bindings, plot gestures and keyboard navigation; stays open while working |
| Cursor readouts and shared time selection | Implemented | Exact indices, sample-count time, optional UTC at millisecond resolution |
| Optional capture UTC and retune-aware frequency axes | Implemented | Unknown/gapped UTC is not inferred; mixed-frequency aggregates remain baseband |
| Preferences and reproducible sessions | Implemented | Versioned, atomic JSON; indices stored as decimal strings |
| Recent files | Implemented | File → Open recent; ten recordings/sessions; persistent history; metadata source paths retained; clear history |
| Update notifications | Implemented | Stable GitHub releases with complete Linux packages/checksums; checks on each startup and daily while running with opt-out; notification once per app session; manual Help action; website download/install; quiet automatic failures |
| SigMF core recording import | Implemented subset | Single stream; required extensions and embedded later headers rejected |
| Bookmarks/notes | Implemented | Persist in sessions |
| Numeric CSV, plot PNG, raw range export | Implemented | PNG metadata; byte-exact raw/decompressed bytes or converted float64 + sidecar; cancellable worker writes; refuses overwrite |
| Background cancellation and stale-result protection | Implemented | Separate analysis and hover controllers, each with one worker and one replaceable pending request |
| Bounded in-memory preview cache | Implemented | 64 MiB LRU per controller, interpretation/DSP/range/source identity keys |
| Headless CLI | Implemented | Inspection, spectra, averages, waveform, exports |
| GitHub automatic builds and downloadable artifacts | Implemented | All targets in Release/Debug/ASAN; separate AppImage and portable tarball downloads, sources, checksums/reports; runner/retention details in docs/CI.md |
| Self-contained portable folder / AppImage | Implemented | Bundled libraries/plugins/font; tar folder and no-FUSE AppImage extraction path |
| Ubuntu 22.04/24.04/26.04 and Fedora 44 package checks | Smoke checks passed | Clean offline containers; Ubuntu 22.04 headless X11 at normal/doubled scale; coverage in docs/VALIDATION.md |
| Large-file benchmarks | Measured locally | Dense 400 MB / 4 GB / 40 GB; benchmark report records scope and hardware |
| Numerical verification | Passed | 397 independent NumPy/SciPy checks, including integrated band power/SNR across all windows; core and GUI integration checks; dated scope in Validation |
| Sanitizers and parser stress | Passed | ASAN/UBSAN; TSAN with instrumented Qt; 301 metadata stress cases + decoder stress |
