#!/usr/bin/env python3
"""Deterministic bounded parser stress, complementary to numeric fixtures."""
import copy
import json
import pathlib
import random
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
randomizer = random.Random(20261006)
with tempfile.TemporaryDirectory(prefix="rf-parser-") as temporary:
    directory = pathlib.Path(temporary)
    (directory / "input.sigmf-data").write_bytes(bytes(16384))
    metadata = directory / "input.sigmf-meta"
    valid = {"global": {"core:datatype": "ci16_le", "core:sample_rate": 1e8, "core:version": "1.2.6"},
             "captures": [{"core:sample_start": 0}], "annotations": []}
    malformed = [None, [], {}, "bad", True, -1, 1.5, "18446744073709551616", 1e100]
    for index in range(300):
        root = copy.deepcopy(valid)
        if index % 3 == 0:
            metadata.write_bytes(randomizer.randbytes(randomizer.randrange(0, 4096)))
        else:
            field = randomizer.choice(["global", "captures", "annotations", "rate", "start", "datatype", "extensions"])
            value = randomizer.choice(malformed)
            if field in {"global", "captures", "annotations"}:
                root[field] = value
            elif field == "start":
                root["captures"][0]["core:sample_start"] = value
            else:
                root["global"]["core:" + {"rate": "sample_rate"}.get(field, field)] = value
            metadata.write_text(json.dumps(root))
        result = subprocess.run([str(binary), "inspect", "--file", str(metadata)], capture_output=True, timeout=10)
        assert result.returncode in {0, 1}, f"Parser crashed: {result.returncode}, {result.stderr!r}"
        if index % 3 == 0:
            assert result.returncode == 1, "random non-JSON metadata accepted"
    metadata.write_bytes(b" " * (4 * 1024 * 1024 + 1))
    result = subprocess.run([str(binary), "inspect", "--file", str(metadata)], capture_output=True, timeout=10)
    assert result.returncode == 1, "oversized metadata accepted"
print("Passed 301 bounded metadata-parser stress cases")
