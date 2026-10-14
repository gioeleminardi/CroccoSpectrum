# AGENTS.md

## Scope and working agreement

This guide applies throughout the repository. Read any more specific `AGENTS.md`
before editing its directory. Explicit user instructions take precedence over
repository guidance; more specific repository instructions apply to their scope.

- **Think before coding.** Inspect the implementation, tests, and working-tree
  changes first. State assumptions and tradeoffs. If an ambiguity changes the
  required behavior, explain the alternatives and ask before implementing it.
- **Prefer simplicity.** Implement the smallest change that meets the request.
  Avoid speculative features, dependencies, abstractions, and configuration.
- **Make surgical changes.** Match nearby style. Preserve unrelated user work;
  do not reformat or refactor adjacent code. Remove only code made unused by your
  change, and mention unrelated problems separately.
- **Work toward verifiable outcomes.** For a multi-step task, give a brief plan
  with a check for each step. Reproduce bugs with a focused regression when
  practical, implement the fix, and run the relevant checks. Documentation-only
  changes need command/link review and whitespace checks, not new runtime tests.
- **Report evidence accurately.** Summarize the result, checks actually run,
  skipped or unavailable checks, and remaining limitations. Do not claim old
  validation or a skipped test proves a new change.

## Project context and current architecture

CroccoSpectrum is an offline RF recording analyzer for Linux x86_64. It uses
C++20, Qt 6 Widgets/Network, and double-precision FFTW. The native GUI and headless
CLI share the recording, DSP, analysis, and persistence code. The application
does not acquire live RF data or require Python/Node.js at runtime.

Current capabilities include raw real/complex input, a single-stream SigMF core
subset, partial-download snapshots, linked spectrum/waterfall views, an exact
whole-recording minimap, independent waveform zoom, exact averages/envelopes,
sessions, and CSV/PNG/raw-byte exports. Normal GUI launches have optional stable
release checks; routine tests and CLI analysis remain offline.

Use implementation and configuration as the authority for current behavior:

| Concern | Start here |
| --- | --- |
| Planned version, targets, minimum dependencies, test registration | `CMakeLists.txt` |
| Configure/build/test presets | `CMakePresets.json` |
| C++ formatting | `.clang-format` |
| Development/stable version identity | `cmake/BuildVersion.cmake`; generated `BUILD-INFO.json` |
| Supported CLI flags and defaults | `src/cli.cpp` and `croccospectrum-cli --help` |
| Dependency pins and portable ABI baseline | `packaging/dependencies.json`, `packaging/Containerfile` |
| Build/publication behavior | `.github/workflows/`, `ci/build.sh`, `ci/run.sh`, `ci/publish.py` |
| Recorded qualification and performance | [Validation](docs/VALIDATION.md), [Benchmarks](docs/BENCHMARKS.md) |

Version labels in historical documentation can lag CMake. Check the build
identity and dated evidence instead of copying a version or treating every
feature-status entry as a current qualification result.

### Repository map

| Area | Responsibility |
| --- | --- |
| `src/recording/` | Sample formats, checked positional reads, decoding, source identity, metadata and SigMF parsing |
| `src/dsp/` | FFTW engine, periodic windows, normalization, real/complex spectral conventions |
| `src/app/Analysis.*` | Bounded previews, complete scans, ordered parallel reductions, cancellation and exports |
| `src/app/AnalysisController.*` | Background scheduling, generations, immutable results, bounded worker-owned caches |
| `src/app/Session.*`, `src/app/ExportFile.h` | Versioned preferences/sessions and transactional export publication |
| `src/app/UpdateChecker.*` | Asynchronous Qt Network release checks, preferences and retry behavior |
| `src/ui/` | Import dialog, main-window commands/state, plotting and input gestures |
| `src/main.cpp`, `src/cli.cpp` | GUI/CLI entry points |
| `tests/` | QtTest core/UI/update suites, Python pipeline/oracle/stress tools, website tests and benchmarks |
| `packaging/`, `ci/` | Portable bundles, dependency/source manifests, container checks and release publication |
| `site/` | Static download website and release selection; no npm build step |
| `docs/`, `assets/` | Detailed guides, recorded evidence and existing branding |

`rf_core` depends on Qt Core, FFTW, and Threads. `rf_ui` adds Widgets and Network;
the CLI links only to the core. Preserve that boundary. There is no plugin
registry or alternate DSP backend to extend speculatively.

## Build and verification commands

