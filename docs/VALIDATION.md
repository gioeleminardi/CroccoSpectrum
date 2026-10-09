# Validation evidence

Recorded checks for CroccoSpectrum 0.1.0, 0.1.1, and the changes included in
0.1.2, 0.1.3, 0.1.4, 0.1.5, and 0.1.6, plus unreleased updates as of 2026-10-09.
The environments and test coverage below define the scope of this evidence.

## Passed

- Update notifications (unreleased): all five CTest suites pass on Fedora 44
  Debug / Qt 6.11.2 and Ubuntu 24.04 Release / official Qt 6.11.2 SDK, and on
  Ubuntu 24.04 Debug / system Qt 6.4.2. Simulated replies cover numeric version
  ordering, draft/prerelease/malformed tags, incomplete or empty assets,
  preference compatibility, daily throttling, disabled/manual checks, concurrent
  requests, HTTP/network/TLS failures, invalid/oversized JSON, rate-limit reset
  and Retry-After headers, timeout, cancellation, nonmodal notifications, browser
  URLs, and duplicate suppression across launches. The notification was visually
  inspected offscreen. Opt-in live GitHub HTTPS checks pass with host and bundled
  Qt Network/TLS/OpenSSL libraries; routine tests stay offline. Both final
  AppImage and portable-folder GUI/CLI launches and TLS availability pass in
  clean network-disabled Ubuntu 24.04/26.04 and Fedora 44 containers. Package
  manifests, archive checksums, and matching OpenSSL source provisions verify.
- CI artifact layout (unreleased): Ubuntu 24.04 container Release and Debug
  builds pass all four CTest suites. Release produces exactly one executable
  AppImage and one portable tarball with GUI/CLI launchers and bundled runtime
  dependencies; sources and checksums are stored separately. Both final packages
  launch offline on Ubuntu 24.04, Ubuntu 26.04, and Fedora 44. Debug archives retain
  all build outputs, executable permissions, build identity, and a valid checksum.
  The GitHub workflow passes actionlint; shell scripts and embedded
  Python parse successfully. The local builder was refreshed after a cached Ubuntu
  library's exact source version became unavailable from the live mirrors.
- CroccoSpectrum 0.1.6: Fedora 44 Debug and Release builds and all four CTest
  suites pass, including the independent numerical reference checks. Both GUI
  and CLI report 0.1.6; packaging metadata matches the CMake project version.
  Partial-import regressions cover incomplete real/IQ frames, byte offsets and
  declared lengths, fixed bounds during appends, replacement/truncation,
  clipped SigMF captures/annotations, session snapshots, strict-mode checks,
  completed byte exports, and import-preset compatibility. GUI tests cover
  toggling partial mode, decoded import previews, cached previews and averaging
  after appends; the import dialog was visually inspected offscreen. All five
  CLI commands were checked with partial raw data in both configurations,
  along with SigMF/descriptor imports, exported sidecars, and invalid bounds.
- CroccoSpectrum 0.1.5 version bump: Debug and Release builds pass, along with
  all four Debug CTest suites. Both GUI and CLI `--version` report 0.1.5 in both
  configurations; packaging metadata matches the CMake project version.
- CroccoSpectrum 0.1.4: Fedora 44 Debug build and all four CTest suites pass.
  Baudline rendering checks exact black/#00F9AB endpoints, midpoint, clipping,
  invalid cells, and legend colors at normal and double display scale. Session
  and preference checks retain the palette, restore its selector, and preserve
  the DSP snapshot during palette edits. Both executables report 0.1.4; normal
  and double-scale renders were visually inspected offscreen.
- CroccoSpectrum 0.1.3 version bump: Fedora 44 Debug build and all four CTest
  suites pass. Both GUI and CLI `--version` report 0.1.3; packaging metadata
  matches the CMake project version.
