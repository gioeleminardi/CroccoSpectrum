#!/usr/bin/env bash
# Runs inside the Ubuntu baseline builder. Compile the default ALL target so
# GUI, CLI, both libraries, both test executables and the soak tool are covered.
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:?Missing build configuration}
output="$PWD/dist/ci/$mode"
mkdir -p "$output/packages" "$output/reports"
version=$(python3 -c 'import json; print(json.load(open("packaging/dependencies.json"))["application"])')
qt_version=$(python3 -c 'import json; print(json.load(open("packaging/dependencies.json"))["release_qt"])')
aqt_version=$(python3 -c 'import json; print(json.load(open("packaging/dependencies.json"))["aqtinstall"])')
qt="$PWD/.cache/qt/$qt_version/gcc_64"
if [[ ! -f "$qt/lib/libQt6Core.so.6" ]]; then
    python3 -m venv .cache/qt-tools
    .cache/qt-tools/bin/pip install "aqtinstall==$aqt_version"
    .cache/qt-tools/bin/python -m aqt install-qt linux desktop "$qt_version" linux_gcc_64 \
        --outputdir .cache/qt --archives qtbase qtwayland qtsvg icu
fi
case "$mode" in
    release) type=Release; sanitizer=OFF ;;
    debug) type=Debug; sanitizer=OFF ;;
    asan) type=Debug; sanitizer=ON ;;
    *) exit 2 ;;
esac
build="$PWD/build/ci-$mode"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE="$type" \
    -DCMAKE_PREFIX_PATH="$qt" -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DRF_ENABLE_ASAN="$sanitizer" -DPython3_EXECUTABLE=/usr/bin/python3
grep -Fx "CMAKE_PROJECT_VERSION:STATIC=$version" "$build/CMakeCache.txt"
cmake --build "$build" --parallel "${RF_CI_JOBS:-2}"
python3 -c 'import numpy, scipy' # Missing independent-test dependencies must fail CI.
ctest --test-dir "$build" --output-on-failure --output-junit "$output/reports/tests.xml"
cp "$build/Testing/Temporary/LastTest.log" "$output/reports/"

# A tar preserves executable bits lost by artifact ZIP uploads. Development
# archives contain every actual CMake output, not just the installed GUI/CLI.
export RF_CI_MODE="$mode" RF_CI_OUTPUT="$output" RF_CI_BUILD="$build" RF_CI_QT="$qt_version"
python3 - <<'PY'
from pathlib import Path
import hashlib,json,os,shutil,subprocess,tarfile
output=Path(os.environ['RF_CI_OUTPUT']); build=Path(os.environ['RF_CI_BUILD'])
pins=json.loads(Path('packaging/dependencies.json').read_text())
mode=os.environ['RF_CI_MODE']
info={'application':'CroccoSpectrum','version':pins['application'],'configuration':mode,
      'commit':os.environ['RF_CI_COMMIT'],'qt':os.environ['RF_CI_QT'],
      'compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
      'platform':'Ubuntu 24.04 x86_64','runtime_note':'Development outputs require matching Qt/FFTW and compiler runtimes. Use the release AppImage or portable folder for bundled dependencies.'}
(output/'reports/BUILD-INFO.json').write_text(json.dumps(info,indent=2)+'\n')
if mode != 'release':
    stage=build/'artifact'; stage.mkdir(exist_ok=True)
    for name in ['croccospectrum','croccospectrum-cli','rf-core-tests','rf-ui-tests','rf-soak-test','librf_core.a','librf_ui.a']:
        shutil.copy2(build/name,stage/name)
    shutil.copy2(output/'reports/BUILD-INFO.json',stage/'BUILD-INFO.json')
    archive=output/'packages'/f"croccospectrum-{pins['application']}-linux-x86_64-{mode}.tar.gz"
    with tarfile.open(archive,'w:gz') as target:
        target.add(stage,arcname=archive.name.removesuffix('.tar.gz'))
    digest=hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name+'.sha256').write_text(f'{digest}  {archive.name}\n')
(output/'reports/signal.iq').write_bytes(bytes.fromhex('00400000')*8192)
PY

if [[ $mode == release ]]; then
    python3 packaging/portable.py --build "$build" --qt "$qt" --output dist/release
    bundle="$PWD/dist/release/croccospectrum-$version-x86_64"
    apt-get update # Include deb-src indexes for the exact bundled Ubuntu packages.
    python3 packaging/sources.py --bundle "$bundle"
    python3 packaging/finalize.py --bundle "$bundle"
    # Fetch only pinned tooling; verify even when a local cache is available.
    # The runtime's upstream continuous URL is mutable; a changed hash fails CI.
    python3 - <<'PY'
from pathlib import Path
import hashlib,json,urllib.request
pins=json.loads(Path('packaging/dependencies.json').read_text())
for key,name in [('appimagetool','appimagetool.AppImage'),('appimage_runtime','appimage-runtime-x86_64')]:
    path=Path('.cache')/name; path.parent.mkdir(exist_ok=True)
    if not path.exists():
        urllib.request.urlretrieve(pins[key+'_url'],path)
    if hashlib.sha256(path.read_bytes()).hexdigest()!=pins[key+'_sha256']:
        raise SystemExit(f'Pinned {key} checksum mismatch: {path}')
    path.chmod(0o755)
PY
    bash packaging/appimage.sh .cache/appimagetool.AppImage "$bundle" "$bundle.AppImage"
    mkdir -p "$output/sources" "$output/reports/checksums"
    cp "$bundle.tar.gz" "$bundle.AppImage" "$output/packages/"
    cp "dist/release/croccospectrum-$version-source.tar.gz" dist/release/dependency-sources.tar.gz "$output/sources/"
    cp dist/release/*.sha256 "$output/reports/checksums/"
    cp "$bundle/bundle-manifest.json" "$output/reports/"
fi

# Verify downloads with basename-relative checksum files on either forge.
python3 - <<'PY'
from pathlib import Path
import hashlib,json,os
output=Path(os.environ['RF_CI_OUTPUT']); packages=output/'packages'
for checksum in [*packages.glob('*.sha256'), *(output/'reports/checksums').glob('*.sha256')]:
    digest,name=checksum.read_text().strip().split(maxsplit=1)
    name=Path(name).name
    archive=packages/name if (packages/name).is_file() else output/'sources'/name
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest:
        raise SystemExit(f'Artifact checksum mismatch: {name}')
    checksum.write_text(f'{digest}  {name}\n')
PY
