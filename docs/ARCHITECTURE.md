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

`app/UpdateChecker.h/.cpp` belongs to the GUI library and uses asynchronous
Qt Network requests, independently of analysis workers. Normal GUI launches
explicitly start daily checks; automation launches and tests do not. Update
settings live in application preferences, outside recording sessions. Release
selection requires stable numeric version tags and uploaded Linux packages and
checksums. Downloads and installation are handled through the user's browser.

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

Spectrum Shift-click measurements snap to bin centers and average the exact
linear bins between both endpoints, inclusive. This is mean spectral power,
not integrated band power; the result retains the current spectrum/PSD unit.
The mean is recomputed on completion, a new trace, or changed endpoints during
a Shift-drag edit, not on every paint. Band drags preserve the bin/row span and
clamp both endpoints together; marker drags may cross the other endpoint. A
marker hit takes priority over a band hit within six logical pixels. The normal
drag threshold distinguishes edits from replacement Shift-clicks. Waterfall
measurements use row-start frame timestamps and
subtract 64-bit indices before conversion to seconds. Duration is independent
of preview row skipping or overview aggregation, although those displays limit
event-boundary resolution. Plot measurement state is transient.

Waterfall measurement changes publish their ordered row-start frame bounds to
the time-selection fields without calling `setRange`. The separate average
button submits a snapshot of those bounds to the existing analysis worker,
using the same exclusive-end convention as the time panel. Pending gestures
and intervals shorter than one FFT window keep that button disabled.

Spectrum hover stores the pointer's plot position and paints a crosshair with
axis-coordinate labels using the current frequency range and power limits.
The status readout includes these coordinates alongside the existing exact
FFT-bin values. Pointer exit clears the crosshair; no additional FFT is needed.

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
replaces its pending job. The main window uses separate controllers for one-window hover previews, waterfall
viewport detail, and the whole-recording minimap. Navigation and hover do not cancel
averages or exports, and minimap scanning continues through viewport changes.
Progressive minimap snapshots are immutable copies, published about every 100 ms
as rows complete. A mutex-protected latest snapshot and one queued notification
coalesce GUI delivery, bounding the queue even while the GUI is occupied.
The main window owns the shared frame cursor and freeze state for both time
plots. Waveform sample buckets select the corresponding waterfall row start.
An out-of-range waveform preview follows the selected frame using the hover
job's bounded waveform snapshot; exact full-selection waveforms are retained.
Endpoint placement and editing temporarily suppress shared hover and freeze commands,
and invalidates queued hover results without changing the user's freeze state.
Completed frequency selections follow the displayed frame and recalculate
their mean; new analysis contexts clear plot measurements.
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
- Initial waterfall previews: at most 128 × 1,024 cells. Explicit exact overview
  requests use the same physical-pixel grid bounds as viewport detail.
- Waterfall viewport grids: physical plot pixels by default, or the row count
  chosen in Settings (128–4,096); exact overviews use the same choice. At most
  4,096 per axis and 4,194,304 double-valued cells (32 MiB). Automatic resolution
  limits FFT work to approximately eight million input frames. Explicit row
  choices raise that budget to at least one FFT per requested row, subject to
  the grid's memory bound; work remains cancellable. Small intervals scan every window into maxima;
  larger intervals are explicitly sampled. Frequency reduction uses the visible
  FFT bins, with exact bin-aligned raster bounds.
- Minimap: 1,024 × 192 cells (1.5 MiB), scanning every required window; it does
  not compute an implicit numeric average. Resizing does not restart this pass.
- Waveform snapshots: at most 2,048 envelope buckets; every sample in a covered
  bucket contributes to its extrema.
- Preview LRU: 64 MiB per controller, maintained only by its worker; the main
  window's analysis, hover, and viewport caches retain at most 192 MiB, plus a
  separate 4 MiB minimap cache. Raster images and active FFT buffers are additional.

Cache keys include source identity, full descriptor, DSP settings, and range. Waterfall keys also include the requested frequency
bounds, grid dimensions, explicit row density, and full-scan mode. Palette and color limits do not
change these numerical cache keys.

Each controller reports activity from submission until both its executing job
and pending request are gone, including cache hits, failures, and cancellation.
Ordered notifications arrive on the controller's QObject thread. The status-bar
spinner combines activity from the analysis, waterfall, and minimap controllers;
one worker finishing does not hide it while another is still processing. The
hover FFT controller is excluded so ordinary pointer movement stays quiet.

Identity checks size, inode/device, modification and change timestamps and path
replacement. This is ordinary mutation detection, not a cryptographic guarantee
against arbitrary same-size edits with manipulated timestamps. Do not claim it
is a verified source hash.

Partial imports are explicit (`allow_partial` in recording JSON, default false).
They freeze the sample count and data length at the complete frames available
when opened, discard future captures, and clip annotations to that range.
Their identity remains the opening identity as the file grows. Size checks
reject shrinkage below the opening file length and path checks reject
replacement; timestamp changes are allowed. This assumes a stable downloaded
prefix and does not detect in-place edits or preallocated missing regions.
Sessions retain the snapshot descriptor; reimporting the original file obtains
new samples and metadata. Sample exports reset partial mode because their
output is a completed recording. Import presets also reset it to false.

Full passes traverse every complete hop-aligned window and never use previews
as measurement input. Exact waterfall overviews and averages share one pass;
temporal/frequency maxima preserve events in covered windows but are not a
replacement for numeric bins or zoomed analysis. Capture boundaries are never
crossed by an accepted FFT window.

Waterfall viewport bounds are 64-bit frame ranges separate from the analysis
selection. Pixel mapping subtracts integer frame origins before conversion.
Measurements retain frame endpoints across density refreshes and band drags
preserve their frame span. Hover inspects the painted cell's representative FFT
start; maximum-aggregated cells remain visual summaries, not numeric measurements.
Retune/absolute-frequency labeling uses the actual waterfall viewport. PNG
metadata distinguishes visible bounds from the cached raster's source bounds.

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
