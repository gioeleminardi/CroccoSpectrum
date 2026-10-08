#!/usr/bin/env bash
# CI runner entry point. Docker cp/streaming avoids
# assuming that a containerized runner's workspace path exists on its host.
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:?Usage: bash ci/build.sh release|debug|asan}
case "$mode" in release|debug|asan) ;; *) exit 2 ;; esac
engine=${RF_CI_ENGINE:-docker}
output="$PWD/dist/ci/$mode"
rm -rf "$output/packages" "$output/sources" "$output/reports/checksums"
mkdir -p "$output/reports" "$output/packages"
containers=()
cleanup() {
    # Recover CTest diagnostics even if compilation, testing or packaging fails.
    if [[ -n ${builder:-} ]]; then
        "$engine" cp "$builder:/workspace/dist/ci/$mode/reports/." "$output/reports/" 2>/dev/null || true
    fi
    for container in "${containers[@]}"; do
        "$engine" rm -f "$container" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# Refresh Ubuntu packages on persistent runners so their exact source versions
# remain available in the live indexes used by release packaging.
"$engine" build --no-cache -t "croccospectrum-ci:$mode" -f packaging/Containerfile . \
    2>&1 | tee "$output/reports/container-build.log"
builder=$("$engine" create -w /workspace -e LANG=C.UTF-8 \
    -e RF_CI_COMMIT="${RF_CI_COMMIT:-$(git rev-parse HEAD)}" \
    -e RF_CI_JOBS="${RF_CI_JOBS:-2}" "croccospectrum-ci:$mode" sleep infinity)
containers+=("$builder")
"$engine" start "$builder" >/dev/null
tar --exclude=./.git --exclude=./.cache --exclude=./build --exclude=./dist \
    --exclude='*/__pycache__' --exclude=./aqtinstall.log -cf - . \
    | "$engine" exec -i "$builder" tar -xf -
"$engine" exec "$builder" bash ci/run.sh "$mode" 2>&1 | tee "$output/reports/build.log"
"$engine" cp "$builder:/workspace/dist/ci/$mode/packages/." "$output/packages/"
if [[ $mode == release ]]; then
    mkdir -p "$output/sources"
    "$engine" cp "$builder:/workspace/dist/ci/$mode/sources/." "$output/sources/"
fi
"$engine" cp "$builder:/workspace/dist/ci/$mode/reports/." "$output/reports/"

if [[ $mode == release ]]; then
    # Test final packages without installing libraries in the target containers.
    # Copy through the Docker API so this also works with containerized runners.
    staging=$(mktemp -d)
    trap 'cleanup; rm -rf "$staging"' EXIT
    tar -xzf "$output/packages/"*-x86_64.tar.gz -C "$staging"
    for image in docker.io/library/ubuntu:24.04 docker.io/library/ubuntu:26.04 registry.fedoraproject.org/fedora:44; do
        target=${image##*/}
        target=${target/:/-}
        container=$("$engine" create --network none -e LANG=C.UTF-8 \
            -e QT_QPA_PLATFORM=offscreen -e XDG_CONFIG_HOME=/tmp/config "$image" \
            sh -c '/opt/crocco/croccospectrum-cli inspect --file /tmp/signal.iq && /opt/crocco/AppRun --accept-defaults --smoke-test /tmp/signal.iq && /opt/CroccoSpectrum.AppImage --appimage-extract-and-run --accept-defaults --smoke-test /tmp/signal.iq')
        containers+=("$container")
        "$engine" cp "$staging/"croccospectrum-*-x86_64 "$container:/opt/crocco"
        "$engine" cp "$output/packages/"*.AppImage "$container:/opt/CroccoSpectrum.AppImage"
        "$engine" cp "$output/reports/signal.iq" "$container:/tmp/signal.iq"
        "$engine" start -a "$container" 2>&1 | tee "$output/reports/smoke-$target.log"
        [[ $("$engine" inspect -f '{{.State.ExitCode}}' "$container") == 0 ]]
    done
fi
