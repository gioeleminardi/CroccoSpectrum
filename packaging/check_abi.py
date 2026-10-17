#!/usr/bin/env python3
"""Reject bundled ELF files that need a newer host glibc than the pinned baseline."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def check_glibc(directory, maximum):
    limit = tuple(map(int, maximum.split(".")))
    highest = (0,)
    count = 0
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        count += 1
        versions = subprocess.check_output(
            ["readelf", "--version-info", "--wide", str(path)],
            text=True, env={**os.environ, "LC_ALL": "C"})
        # Version definitions describe what a library provides. Only version
        # needs constrain the host, including non-symbol ABI tags such as RELR.
        needs = versions.partition("Version needs section")[2]
        for version in re.findall(r"\bName: GLIBC_(\S+)", needs):
            if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
                raise ValueError(f"{path.relative_to(directory)} requires unsupported "
                                 f"GLIBC_{version}")
            required = tuple(map(int, version.split(".")))
            if required > limit:
                raise ValueError(f"{path.relative_to(directory)} requires GLIBC_{version}; "
                                 f"the package baseline is glibc {maximum}")
            highest = max(highest, required)
    if not count:
        raise ValueError(f"No ELF files found in {directory}")
    return count, ".".join(map(str, highest))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    pins = json.loads((Path(__file__).parent / "dependencies.json").read_text())
    try:
        count, highest = check_glibc(args.bundle, pins["minimum_host_glibc"])
    except ValueError as error:
        raise SystemExit(str(error)) from error
    print(f"Checked {count} ELF files: maximum required glibc {highest}; "
          f"baseline {pins['minimum_host_glibc']}")
