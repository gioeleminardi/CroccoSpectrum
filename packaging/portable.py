#!/usr/bin/env python3
"""Bundle trusted build outputs and their ELF dependencies; build-time only.

Never invoke ldd on an untrusted recording. Only the executables we built and
Qt plugins from the selected SDK enter this traversal. The OS kernel, libc,
display server, and GPU driver modules intentionally remain host facilities.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("--build", type=pathlib.Path, required=True)
parser.add_argument("--qt", type=pathlib.Path, required=True)
parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("dist"))
args = parser.parse_args()
source = pathlib.Path(__file__).resolve().parent.parent
pins = json.loads((source / "packaging/dependencies.json").read_text())
version = pins["application"]
destination = args.output.resolve() / f"croccospectrum-{version}-x86_64"
if destination.exists():
    raise SystemExit(f"Refusing to replace {destination}; remove the old generated bundle explicitly")
destination.mkdir(parents=True)
lib = destination / "lib"
lib.mkdir()
manifest = {"application_name": "CroccoSpectrum", "application_version": version, "architecture": "x86_64", "build_os": pathlib.Path("/etc/os-release").read_text(), "files": []}
pending = []


def copy(origin, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origin.resolve(), target)
    manifest["files"].append({"path": str(target.relative_to(destination)), "source": str(origin), "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})


for name in ["croccospectrum", "croccospectrum-cli"]:
    binary = (args.build / name).resolve()
    copy(binary, destination / "bin" / name)
    pending.append(binary)

plugin_groups = ["platforms", "imageformats", "platforminputcontexts", "xcbglintegrations", "wayland-decoration-client", "wayland-graphics-integration-client", "wayland-shell-integration", "tls"]
for group in plugin_groups:
    for plugin in sorted((args.qt / "plugins" / group).glob("*.so")):
        if group == "platforms" and plugin.name not in {"libqxcb.so", "libqwayland.so", "libqwayland-generic.so", "libqwayland-egl.so", "libqoffscreen.so", "libqminimal.so"}:
            continue  # Desktop launchers do not need embedded/VNC platform backends.
        copy(plugin, destination / "plugins" / group / plugin.name)
        pending.append(plugin)

# Qt's OpenSSL backend dlopens these libraries, so ldd alone cannot find them.
tls_backend = args.qt / "plugins/tls/libqopensslbackend.so"
if not tls_backend.is_file():
    raise SystemExit("The Qt SDK must provide the OpenSSL TLS backend")
openssl_directory = pathlib.Path("/usr/lib/x86_64-linux-gnu")
for name in ["libssl.so.3", "libcrypto.so.3"]:
    dependency = openssl_directory / name
    copy(dependency, lib / name)
    pending.append(dependency)

# qt.conf overrides the SDK's absolute build paths. No host Qt plugins should
# be loaded just because the developer happened to install a different Qt.
(destination / "bin" / "qt.conf").write_text("[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\n")
skip = re.compile(r"^(?:ld-linux.*|libc\.so.*|libm\.so.*|libdl\.so.*|libpthread\.so.*|librt\.so.*|libresolv\.so.*)$")
visited = set()
environment = dict(os.environ)
environment["LD_LIBRARY_PATH"] = str((args.qt / "lib").resolve()) + ":" + environment.get("LD_LIBRARY_PATH", "")
while pending:
    binary = pending.pop()
    if binary.resolve() in visited:
        continue
    visited.add(binary.resolve())
    result = subprocess.run(["ldd", str(binary)], env=environment, text=True, capture_output=True, check=True)
    if "not found" in result.stdout:
        raise SystemExit(f"Unresolved dependencies in {binary}:\n{result.stdout}")
    for name, location in re.findall(r"^\s*(\S+) => (/\S+)", result.stdout, re.MULTILINE):
        if skip.match(name):
            continue
        target = lib / name
        dependency = pathlib.Path(location)
        if not target.exists():
            copy(dependency, target)
            pending.append(dependency)

for name in ["README.md", "CHANGELOG.md", "FEATURE_STATUS.md", "LICENSE"]:
    copy(source / name, destination / "share" / "doc" / name)
copy(source / "packaging" / "dependencies.json", destination / "share" / "doc" / "dependencies.json")
copy(source / "packaging" / "croccospectrum.desktop", destination / "croccospectrum.desktop")
copy(source / "assets" / "CroccoSpectrumIcon.png", destination / "croccospectrum.png")

# Carry notices for every system package providing a copied file. The complete
# source is a separate release artifact/provision; copying notices alone is not
# represented as satisfying all GPL/LGPL redistribution obligations.
package_names = set()
for entry in manifest["files"]:
    owner = subprocess.run(["dpkg-query", "-S", str(pathlib.Path(entry["source"]).resolve())], capture_output=True, text=True)
    if owner.returncode == 0:
        package_names.update(line.split(": ", 1)[0].split(":", 1)[0] for line in owner.stdout.splitlines())
for package in sorted(package_names):
    notices = pathlib.Path("/usr/share/doc") / package
    if (notices / "copyright").exists():
        copy(notices / "copyright", destination / "share" / "licenses" / package / "copyright")
    version = subprocess.run(["dpkg-query", "-W", "-f=${Version}", package], text=True, capture_output=True)
    manifest.setdefault("system_packages", {})[package] = version.stdout
    provider = subprocess.run(["dpkg-query", "-W", "-f=${source:Package}\t${source:Version}", package], text=True, capture_output=True, check=True).stdout
    source_package, source_version = provider.split("\t")
    manifest.setdefault("source_packages", {})[source_package] = source_version
for notice in args.qt.rglob("LICENSE*"):
    if notice.is_file():
        copy(notice, destination / "share" / "licenses" / "qt-sdk" / notice.relative_to(args.qt))
for notice in args.qt.rglob("LGPL_EXCEPTION*"):
    if notice.is_file():
        copy(notice, destination / "share" / "licenses" / "qt-sdk" / notice.relative_to(args.qt))
font = pathlib.Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
if font.exists():
    copy(font, destination / "fonts" / font.name)
    copy(pathlib.Path("/usr/share/doc/fonts-dejavu-core/copyright"), destination / "share" / "licenses" / "dejavu" / "copyright")

(destination / "fonts" / "fonts.conf").write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd"><fontconfig><dir prefix="relative">.</dir><cachedir prefix="xdg">croccospectrum/fontconfig</cachedir></fontconfig>\n')

launcher = '''#!/bin/sh
set -eu
APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$APP_DIR/plugins"
export QT_QPA_FONTDIR="$APP_DIR/fonts"
export FONTCONFIG_FILE="$APP_DIR/fonts/fonts.conf"
unset QT_QPA_PLATFORM_PLUGIN_PATH
exec "$APP_DIR/bin/croccospectrum" "$@"
'''
(destination / "AppRun").write_text(launcher)
(destination / "AppRun").chmod(0o755)
(destination / "croccospectrum-cli").write_text(launcher.replace('bin/croccospectrum"', 'bin/croccospectrum-cli"'))
(destination / "croccospectrum-cli").chmod(0o755)
(destination / "bundle-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
archive = destination.parent / (destination.name + ".tar.gz")
with tarfile.open(archive, "w:gz") as tar:
    tar.add(destination, arcname=destination.name)
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
(archive.parent / (archive.name + ".sha256")).write_text(f"{digest}  {archive.name}\n")
print(f"Created {archive}; {len(manifest['files'])} bundled files")