- Waterfall selection averaging: all four Fedora 44 Debug CTest suites pass.
  Forward/reverse GUI cases verify live draft frame fields during placement and
  band/marker edits, unchanged applied range and preview, and an exact average
  containing the marked tones while excluding an outside tone. Button checks
  cover pending, zero/short, cleared and completed selections, Apply, DSP edits,
  and new recordings. Exact row-start bounds above 2^53 are also checked. These
  cases and the shared-frame regressions pass at QT_SCALE_FACTOR=2; normal and
  double-scale button/field layouts were visually inspected offscreen.
- Spectrum crosshair: all four Fedora 44 Debug CTest suites pass. Two added GUI
  cases check pointer-centered horizontal/vertical lines, frequency and power
  coordinates with fixed/automatic limits, zoom and absolute-frequency PSD,
  margin/leave/empty-data behavior, and compatibility with measurement placement
  and Shift-drag edits. These cases and the GHz-bin precision regression also
  pass at QT_SCALE_FACTOR=2. Normal and double-scale renders, including a
  minimum-size panel and an active measurement, were visually inspected offscreen.
- Shift-drag editing: all four Fedora 44 Debug CTest suites pass. Four added GUI
  cases cover moving and resizing forward/reverse selections, live power and
  duration updates, marker crossing/overlap, both data boundaries, replacement
  clicks, and Escape during a drag. Shared-frame checks also cover source-frame
  stability while moving a band or resizing a marker, preserving follow/freeze
  state on release. These edit and shared-frame cases pass at QT_SCALE_FACTOR=2.
- Shift-click measurements included in 0.1.3: all four Fedora 44 Debug CTest suites
  pass with Qt 6.11.2, including all 167 independent NumPy/SciPy comparisons.
  New GUI cases cover linear-bin means versus dB/pixel means, one million FFT
  bins, spectrum/PSD and average/max-hold units, zero/invalid power, reverse and
  zero-width selections, irregular row timestamps, fractional sample rates,
  duration above frame 2^53, replacement/Escape, view-only preservation, range
  reset, and shared hover/freeze/queued-result behavior. The six new GUI cases
  also pass at QT_SCALE_FACTOR=2. Normal and double-scale measurement renders,
  including minimum-size panels, were visually inspected. These checks are
  offscreen; older sanitizer and package evidence does not cover this change.
- Waterfall hover included in 0.1.2: all four CTest suites pass on Fedora 44 in the
  Debug build with Qt 6.11.2. GUI checks cover frame 0, first/middle/last-row
  highlight geometry, rapid hover and selection/DSP changes, zoom preservation,
  known/unknown capture frequencies, invalid windows, sampled/exact overviews,
  uninterrupted averages/exports, and displayed-frame CSV bins/PNG metadata.
  The rendered hover screenshot was visually inspected.
- Shared waveform cursor and click freeze: all four Fedora Debug CTest suites
  pass. Checks cover vertical marker geometry above 2^53, docked/floating
  waveform interaction, locking/releasing from either plot, frozen markers after
  pointer exit, ignored right/margin clicks, selection reset, double-click seek,
  and local waveform previews beyond the initial region while retaining exact
  full-selection waveforms. The frozen-frame screenshot was visually inspected.
- CroccoSpectrum 0.1.1 rebrand: all four Ubuntu-baseline CTest suites pass,
  including embedded icon/logo loading, About-dialog rendering and legacy
  preference migration. Installed and portable launcher icons match the project
  PNG byte for byte. Normal and doubled-scale About screenshots were visually
  inspected. Final portable folder/AppImage launch checks pass offline on
  Ubuntu 24.04/26.04 and Fedora 44; native Fedora X11/Wayland checks also pass.
- Fedora 44 release build: GCC 16.2.1, system Qt 6.11.2, FFTW 3.3.10.
- Ubuntu 24.04 release baseline: GCC 13.3.0, official Qt 6.11.2 gcc_64 SDK,
  Ubuntu FFTW 3.3.10. All four CTest suites pass without skipped references.
- Ubuntu 24.04 development compatibility build against system Qt 6.4.2:
  all four CTest suites pass. The declared minimum Qt API is exercised.
