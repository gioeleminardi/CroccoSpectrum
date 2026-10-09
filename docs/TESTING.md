# Development and testing

Ordinary verification (Qt 6.4+ / FFTW development packages installed):

```sh
cmake --preset debug -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build --preset debug -j6
ctest --preset debug
```

The Python selected above must have NumPy/SciPy. CTest explicitly skips the oracle
when those modules are missing; a skipped oracle is not a numerical pass.
The metadata stress test uses only Python's standard library.

ASAN/UBSAN require the runtime matching the compiler. Use the Ubuntu container
from README when the host compiler installation lacks matching sanitizer libs.
Configure `-DRF_ENABLE_ASAN=ON` in a separate build and run all five suites.

The `updates` suite uses simulated network replies for release selection,
timeouts, rate limits, cancellation, preferences, and notification behavior.
Normal tests never contact GitHub. To explicitly check real HTTPS connectivity:

```sh
QT_QPA_PLATFORM=offscreen RF_TEST_LIVE_UPDATE_CHECK=1 build/debug/rf-update-tests liveHttpsCheck
```

GUI `--smoke-test` also verifies that a usable TLS backend is available without
making a network request. Packaged HTTPS checks use the host's certificate trust
store; Linux systems used for online checks need current CA certificates.

For useful TSAN GUI evidence, instrument Qt too. With the accompanying Qt base
source archive extracted into `.cache/qt-source`, run in the Ubuntu builder:

```sh
cmake -S .cache/qt-source/qtbase-everywhere-src-6.11.2 -B build/qt-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="$PWD/.cache/qt-tsan" -DFEATURE_sanitize_thread=ON -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DFEATURE_xcb=OFF -DFEATURE_opengl=OFF -DFEATURE_vulkan=OFF -DFEATURE_icu=OFF -DFEATURE_dbus=OFF
cmake --build build/qt-tsan -j8
cmake --install build/qt-tsan
cmake -S . -B build/tsan-instrumented -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRF_ENABLE_TSAN=ON -DCMAKE_PREFIX_PATH="$PWD/.cache/qt-tsan" -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/tsan-instrumented -j6
ctest --test-dir build/tsan-instrumented --output-on-failure -R 'core|ui'
```

This instrumented Qt is a test dependency, not the distributed release runtime.
Use separate ASAN and TSAN builds. Do not suppress whole Qt libraries to convert
an unexplained race report into a claimed passing gate.

Explicit dense-file benchmark (up to 40 GB temporary disk space, cleaned on exit):

```sh
python3 tests/benchmark.py --cli dist/croccospectrum-0.2.0-x86_64/croccospectrum-cli --gui dist/croccospectrum-0.2.0-x86_64/AppRun --output build/benchmarks.json
```

Extended GUI stress test (eight hours in this example), built when
BUILD_TESTING is enabled:

```sh
QT_QPA_PLATFORM=offscreen RF_SOAK_SECONDS=28800 build/release/rf-soak-test > build/soak-eight-hours.json
```

The default duration is 60 seconds. The fixture is a temporary dense 400 MB file
containing synthetic samples. Remove `QT_QPA_PLATFORM=offscreen` for a native
session. The harness checks results and emits timing/RSS evidence; it is not a
substitute for human desktop/input/docking inspection on each supported system.

When validating recording workflows, independently cross-check sample decoding,
known tone sign/power, the configured rate against the recorder-reported rate,
headers, and gap logs. Resume sessions and inspect exported metadata/bytes.
Exercise disk-full/unwritable outputs, source replacement, long average
cancellation, missing/corrupt sessions, high-DPI docking, and multiple monitors.
Record exact desktop/OS/build versions.
