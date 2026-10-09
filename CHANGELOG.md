# Changelog

This file records release history and behavior changes. See
[Feature status](FEATURE_STATUS.md) for current capabilities and
[Validation](docs/VALIDATION.md) for test evidence and coverage.

## Unreleased

- Add automatic stable-release checks and update notifications with a release
  page link for manual installation. Help actions check on demand or disable
  automatic checks; incomplete release uploads, offline failures, and rate
  limits are handled without interrupting analysis. Bundle Qt Network's TLS
  backend and OpenSSL runtime libraries in Linux packages.
- Add File → Open recent for the ten most recently opened recordings and
  sessions, with history saved across launches and a Clear recent files action.
  Metadata recordings reopen through their original metadata file.
- Automatically enable absolute-frequency display when opening or changing a
  recording interpretation with a known center frequency. Rename the display
  checkbox to `Absolute frequency`.
- Round spectrum crosshair frequency labels and cursor status readouts to the
  nearest whole Hz.
- Extend the spectrum's vertical frequency cursor through the waterfall to
  correlate signals across both plots. Keep it aligned during zooming and
  resizing, and clear it when the spectrum cursor disappears.
- Add mouse-wheel zoom around the pointer and left-drag pan for both frequency
  and time in the waterfall. Keep frequency navigation linked to the spectra
  and time navigation within the current preview, preserving measurements,
  click-to-freeze, and double-click seeking.

## 0.1.6 — 2026-10-08 — Partial recording import

- Add Open partial / growing file in the import dialog and `--partial` in the
  CLI for recordings still downloading. Analyze the complete frames available
  at open time, ignoring an incomplete final frame and limiting declared data
  lengths and metadata to the downloaded portion.
- Continue analysis while the file is appended to; replacement and truncation
  below the opening file size still stop reads. The downloaded prefix must
  remain unchanged. Reopen the original recording to include more samples;
  saved sessions retain their partial snapshot and sample exports reopen as
  completed recordings.

## 0.1.5 — 2026-10-08 — Keyboard shortcuts cheatsheet

- Add Help → Keyboard shortcuts, opened with F1, listing assigned shortcuts,
  plot gestures, and keyboard navigation. The cheatsheet reads the live menu
  bindings and stays open while working in the main window.

## 0.1.4 — 2026-10-08 — Baudline waterfall palette

- Add the Baudline waterfall palette, a linear gradient from black to `#00F9AB`.
  It recolors the waterfall and its legend without recomputing FFTs and is
  retained in preferences and sessions. Baudline is the default for new settings;
  existing saved palette choices are preserved.

## 0.1.3 — 2026-10-08 — Plot measurements and selection averaging

- Waterfall Shift selections populate the time-selection frame fields without
  applying the range. A new Average waterfall selection button runs an exact
  average of the marked interval while preserving the displayed time range.
- Spectrum hover shows a crosshair with frequency and power axis labels,
  following zoom, absolute-frequency and spectrum/PSD settings. Cursor
  coordinates accompany the exact FFT-bin readout in the status bar.
- Shift-drag a completed measurement's shaded band to move it or either marker
  to resize it, with live power/duration updates and clamping at the data bounds.
- Shift + left-click two endpoints in the spectrum to measure frequency width
  and mean spectral power from exact linear bins, or two waterfall rows to
  measure duration from their sample-frame timestamps. Persistent markers and
  readouts support reverse selections, replacement, and Escape to clear.
- Frame following pauses during endpoint placement without changing the freeze
  state. Completed spectrum measurements follow the displayed frame; view-only
  changes preserve measurements, while new recordings/ranges/DSP clear them.

## 0.1.2 — 2026-10-07 — Synchronized frame inspection

- Hovering the waterfall highlights one row and displays its exact FFT window
  in the frequency spectrum, preserving frequency zoom/pan. New recordings
  start at frame 0; leaving the waterfall retains the last spectrum.
- Hover analysis uses a separate background controller so averages and exports
  continue. Spectrum exports and retune-aware labels follow the displayed frame.
- Waveform hover shares the spectrum frame and waterfall highlight, with a
  vertical waveform marker. Clicking either time plot freezes both markers
  and the spectrum; the next click resumes following the pointer. Bounded
  waveform previews follow frames outside their current range.

## 0.1.1 — 2026-10-06 — Initial public release

- Native Linux desktop application built with C++20, Qt 6 Widgets, and FFTW.
- Read-only access to large recordings with bounded reads, 64-bit sample ranges,
  input-change detection, and explicit format interpretation.
- Real/IQ/QI decoding for signed and unsigned 8/16/24/32/64-bit integers and
  IEEE binary16/32/64, with both byte orders.
- Raw binary, single-stream SigMF core metadata, and exported recording
  descriptor import.
- Power-of-two FFTs from 256 to 1,048,576 points, four window functions,
  configurable overlap, DC removal, and spectral inversion.
- Spectrum and PSD scaling in dBFS and dBFS/Hz, with documented normalization
  and real-input one-sided scaling.
- Exact interval and whole-recording spectral averages, max hold, and a
  waterfall overview that processes every required window.
- Waveform inspection and exact min/max envelopes across selected intervals.
- Dockable spectrum, waterfall, average/max-hold, and waveform panels with linked
  navigation, sample/time selection, frequency zoom/pan, palettes, and color limits.
- Background analysis and exports with progress reporting, cancellation,
  stale-result protection, and a bounded 64 MiB preview cache.
- Optional UTC capture timestamps and frequency axes that account for retunes
  and unknown center frequencies.
- Atomic JSON preferences and sessions, persisted workspace layouts, bookmarks,
  and lossless 64-bit sample indices.
- Numeric CSV, plot PNG with embedded metadata, and byte-preserving sample-range
  exports with sidecars, trimmed capture segments, and annotations.
- Transactional export publication that refuses existing output paths and
  discards cancelled writes.
- Headless command-line tools for inspection, spectra, averages, waveforms,
  and sample exports.
- Embedded application artwork and Linux desktop launcher integration.
- Portable Linux folder and AppImage packages with bundled dependencies,
  license notices, complete application/dependency sources, and checksums.
- GitHub CI builds for Release, Debug, and ASAN/UBSAN configurations,
  with downloadable artifacts and test reports.
- Pinned CI actions and packaging downloads, verified runtime checksums, and
  offline package smoke checks on Ubuntu and Fedora.
- Core and GUI integration tests, 167 independent NumPy/SciPy numerical checks,
  parser stress tests, sanitizer procedures, and sustained-use test tools.
- Installation, analysis, architecture, testing, and benchmark documentation.
  Recorded test environments and coverage are detailed in
  [Validation](docs/VALIDATION.md).
