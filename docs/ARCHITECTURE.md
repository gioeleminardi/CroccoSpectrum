# Architecture and maintenance guide

## Reading order

1. `recording/Recording.h`: sample format, descriptor, closed-open frame ranges,
   read-only file ownership, checked/bounded block access.
2. `recording/Recording.cpp`: byte decoding and binary16 handling. `pread` avoids
   mutable seek state; fstat/path identity checks detect ordinary source changes.
3. `dsp/Spectrum.h/.cpp`: one reusable FFTW plan/buffer set per FFT worker, periodic
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

Spectrum Shift-click measurements snap to bin centers and integrate every exact
linear bin between both endpoints, inclusive. For bin spacing `df = Fs/N`:

```text
Pband = sum(PSD[k] * df) = sum(spectrum_power[k] * df/ENBW)
Band power (dBFS) = 10*log10(Pband)
Band bandwidth B = included bin count * df
Noise density D  = Pnoise/Bnoise
Noise in signal bandwidth N = D*Bsignal
Net signal S = max(Psignal_band - N, 0)
SNR (dB) = 10*log10(S/N)
```

Band power uses normalized mean-square units, independent of the spectrum/PSD
display scale. A single-bin selection has bandwidth `df`, even though its marker
separation is zero. This discrete sum includes real DC/Nyquist bins once with
their existing one-sided scaling; no trapezoidal endpoint weighting is applied.
Full-band integration equals window-weighted mean-square power. Capture the
window's leakage/main lobe in a tone's band; a narrow selection can exclude tone
power. Window weighting can change a finite noise realization's measured power.
The spectrum/PSD conversion follows the documented
[SciPy periodogram scaling](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.periodogram.html).

