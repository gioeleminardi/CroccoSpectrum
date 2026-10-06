#!/bin/sh
# Supply a reviewed/pinned appimagetool binary; do not silently download tools
# during packaging. A portable folder is also shipped for no-FUSE systems.
set -eu
if [ "$#" -ne 3 ]; then
    printf 'Usage: appimage.sh APPIMAGETOOL APPDIR OUTPUT.AppImage\n' >&2
    exit 1
fi
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNTIME="$SCRIPT_DIR/../.cache/appimage-runtime-x86_64"
EXPECTED=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["appimage_runtime_sha256"])' "$SCRIPT_DIR/dependencies.json")
printf '%s  %s\n' "$EXPECTED" "$RUNTIME" | sha256sum --check
# appimagetool writes its embedded digest into the runtime it receives. Work
# on a disposable copy so the checksum-pinned download survives repeated builds.
RUNTIME_DIR=$(mktemp -d)
trap 'rm -rf "$RUNTIME_DIR"' EXIT HUP INT TERM
cp "$RUNTIME" "$RUNTIME_DIR/runtime-x86_64"
ARCH=x86_64 "$1" --appimage-extract-and-run --runtime-file "$RUNTIME_DIR/runtime-x86_64" "$2" "$3"
sha256sum "$3" > "$3.sha256"
