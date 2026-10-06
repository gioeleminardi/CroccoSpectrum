#!/usr/bin/env python3
"""Refresh docs/manifests and archives after source provisions/qualification."""
import argparse
import hashlib
import json
import pathlib
import shutil
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("--bundle", type=pathlib.Path, required=True)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
bundle = args.bundle.resolve()
manifest_path = bundle / "bundle-manifest.json"
manifest = json.loads(manifest_path.read_text())
previous = {item["path"]: item for item in manifest["files"]}
for filename in ["README.md", "CHANGELOG.md", "FEATURE_STATUS.md", "LICENSE"]:
    shutil.copy2(root / filename, bundle / "share/doc" / filename)
shutil.copytree(root / "docs", bundle / "share/doc/docs", dirs_exist_ok=True)
shutil.copy2(root / "packaging/dependencies.json", bundle / "share/doc/dependencies.json")
manifest["files"] = []
for file in sorted(bundle.rglob("*")):
    if not file.is_file() or file.is_symlink() or file == manifest_path:
        continue
    relative = str(file.relative_to(bundle))
    entry = previous.get(relative, {"path": relative, "source": "generated launcher/configuration or accompanying source provision"})
    entry["sha256"] = hashlib.sha256(file.read_bytes()).hexdigest()
    manifest["files"].append(entry)
manifest["source_provisions"] = "../dependency-sources/sources.json"
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

def archive(directory, output):
    with tarfile.open(output, "w:gz") as tar:
        tar.add(directory, arcname=directory.name)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + ".sha256").write_text(f"{digest}  {output.name}\n")

archive(bundle, bundle.parent / (bundle.name + ".tar.gz"))
archive(bundle.parent / "dependency-sources", bundle.parent / "dependency-sources.tar.gz")
source_name = f"croccospectrum-{manifest['application_version']}"
source_archive = bundle.parent / (source_name + "-source.tar.gz")
with tarfile.open(source_archive, "w:gz") as tar:
    for path in sorted(root.iterdir()):
        if path.name in {"build", "dist", ".cache", ".git", "aqtinstall.log"}:
            continue
        tar.add(path, arcname=source_name + "/" + path.name, filter=lambda entry: None if "__pycache__" in entry.name else entry)
digest = hashlib.sha256(source_archive.read_bytes()).hexdigest()
source_archive.with_name(source_archive.name + ".sha256").write_text(f"{digest}  {source_archive.name}\n")
print(f"Updated binary/source archives and {len(manifest['files'])} manifest hashes")
