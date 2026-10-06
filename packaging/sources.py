#!/usr/bin/env python3
"""Collect release source provisions in the Ubuntu builder, beside binaries.

Run after portable.py, with deb-src indexes enabled. Keep exact Ubuntu patches
and .dsc files, rather than substituting a same-named upstream source release.
Downloaded source is never executed here. Archive members are read for notices
without extracting arbitrary archive paths into the application folder.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tarfile
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--bundle", type=pathlib.Path, required=True)
parser.add_argument("--cache", type=pathlib.Path, default=pathlib.Path(".cache/dependency-sources"))
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
cache = args.cache.resolve()
cache.mkdir(parents=True, exist_ok=True)
bundle = args.bundle.resolve()
manifest = json.loads((bundle / "bundle-manifest.json").read_text())
pins = json.loads((root / "packaging/dependencies.json").read_text())
provisions = bundle.parent / "dependency-sources"
provisions.mkdir(exist_ok=True)

for entry in pins["upstream_sources"]:
    archive = cache / entry["file"]
    if not archive.exists():
        urllib.request.urlretrieve(entry["url"], archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != entry["sha256"]:
        raise SystemExit(f"Source checksum mismatch: {archive}")
    shutil.copy2(archive, provisions / archive.name)
    with tarfile.open(archive) as source:
        for member in source:
            path = pathlib.PurePosixPath(member.name)
            name = path.name.upper()
            if not member.isfile() or ".." in path.parts or path.is_absolute():
                continue
            if not any(word in name for word in ["LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE", "ATTRIBUTION"]):
                continue
            target = bundle / "share/licenses/upstream" / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.extractfile(member).read())

ubuntu = cache / "ubuntu"
ubuntu.mkdir(exist_ok=True)
for package, version in sorted(manifest["source_packages"].items()):
    subprocess.run(["apt-get", "source", "--download-only", f"{package}={version}"], cwd=ubuntu, check=True)
for path in ubuntu.iterdir():
    if path.is_file():
        shutil.copy2(path, provisions / path.name)

index = {"upstream": pins["upstream_sources"], "ubuntu_packages": manifest["source_packages"], "files": []}
for path in sorted(provisions.iterdir()):
    if path.name != "sources.json" and path.is_file():
        index["files"].append({"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
(provisions / "sources.json").write_text(json.dumps(index, indent=2) + "\n")
shutil.copy2(provisions / "sources.json", bundle / "share/doc/dependency-sources.json")
print(f"Collected {len(index['files'])} source archives/patches/descriptors in {provisions}")
