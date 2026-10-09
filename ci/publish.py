#!/usr/bin/env python3
"""Validate release tags and publish verified artifacts, uploading to a draft first."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def api(path, method="GET", data=None, missing_ok=False):
    command = ["gh", "api", path, "--method", method]
    if data is not None:
        command += ["--input", "-"]
    result = subprocess.run(command, input=json.dumps(data) if data is not None else None,
                            text=True, capture_output=True)
    if result.returncode:
        if missing_ok and "HTTP 404" in result.stderr:
            return None
        raise RuntimeError(result.stderr.strip())
    return json.loads(result.stdout)


def release_for_tag(tag):
    # gh also finds drafts, which the REST release-by-tag endpoint can omit.
    result = subprocess.run(["gh", "release", "view", tag, "--json", "apiUrl"],
                            text=True, capture_output=True)
    if result.returncode:
        if "release not found" in result.stderr.lower():
            return None
        raise RuntimeError(result.stderr.strip())
    return api(json.loads(result.stdout)["apiUrl"])


def changelog_section(changelog, heading):
    match = re.search(r"^## " + re.escape(heading) + r"(?:[ \t][^\n]*)?\n(.*?)(?=^## |\Z)",
                      changelog, re.MULTILINE | re.DOTALL)
    if not match or not match[1].strip():
        raise ValueError(f"CHANGELOG.md needs a nonempty '{heading}' section")
    return match[1].strip()


def validate_tag(tag, root):
    base = re.search(r"project\(CroccoSpectrum VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b",
                     (root / "CMakeLists.txt").read_text())[1]
    if tag != "v" + base:
        raise ValueError(f"Stable tag must match the CMake version: v{base}")
    changelog_section((root / "CHANGELOG.md").read_text(), base)
    subprocess.run(["git", "merge-base", "--is-ancestor", "HEAD", "origin/main"],
                   cwd=root, check=True)


def release_files(assets, info):
    version = info["version"]
    files = [
        assets / "CroccoSpectrum-linux-amd64-AppImage" / f"croccospectrum-{version}-x86_64.AppImage",
        assets / "CroccoSpectrum-linux-amd64-portable" / f"croccospectrum-{version}-x86_64.tar.gz",
        assets / "CroccoSpectrum-linux-amd64-sources" / f"croccospectrum-{version}-source.tar.gz",
        assets / "CroccoSpectrum-linux-amd64-sources" / "dependency-sources.tar.gz",
    ]
    checksums = []
    for file in files:
        checksum = assets / "reports-release/checksums" / (file.name + ".sha256")
        digest, name = checksum.read_text().strip().split(maxsplit=1)
        with file.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if name != file.name or digest != actual:
            raise ValueError(f"Checksum mismatch for {file.name}")
        checksums.append(checksum)
    return files + checksums + [assets / "reports-release/BUILD-INFO.json"]


def require_uploaded(release, files):
    uploaded = {asset["name"] for asset in release["assets"]
                if asset["state"] == "uploaded" and asset["size"] > 0}
    missing = {file.name for file in files} - uploaded
    if missing:
        raise ValueError(f"Release assets are incomplete: {', '.join(sorted(missing))}")


def publish(assets, changelog, repository):
    info = json.loads((assets / "reports-release/BUILD-INFO.json").read_text())
    development = info["channel"] == "development"
    commit = info["commit"]
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
        raise ValueError("Publishing requires the full built commit")
    expected = info["base_version"]
    if development:
        if not re.fullmatch(r"[1-9][0-9]*", info["build_number"]):
            raise ValueError("Publishing requires a CI build number")
        expected += f"-dev.{info['build_number']}.g{commit[:7]}"
    elif info["channel"] != "stable":
        raise ValueError("Unknown release channel")
    if info["version"] != expected:
        raise ValueError("Build version does not match its release identity")
    files = release_files(assets, info)
    tag = "v" + expected
    prefix = f"repos/{repository}"
    ref = api(f"{prefix}/git/ref/tags/{tag}", missing_ok=True)
    if ref and api(f"{prefix}/commits/{tag}")["sha"] != commit:
        raise ValueError(f"Refusing to replace tag {tag} on a different commit")
    existing = release_for_tag(tag)
    if existing and not existing["draft"]:
        if existing["prerelease"] != development:
            raise ValueError("Published release has the wrong channel")
        require_uploaded(existing, files)
        print(f"{tag} is already published; keeping its original assets")
        return
    heading = info["base_version"]
    if development and re.search(r"^## Unreleased(?:[ \t][^\n]*)?\n", changelog, re.MULTILINE):
        heading = "Unreleased"
    notes = changelog_section(changelog, heading)
    version_key = lambda value: tuple(map(int, value.removeprefix("v").split(".")))
    current_version = version_key(info["base_version"])
    previous = api(f"{prefix}/releases/latest", missing_ok=True)
    previous_version = version_key(previous["tag_name"]) if previous else None
    for section in re.finditer(r"^## ([0-9]+\.[0-9]+\.[0-9]+)(?:[ \t][^\n]*)?\n.*?(?=^## |\Z)",
                               changelog, re.MULTILINE | re.DOTALL):
        version = version_key(section[1])
        if version < current_version and (previous_version is None or version > previous_version):
            notes += "\n\n" + section[0].strip()
    body = (f"{'Development build from devel' if development else 'Stable release'}\n\n"
            f"Version: {expected}\nCommit: {commit}\nBuild date: {info['built_at']}\n"
            f"CI run: {info['run_url']}\n\n{notes}\n")
    metadata = {"name": f"Development build {expected}" if development else tag,
                "body": body, "prerelease": development}
    if not ref:
        api(f"{prefix}/git/refs", "POST", {"ref": f"refs/tags/{tag}", "sha": commit})
    if existing:
        release = api(f"{prefix}/releases/{existing['id']}", "PATCH", metadata)
    else:
        release = api(f"{prefix}/releases", "POST",
                      {**metadata, "tag_name": tag, "target_commitish": commit, "draft": True})
    subprocess.run(["gh", "release", "upload", tag, *map(str, files), "--clobber"], check=True)
    require_uploaded(api(f"{prefix}/releases/{release['id']}"), files)
    # A prerelease never becomes Latest. An older stable rerun must not displace
    # a newer stable release that finished first.
    latest = api(f"{prefix}/releases/latest", missing_ok=True) if not development else None
    make_latest = "false"
    if not development:
        if latest is None or version_key(tag) > version_key(latest["tag_name"]):
            make_latest = "true"
    api(f"{prefix}/releases/{release['id']}", "PATCH",
        {"draft": False, "make_latest": make_latest})
    print(f"Published {tag} from {commit}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate-tag")
    validate.add_argument("tag")
    upload = commands.add_parser("publish")
    upload.add_argument("--assets", type=Path, default=Path("release-assets"))
    args = parser.parse_args()
    try:
        if args.command == "validate-tag":
            validate_tag(args.tag, Path.cwd())
        else:
            publish(args.assets, Path("CHANGELOG.md").read_text(), os.environ["GH_REPO"])
    except (ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
