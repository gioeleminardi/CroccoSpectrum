# Architecture and maintenance guide

## Reading order

1. `recording/Recording.h`: sample format, descriptor, closed-open frame ranges,
   read-only file ownership, checked/bounded block access.
2. `recording/Recording.cpp`: byte decoding and binary16 handling. `pread` avoids
   mutable seek state; fstat/path identity checks detect ordinary source changes.
3. `dsp/Spectrum.h/.cpp`: one reusable FFTW plan/buffer set per job, periodic
   windows, real/complex outputs, and power normalization.
4. `app/Analysis.h/.cpp`: bounded previews, exact window traversal, envelopes,
   progress/cancellation checkpoints, transactional exports.
5. `app/AnalysisController.h/.cpp`: one worker, one pending request, immutable
   snapshots, generation checks, worker-only byte-budgeted LRU cache.
6. `app/Session.h/.cpp` and `recording/Metadata.cpp`: schemas, decimal indices,
   atomic persistence and the explicit SigMF subset.
7. `ui/MainWindow.cpp`: commands, controls, shared selection, restoring state;
   `ui/Plots.cpp`: paint-only presentation and cursor/navigation interactions.

There is no dynamic plugin framework or backend registry. The GUI library
depends on the core, and the headless CLI/tests link only to Qt Core/FFTW.

## Units and numerical contract

Every range is in sample **frames**, `[begin,end)`. Complex int16 has four bytes
per frame. Byte offsets and frame counts use 64-bit integers. Coordinates use
local relative differences before conversion to double whenever possible.

For unnormalized forward FFT `X = FFT(x*w)`:

```text
Spectrum power = |X|² / |sum(w)|²
PSD            = |X|² / (Fs * sum(w²))
ENBW           = Fs * sum(w²) / |sum(w)|²
Bin spacing    = Fs / N
Window time    = N / Fs
Row hop time   = H / Fs
```

Real-input interior bins are doubled; DC/Nyquist are unchanged. Complex results
are fftshifted. Averages use the arithmetic mean of linear powers, accumulated
online to avoid unbounded arrays or overflowing sums. Log conversion happens
only when presenting/exporting dB. Zero power exports as negative infinity.

Input NaN/Inf or nonrepresentable resulting power invalidates a window. Invalid
windows and capture-boundary windows are excluded and counted. Finite clipping
is retained as actual recorded data and reported separately. Integer ADC widths
not equal to the stored scalar width need explicit format support; never infer
ADC full scale from the device model.

Double-precision analysis cannot preserve every 64-bit integer step. Original
byte export is exact. The independent SciPy oracle explicitly matches window,
hop, scaling, detrending, and sidedness, instead of relying on library defaults.

## Threads and ownership

Each `AnalysisController` runs recording/DSP jobs on its own `std::jthread`.
Submitting a new job atomically cancels that controller's running token and
replaces its pending job. The main window uses a separate controller for
one-window hover previews, so mouse movement does not cancel averages or exports.
The main window owns the shared frame cursor and freeze state for both time
plots. Waveform sample buckets select the corresponding waterfall row start.
An out-of-range waveform preview follows the selected frame using the hover
job's bounded waveform snapshot; exact full-selection waveforms are retained.
There is no file-sized queue. The GUI debounces navigation edits and invalidates
its current generation immediately. Worker signals are queued to GUI objects;
slots compare their generation before applying any result.

The controller joins its worker before its QObject/cache members are destroyed.
FFTW planning/destruction is serialized with a process-wide mutex; execution
uses job-private arrays. FFTW internal threads are initially disabled to avoid
nested parallelism. A blocked filesystem call is not forcibly interrupted;
cancellation is cooperative between bounded reads/transforms.

Snapshots become read-only when emitted. Plot widgets neither mutate numerical
results nor read files. Palette changes recolor cached screen-scale data. A CSV
request owns a snapshot of the exact bins/settings so concurrent UI edits cannot
change an export midway.

## Bounds and cache identity

- Maximum individual recording read: 32 MiB.
- Ordinary decoded read block: 262,144 frames; raised only for a larger FFT.
- Preview FFT work: at most 128 windows / approximately 8 million input frames.
- Waterfall snapshots: at most 128 × 1,024 double-valued cells.
- Waveform snapshots: at most 2,048 envelope buckets; every sample in a covered
  bucket contributes to its extrema.
- Preview LRU: 64 MiB per controller, maintained only by its worker; the main
  window's analysis and hover caches together retain at most 128 MiB.

Cache keys include source identity, full descriptor, DSP settings, and range.
Identity checks size, inode/device, modification and change timestamps and path
replacement. This is ordinary mutation detection, not a cryptographic guarantee
against arbitrary same-size edits with manipulated timestamps. Do not claim it
is a verified source hash.

Full passes traverse every complete hop-aligned window and never use previews
as measurement input. Exact waterfall overviews and averages share one pass;
temporal/frequency maxima preserve events in covered windows but are not a
replacement for numeric bins or zoomed analysis. Capture boundaries are never
crossed by an accepted FFT window.

## Persistence and maintenance

Application settings/session schema version is 1. Treat unknown versions as a
recoverable error and preserve the original. Recording-specific settings never
silently become facts about a new raw file. Preferences save import defaults,
UI/DSP/view choices, and the application-produced Qt layout state.

When changing measurement behavior, first extend independent/reference fixtures,
then update comments, the user conventions, changelog, and feature/validation
status. When adding a parser, reject mandatory unsupported semantics instead of
displaying plausible but incorrect results. Benchmark changes to rendering,
decoding, FFT threading, or cache behavior.