- Core checks: scalar byte fixtures, FP16 special values, signed/unsigned 64-bit
  near-zero centering, 100 GiB sparse offsets, truncation/mutation, read bounds,
  partial frames, lossless JSON indices above 2^53, frequency sign/DC/Nyquist/
  tone scaling, overlapping block traversal, capture boundaries, cancellation,
  envelope peaks, byte exports, trimmed capture/annotation semantics, sessions
  and SigMF. Oversized saves preserve the previous readable file. Exact-overview regression
  preserves a burst skipped by the preview; extreme floating PSD avoids premature
  squared-magnitude underflow/overflow.
- Widgets integration checks: latest FFT/range request wins; palette-only edits
  preserve the DSP snapshot; preference restore; precise GHz cursor readouts; whole-pass cancellation in favor
  of preview; tiny-file inspection; background PNG metadata and no overwrite.
- Independent NumPy/SciPy oracle: **167 checks** covering the declared decoder
  matrix, real/complex Welch windows/scales/DC choices, fractional rate, every
  supported FFT exponent, and invalid/fractional/overflowing CLI integers.
- Parser stress: **301 bounded metadata cases**, plus seeded random complete/
  incomplete buffers for every scalar encoding in the core suite.
- AddressSanitizer + UndefinedBehaviorSanitizer: all four suites pass with the
  matching Ubuntu GCC 13 runtimes; no leak suppression was used.
- ThreadSanitizer: core and Widgets suites pass with **Qt 6.11.2 itself rebuilt
  with thread instrumentation**. No race suppressions were used. Running against
  prebuilt Qt gave internal Qt thread-pool warnings; that run is not counted as
  a pass. Test observers relay worker signals to the GUI thread before spying.
- Portable-folder CLI and offscreen GUI launch in clean, offline Ubuntu 24.04,
  Ubuntu 26.04 and Fedora 44 containers with no host Qt/Python installed.
- Fedora native X11 and Wayland launch/preview smoke checks; rendered GUI screenshot
  inspected. The final artifacts also have a no-FUSE AppImage extraction launch.
- Dense 400 MB, 4 GB and 40 GB fixtures; bounded first views and tail spectra;
  400 MB/4 GB full averages. See [BENCHMARKS.md](BENCHMARKS.md).
- Short GUI stress: 499 mixed-operation cycles over 60 seconds, latest-result and
  palette invariants preserved; memory/timing recorded in the benchmark report.
- Shared CI pipeline executed locally in fresh Ubuntu 24.04 containers: all
  Release, Debug and ASAN/UBSAN targets build and all four suites pass in each
  configuration. All seven outputs, tar executable permissions and checksums
  were checked. The final CI Release folder/AppImage launch offline on Ubuntu
  24.04/26.04 and Fedora 44. GitHub workflow passes actionlint.

Performance and original sanitizer evidence above was collected for 0.1.0.
The local CI runs add fresh 0.1.1 Release/Debug and ASAN/UBSAN evidence. TSAN was
not repeated for the CI change; the numerical algorithms and recording/session
formats remain unchanged.

The synchronized frame and freeze changes included in 0.1.2 were checked in the
Fedora Debug build before the version bump. No build or package qualification
was completed for the version bump; the older sanitizer, Qt 6.4 compatibility,
packaging, and benchmark evidence does not include these changes.

Sanitizer checks used the Ubuntu builder with compiler-matched runtimes.
Build/test logs are generated under the ignored `build/` directory.

## Coverage limitations

Recording checks use synthetic fixtures. Raw-file interpretation must match the
recording writer's format, component order, byte order, and sample rate.

Native desktop evidence is limited to Fedora X11/Wayland smoke checks. Ubuntu
launch checks use containers. Native high-DPI and multiple-monitor interaction and
long-duration stability have not been validated by the recorded runs.

The recorded checks do not cover comprehensive disk-full/permission failures,
broader parser fuzzing, blocked network storage, the sustained-storage ceiling,
or a performance matrix for every supported encoding. Sparse offset checks
verify addressing rather than storage throughput.

CI evidence comes from local pipeline execution. Runner registration, server
permissions, and server-side artifact uploads on GitHub have not been
verified by those runs.