Required: a C++20 compiler, CMake 3.22+, Ninja, pkg-config, Qt 6.4+ development
files for Core/Gui/Widgets/Network/Test, and FFTW 3 double precision. Python with
NumPy/SciPy and Node.js enable all currently registered test suites. Installation
commands are in [README.md](README.md#build-from-source).

Run from the repository root:

```sh
cmake --preset debug -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build --preset debug --parallel 2
ctest --preset debug
```

Use the Python interpreter that has NumPy/SciPy; replace `/usr/bin/python3` when
needed. `release` is the equivalent optimized preset. Outputs are in
`build/PRESET/`; keep generated builds, fixtures, packages and reports out of Git.
Prefer existing suitable build directories to avoid unnecessary rebuilds.

For focused verification after building:

```sh
ctest --preset debug -R '^(core|parser-stress|reference)$'
ctest --preset debug -R '^(ui|updates)$'
ctest --preset debug -R '^(release-pipeline|website-releases)$'
```

| Changed behavior | Relevant checks |
| --- | --- |
| Decoding, indexing, metadata, sessions, byte exports | `core`, `parser-stress`; `reference` for supported encodings/CLI behavior |
| FFT windows, scaling, averaging, coverage or parallel reductions | `core`, independent `reference`; benchmark changed hot paths |
| Navigation, rendering, controller scheduling, cache identity | `ui`, relevant `core`; inspect the affected GUI interaction |
| Release checks and update preferences | `updates` with simulated network replies |
| Build identities, package/publication rules, download channels | `release-pipeline`, `website-releases`; package checks if bundle behavior changes |
| Documentation only | Check links, anchors, commands against source, and `git diff --check` |

Run the full applicable CTest suite before handing off a code change. Python
tests are registered only when Python is found; `website-releases` requires
Node.js. `reference` returns skip code 77 without NumPy/SciPy. Check the discovered
suites and skips: a green CTest summary with missing or skipped checks is not
complete numerical verification. The UI/update suites set the offscreen platform
through CTest. Native docking checks require a live display and can be skipped
headlessly; describe that limit.

ASAN/UBSAN use the separate `asan` configure/build/test presets and matching
compiler runtimes. TSAN uses the `tsan` configure/build presets; **there is no
TSAN test preset**. Follow [Testing](docs/TESTING.md) for instrumented Qt and
explicit `ctest --test-dir ...` commands. Do not enable ASAN and TSAN together
or suppress unexplained library races to manufacture a pass.

GUI changes need visual inspection where feasible, including normal and doubled
display scale for geometry/text changes. Soak runs, dense-file benchmarks, real
HTTPS checks, and portable container builds are separate, explicit procedures
in [Testing](docs/TESTING.md) and [CI builds](docs/CI.md); run them when the change
needs that evidence rather than on every edit.

## C++ and Qt conventions

- Follow the style of the edited file: `#pragma once`, the `rf` namespace, named
  types and explicit ownership. `.clang-format` defines LLVM-based formatting,
  four-space indentation, a 100-column limit and Linux brace placement. Keep
  formatting limited to changed code; avoid whole-file reformatting.
- Use RAII for file descriptors, temporary files, FFTW resources and thread
  lifetime. Use existing checked readers/validators and export helpers rather
  than bypassing them. Do not silence `-Wconversion` or `-Wshadow` warnings with
  unchecked casts.
- Keep units and ownership explicit. Comment numerical conventions, cancellation
  boundaries and resource limits when they are not obvious from the code.
- Keep file I/O and FFT work out of plot painters and GUI-thread interaction
  handlers. Plot widgets consume immutable snapshots and render cached data.
- Maintain Qt 6.4 API compatibility unless the task explicitly changes the
  supported minimum. The newer packaged Qt pin does not raise the source minimum.
- Add dependencies only when the requested behavior needs them; preserve the
  existing CMake/QtTest/Python-standard-library/Node-test workflows.

## Numerical and recording invariants

- A frame is one real scalar or one complex I/Q pair. Use unsigned 64-bit frame
  indices and half-open `[begin, end)` ranges throughout. Validate bounds and
  byte arithmetic before reading. Never confuse components, frames and bytes.
- Preserve indices above `2^53`: subtract integer origins before converting
  coordinates to floating point. Store application JSON indices as decimal
  strings; respect SigMF's own schema when importing it.
- Interpret complex samples as `I + jQ`. Real spectra are one-sided with doubled
  interior-bin power, excluding DC/Nyquist; complex spectra are FFT-shifted.
  Preserve integer normalization and the explicit floating full-scale reference.
- For the unnormalized forward FFT of a periodically windowed signal, spectrum
  power is `|X|² / |sum(w)|²`; PSD is `|X|² / (Fs * sum(w²))`. Average linear
  powers before converting to dB. Zero power is negative infinity, not a fabricated
  numerical floor. dBFS/dBFS/Hz are not calibrated dBm.
- Selected-band power integrates PSD times FFT bin spacing, or spectrum power
  times spacing/ENBW, over inclusive bin centers. Report dBFS; normalize a
  separate noise band's power to the signal bandwidth before subtracting noise
  for SNR. Use one displayed FFT or a completed linear-power average, preserving
  the analyzed interval and band/DSP metadata in exports.
- Exact passes visit every eligible complete hop-aligned window. Reject and
  count invalid and capture-boundary windows; report trailing frames and finite
  clipping separately. Do not use sampled previews or aggregated pixels as exact
  measurement input. Preserve window-order reductions when parallelizing.
- Keep preview coverage labels honest. Exact minimap/overview cells use maxima
  to retain events; spectrum measurements use the actual bins. Waveform previews
  can miss peaks, while exact scans preserve every sample's min/max contribution.
- Source recordings stay read-only. Use bounded positional reads, preserving
  source mutation/replacement checks; do not replace them with whole-file loading
  or mappings that can fault on truncation.
- Partial imports are fixed snapshots of complete frames at open time. Appends
  are allowed, but truncation below the opening file size or path replacement
  fails. This assumes a stable downloaded prefix and does not verify same-size
  edits or preallocated holes. Sessions retain snapshot bounds; sample exports
  reset partial mode.

Use [Architecture](docs/ARCHITECTURE.md) for the full measurement and storage
contract. When changing it, extend the independent numerical fixtures with
explicit window, hop, detrending, scaling and sidedness; avoid tests that merely
reimplement the same algorithm.

## Concurrency, navigation and resource bounds

- Each controller owns a `std::jthread`, one running job and one replaceable
  pending job. New submissions cancel obsolete work. Preserve cooperative
  checkpoints, exception propagation and joining before dependent members die.
- Results are immutable once published. Deliver worker results to GUI objects
  through queued notifications, and reject stale generations before updating
  state. Coalesce progressive snapshots so a slow GUI cannot accumulate a queue
  proportional to recording size. Qt documents these thread-affinity constraints
  in [Threads and QObjects](https://doc.qt.io/qt-6/threads-qobject.html).
- Each FFT worker owns its engine, buffers and positional-read storage. Keep
  FFTW planning/destruction serialized and transforms single-threaded; parallel
  scans use bounded outer workers. The planner's shared state is documented in
  [FFTW thread safety](https://www.fftw.org/fftw3_doc/Thread-safety.html).
- Cancellation, progress and snapshot callbacks stay on the coordinating job
  thread. Stop and join all scan workers on cancellation or read failure. A
  blocked filesystem call is not forcibly interrupted.
- Preserve separate controllers for analysis/export, hover, waterfall detail,
  independent waveform zoom and the minimap. Navigation must not cancel unrelated
  averages/exports or restart the minimap. Palette/range-color edits recolor
  numerical snapshots without new FFTs.
- Keep the waterfall viewport separate from the analysis interval. Applying
  **Use waterfall view** is explicit. Waterfall time navigation resets waveform
  bounds; frequency-only navigation retains independent waveform zoom/exact scans.
- Treat caches as worker-owned and byte-budgeted. Keys include source identity,
  interpretation, DSP settings and range, plus waterfall frequency/grid/density
  choices; palettes and color limits are presentation state.
- Preserve existing bounds: 32 MiB per recording read, 64 MiB ordinary controller
  caches (4 MiB minimap), up to eight FFT scan workers with a 128 MiB worker-storage
  estimate, and at most 4,194,304 waterfall cells. These are individual budgets,
  not a promise about total process RSS. Consult source and architecture before
  changing them, and benchmark affected paths.

## Persistence, packaging and documentation

- Keep preference/session schemas versioned and compatible. Reject unsupported
  versions or mandatory metadata semantics clearly; preserve readable existing
  files on failed saves. Raw interpretation must come from the user or metadata,
  never an unannounced device-model assumption.
- Preferences use atomic replacement; measurement exports use temporary files
  and no-overwrite publication through `ExportFile`. Cancellation/failure must
  not expose a partially written file as a completed result.
- `CMakeLists.txt` owns the planned numeric version. Development identities are
  generated by `cmake/BuildVersion.cmake`; package names, CLI/GUI versions and
  manifests must agree. A Release build is not automatically a stable channel.
- Preserve the Ubuntu 24.04/glibc 2.39 portable baseline, pinned dependencies,
  checksums, notices and accompanying sources. Use [CI builds](docs/CI.md) for
  package validation and publication procedures; do not publish as an incidental
  verification step.
- Keep normal tests offline. Update tests simulate network replies; live HTTPS
  checks require the explicit opt-in documented in [Testing](docs/TESTING.md).
- Update `CHANGELOG.md` under **Unreleased** for behavior changes, and relevant
  README/architecture/feature guidance when their claims change. Add validation
  evidence only for checks actually completed, with build/environment and scope;
  preserve older evidence as historical rather than relabeling it as current.
- Maintain this file when commands, boundaries or invariants change. Keep task
  transcripts and speculative plans out of permanent project instructions. The
  [AGENTS.md format guide](https://agents.md/) describes its role and scope.

Before handoff, inspect `git diff`, run `git diff --check`, and confirm that every
changed file belongs to the request. Report pre-existing failures separately;
do not widen a focused patch to repair unrelated code or claim unrun checks.