The completed selected band also reports 99% occupied bandwidth (OBW): exclude
0.5% of its integrated linear power from each end, following the equal-tail
definition described in [MathWorks OBW](https://www.mathworks.com/help/signal/ref/obw.html).
This is an equal-tail interval, not a search for the narrowest interval.
For the discrete-to-continuous edge convention, treat each included bin as a
constant-density cell `[f[k]-df/2, f[k]+df/2]` containing its full integrated
power `PSD[k]*df` (or `spectrum_power[k]*df/ENBW`). Accumulate each tail from its
own end and interpolate linearly inside its crossing cell. If a threshold lands
exactly on a zero-power gap, retain the outer edge of that gap. The bandwidth is
`upper_hz - lower_hz`; one populated bin therefore gives `0.99*df`, not zero.
The cells at FFT boundaries, including real DC/Nyquist, retain their full `df`
width without clipping or half weighting, consistent with band-power integration.
Thus OBW edges can extend up to half a bin outside the selected bin-center bounds
or FFT axis. The two pink markers and lower/upper/width readouts use these edges;
markers outside the current viewport are clipped for painting only.

OBW includes all measured noise without a noise-floor threshold or subtraction,
independently of assigned SNR bands. Zero total power, negative/nonfinite bins,
or unrepresentable integration/edges report unavailable. It uses exact displayed
FFT bins or the completed linear-power average, never dB or pixel maxima.
Max-hold OBW describes the composite trace, not necessarily one emission.
Band choice, FFT resolution, window leakage and noise change the result;
cell interpolation does not improve resolving power or calibrate the recording.

Right-click assigns a completed selection to a signal or noise band. Assignments
copy the bin bounds; subsequent selection edits require reassignment to change
them. Both bands are recomputed from the same single complete FFT window or
completed interval average, never waterfall pixels. Interval averages integrate
the arithmetic mean of linear powers over valid windows. Max-hold may integrate
bins from different windows and is labeled as such; SNR is unavailable on it.
SNR assumes the noise band's PSD represents additive noise in the signal band.
Use a noise-only band away from signals, leakage, DC and real Nyquist endpoints;
those endpoint noise bins are not doubled like the real interior bins. The two
bands must not overlap, and their bandwidths may differ. Subtract the normalized
noise in linear power before taking logarithms. A nonpositive net signal reports
negative infinity; positive signal with zero measured noise reports positive
infinity, not a finite noise-floor estimate. Both zero, invalid bins, and overlap
report SNR unavailable. Zero integrated power is negative infinity. JSON encodes
infinite logarithmic results as `"-inf"`/`"inf"`, retaining zero linear power.

Measurements are recomputed on completion, a new trace, or changed endpoints
during a Shift-drag edit, not on every paint. Band drags preserve the bin span and
clamp both endpoints together; marker drags may cross the other endpoint. A
marker hit takes priority over a band hit within six logical pixels. The normal
drag threshold distinguishes edits from replacement Shift-clicks. Waterfall
measurements use row-start frame timestamps and subtract 64-bit indices before
conversion to seconds. Duration is independent of preview row skipping or
overview aggregation, although those displays limit event-boundary resolution.
Plot measurement state is transient; Escape also clears signal/noise bands.

The opt-in spectral-peaks dialog uses the original displayed FFT, average or
max-hold bins, never pixel maxima. It retains at most twelve local maxima in
descending bin-power order (ties retain frequency order). Search bounds include
bin centers; neighbors outside the visible/selected range still determine maxima
and interpolation. A two-bin equal maximum has one representative; wider flat
tops are omitted. Peak labels/readouts update on trace, range and selection
changes, outside paint handlers, and disappear when the dialog closes.

Sub-bin estimates fit a parabola through three log-power bins, using
`offset = (left - right) / (2 * (left - 2*peak + right))`, bounded to half a bin.
See [Quadratic interpolation](https://www.dsprelated.com/freebooks/sasp/Quadratic_Interpolation_Spectral_Peaks.html)
and [window-dependent bias](https://www.dsprelated.com/freebooks/SASP/Bias_Parabolic_Peak_Interpolation.html).
The original bin frequency and power are retained; power is not interpolated.
Estimates are unavailable at FFT edges, with nonpositive/nonfinite neighbors,
invalid curvature, fewer than five valid search bins, or peak power less than
15 dB above the median search-bin power. Another local maximum within 2 bins
(rectangular), 4 (Hann/Hamming), or 8 (Blackman-Harris), and within 20 dB of the
candidate, suppresses its estimate even outside the search range. These guards
are heuristics: weak or merged carriers, noise and sidelobes cannot be reliably
classified from three bins. The dialog always states that interpolation gives
neither additional resolving power nor an accuracy guarantee. Differences use
the original numerical values, with the lower table row minus the upper row.
Average/max-hold estimates assume stationary carriers; max-hold bins may come
from different windows, so a fitted peak need not describe one recorded carrier.

Other UI frequency values round to whole Hz before scaling to compact kHz/MHz/GHz
labels. This includes axis ticks, cursors, band endpoints/widths, bin spacing and
ENBW. Time readouts select s/ms/µs/ns by magnitude, including window/hop durations,
recording/import summaries and waterfall/waveform times. The shared UI formatter
in `src/ui/DisplayUnits.h` changes presentation only; numeric export bounds and
analysis retain their original precision.
UI dB/dBFS/dBFS/Hz readouts use one decimal place, preserving infinities and
rounding small finite values to `0.0` without a negative-zero label. This rounding
is applied after logarithms for presentation; integration, noise subtraction,
plot color limits and numeric export values retain full precision.

Spectrum/average CSV headers and PNG metadata include `band_measurements` with
baseband frequency bounds in Hz, inclusive bin indices, integration bandwidth,
ENBW, linear power and dBFS. Signal/noise bands include SNR, normalization,
assumptions, and availability status when both are assigned. The surrounding
metadata records the recording/full-scale reference, source identity, actual
analyzed `[start,end)` frame interval, DSP settings and trace method. CSV bin
columns retain their original spectrum/PSD units. PNG view settings separately
record whether the axis shows absolute RF frequency; band metadata always uses
baseband Hz to remain unambiguous across capture retunes.
The selected band's `occupied_bandwidth` object contains `available`, `percent`
(99), `tail_percent` (0.5), `edge_convention`, `noise_treatment`, and, when available,
`lower_hz`, `upper_hz`, `bandwidth_hz`; unavailable results have a `reason`.
CLI `--band` uses the same calculation in JSON and CSV metadata. Numeric exports
retain unrounded edge values; the UI follows the existing whole-Hz convention.

Waterfall measurement changes publish their ordered row-start frame bounds to
the analysis-interval fields without calling `setRange`. The separate average
button submits a snapshot of those bounds to the existing analysis worker,
using the same exclusive-end convention as the analysis interval. Pending gestures
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
Larger FFT scans use up to eight additional workers, capped at half the advertised
CPU threads and reduced for short jobs or large FFT buffers. Each worker owns its
FFTW engine and positional-read buffer. A single bounded batch per worker is
consumed in window order, preserving the serial online mean, max hold, coverage
counts, and waterfall cells. Cancellation, progress, and snapshot callbacks run
only on the coordinating job thread. Cancellation or a worker exception stops
the batch pipeline and joins every worker before returning or throwing.
Submitting a new job atomically cancels that controller's running token and
replaces its pending job. The main window uses separate controllers for one-window hover previews, waterfall
viewport detail, independent waveform zoom, and the whole-recording minimap. Navigation and hover do not cancel
averages or exports, and minimap scanning continues through viewport changes.
Progressive minimap snapshots are immutable copies, published about every 100 ms
as rows complete. A mutex-protected latest snapshot and one queued notification
coalesce GUI delivery, bounding the queue even while the GUI is occupied.
The main window owns the shared frame cursor and freeze state for both time
plots. Waveform sample buckets select the corresponding waterfall row start.
Waterfall time navigation resets the waveform to its viewport and supplies a
matching waveform snapshot. Waveform wheel zoom and drag panning refine a local
interval independently, including below FFT length. Its own controller reads
bounded previews and rejects obsolete requests. Hover never changes the interval.
Exact Waveform scans the visible waveform interval; frequency-only waterfall
refinement retains independent zoom and exact results. Short exact views keep
one point per sample, drawn as connected I/Q traces with sample markers.
Endpoint placement and editing temporarily suppress shared hover and freeze commands,
and invalidates queued hover results without changing the user's freeze state.
Completed frequency selections follow the displayed frame and recalculate
their mean; new analysis contexts clear plot measurements.
There is no file-sized queue. The GUI debounces navigation edits and invalidates
its current generation immediately. Worker signals are queued to GUI objects;
slots compare their generation before applying any result.

The controller joins its worker before its QObject/cache members are destroyed.
FFTW planning/destruction is serialized with a process-wide mutex; execution
uses worker-private arrays. Each FFTW transform remains single-threaded to avoid
nested parallelism. A blocked filesystem call is not forcibly interrupted;
cancellation is cooperative between bounded reads/transforms.

Snapshots become read-only when emitted. Plot widgets neither mutate numerical
results nor read files. Palette changes recolor cached screen-scale data. A CSV
request owns a snapshot of the exact bins/settings so concurrent UI edits cannot
change an export midway.

## Bounds and cache identity

- Maximum individual recording read: 32 MiB.
- Ordinary decoded read block: 262,144 frames; raised only for a larger FFT.
- Parallel FFT worker selection uses a 128 MiB storage estimate per scan,
  accounting for FFT buffers, decoded/raw blocks, and bounded batches. It is
  not a process RSS limit; the coordinating engine, results, caches, and allocator
  retention are additional. Batch spectra cover at most 262,144 input frames,
  or one larger FFT. No recording-sized work queue is allocated.
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
- Waveform snapshots: at most 2,048 envelope buckets. Previews read at most 131,072
  frames, distributed across the entire viewport, and label sampled coverage.
  Exact scans include every sample in each bucket's extrema.
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
selection. **Use waterfall view** explicitly applies the viewport through
`setRange`; ordinary minimap navigation leaves the analysis interval unchanged.
Pixel mapping subtracts integer frame origins before conversion.
Measurements retain frame endpoints across density refreshes and band drags
preserve their frame span. Hover inspects the painted cell's representative FFT
start; maximum-aggregated cells remain visual summaries, not numeric measurements.
Retune/absolute-frequency labeling uses the actual waterfall viewport. PNG
metadata distinguishes visible bounds from the cached raster's source bounds.

## Persistence and maintenance

Audio/text imports use bounded libsndfile/text conversion into an owned
`QTemporaryFile` containing little-endian float64 samples. gzip/bzip2 raw imports
use bounded streaming decompression into an owned raw temporary file; offsets
and lengths apply to decompressed bytes. Audio is limited to WAV/AIFF PCM
8/16/24/32 and IEEE float32/64. Channel/column mappings are explicit and one-based;
audio defaults to one real channel. Header rates override manual rates, with
AIFF's original extended-precision rate retained rather than libsndfile's rounded
integer. Text rows are capped at 1 MiB and output buffers at 256 KiB; audio uses
at most 32,768 input doubles, and compression buffers are 256 KiB each.

Conversion checks cancellation between bounded blocks/rows and removes storage
on failure, cancellation or final recording destruction. The original source
descriptor/path and stat identity remain authoritative; reads of converted
storage still verify the original. Conversion needs disk space proportional to
the expanded input, and completes before analysis. Sample exports reset the
input format to raw: bytes represent normalized float64 audio, supplied float64
text values, or unchanged decompressed raw data. Float full scale still applies
at decoding time. Reopening a session reconverts the original path with the saved
mapping. Ordinary imports retain their direct positional-read behavior.

Preferences and raw sessions use schema 1; converted/compressed sessions use
schema 2 so older applications reject them instead of reading container bytes as
raw samples. Recording import mappings have a separately checked schema 1.
Both session versions remain readable. Treat unknown versions as a
recoverable error and preserve the original. Recording-specific settings never
silently become facts about a new raw file. Preferences save import defaults,
UI/DSP/view choices, and the application-produced Qt layout state.

When changing measurement behavior, first extend independent/reference fixtures,
then update comments, the user conventions, changelog, and feature/validation
status. When adding a parser, reject mandatory unsupported semantics instead of
displaying plausible but incorrect results. Benchmark changes to rendering,
decoding, FFT threading, or cache behavior.
