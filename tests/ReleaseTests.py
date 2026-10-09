#!/usr/bin/env python3
"""Offline checks for build identities and draft-before-publication behavior."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("publish", ROOT / "ci/publish.py")
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)
COMMIT = "233be51" + "a" * 33


class BuildVersionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\n'
            'project(CroccoSpectrum VERSION 0.2.0 LANGUAGES NONE)\n'
            f'include("{ROOT}/cmake/BuildVersion.cmake")\n')

    def configure(self, **variables):
        result = subprocess.run(["cmake", "-G", "Ninja", "-S", str(self.root), "-B", str(self.root / "build"),
                                 *[f"-D{key}={value}" for key, value in variables.items()]],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads((self.root / "build/BUILD-INFO.json").read_text())

    def test_successive_ci_builds_share_one_identity(self):
        for number in (184, 185):
            info = self.configure(RF_BUILD_COMMIT=COMMIT, RF_BUILD_NUMBER=number,
                                  RF_BUILD_DATE="2026-10-09T12:00:00Z")
            self.assertEqual(info["version"], f"0.2.0-dev.{number}.g233be51")
            self.assertEqual(info["base_version"], "0.2.0")
            self.assertEqual(info["commit"], COMMIT)
            self.assertEqual(info["channel"], "development")
            self.assertEqual(info["built_at"], "2026-10-09T12:00:00Z")

    def test_stable_build_has_no_suffix(self):
        info = self.configure(RF_BUILD_COMMIT=COMMIT, RF_BUILD_CHANNEL="stable",
                              RF_BUILD_NUMBER=186)
        self.assertEqual(info["version"], "0.2.0")
        self.assertEqual(info["channel"], "stable")

    def test_local_modified_build_is_identifiable(self):
        info = self.configure(RF_BUILD_COMMIT=COMMIT, RF_BUILD_DIRTY="ON")
        self.assertEqual(info["version"], "0.2.0-dev.local.g233be51.dirty")

    def test_source_without_git_is_still_development(self):
        info = self.configure()
        self.assertEqual(info["version"], "0.2.0-dev.local.gunknown")

    def test_invalid_identity_is_rejected(self):
        result = subprocess.run(["cmake", "-G", "Ninja", "-S", str(self.root), "-B", str(self.root / "build"),
                                 "-DRF_BUILD_COMMIT=short"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("full Git commit hash", result.stdout + result.stderr)


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.assets = Path(self.temporary.name)
        self.info = {"base_version": "0.2.0", "version": "0.2.0-dev.184.g233be51",
                     "channel": "development", "commit": COMMIT, "build_number": "184",
                     "built_at": "2026-10-09T12:00:00Z", "run_url": "https://github.com/o/r/actions/runs/1"}
        self.changelog = "## Unreleased\n\n- New feature.\n\n## 0.2.0 — 2026-10-09\n\n- Final feature.\n"
        self.ref = None
        self.release = None
        self.latest = None
        self.calls = []
        self.make_assets()
        self.addCleanup(patch.stopall)
        patch.object(publisher, "api", side_effect=self.api).start()
        patch.object(publisher, "release_for_tag", side_effect=lambda tag: copy.deepcopy(self.release)).start()
        self.upload = patch.object(publisher.subprocess, "run", side_effect=self.upload_files).start()

    def make_assets(self):
        report = self.assets / "reports-release"
        (report / "checksums").mkdir(parents=True, exist_ok=True)
        (report / "BUILD-INFO.json").write_text(json.dumps(self.info))
        version = self.info["version"]
        for directory, filename in [
            ("CroccoSpectrum-linux-amd64-AppImage", f"croccospectrum-{version}-x86_64.AppImage"),
            ("CroccoSpectrum-linux-amd64-portable", f"croccospectrum-{version}-x86_64.tar.gz"),
            ("CroccoSpectrum-linux-amd64-sources", f"croccospectrum-{version}-source.tar.gz"),
            ("CroccoSpectrum-linux-amd64-sources", "dependency-sources.tar.gz"),
        ]:
            file = self.assets / directory / filename
            file.parent.mkdir(exist_ok=True)
            file.write_bytes(b"verified artifact")
            digest = hashlib.sha256(file.read_bytes()).hexdigest()
            (report / "checksums" / (filename + ".sha256")).write_text(f"{digest}  {filename}\n")

    def api(self, path, method="GET", data=None, missing_ok=False):
        self.calls.append((path, method, data))
        if "/git/ref/tags/" in path:
            return self.ref
        if "/commits/" in path:
            return {"sha": self.ref["object"]["sha"]}
        if path.endswith("/git/refs"):
            self.ref = {"object": {"sha": data["sha"]}}
            return self.ref
        if path.endswith("/releases/latest"):
            return self.latest
        if method == "POST":
            self.release = {**data, "id": 1, "assets": []}
        elif method == "PATCH":
            self.release.update(data)
        return copy.deepcopy(self.release)

    def upload_files(self, command, **kwargs):
        self.assertEqual(command[:3], ["gh", "release", "upload"])
        self.assertTrue(self.release["draft"])
        self.release["assets"] = [{"name": Path(file).name, "state": "uploaded", "size": 100}
                                  for file in command[4:-1]]

    def publish(self):
        publisher.publish(self.assets, self.changelog, "o/r")

    def test_complete_assets_are_uploaded_before_publication(self):
        self.publish()
        self.assertFalse(self.release["draft"])
        self.assertTrue(self.release["prerelease"])
        self.assertEqual(self.release["make_latest"], "false")
        self.assertEqual(self.ref["object"]["sha"], COMMIT)
        self.assertEqual(len(self.release["assets"]), 9)
        self.assertIn("- New feature.", self.release["body"])
        self.assertNotIn("- Final feature.", self.release["body"])
        self.assertEqual(self.calls[-1][2], {"draft": False, "make_latest": "false"})

    def test_development_release_falls_back_to_matching_version_notes(self):
        self.changelog = ("## 0.2.0 — 2026-10-09\n\n- Final feature.\n\n"
                          "## 0.1.6 — 2026-10-08\n\n- Older feature.\n")
        self.latest = {"tag_name": "v0.1.6"}
        self.publish()
        self.assertFalse(self.release["draft"])
        self.assertTrue(self.release["prerelease"])
        self.assertEqual(self.release["make_latest"], "false")
        self.assertIn("- Final feature.", self.release["body"])
        self.assertNotIn("- Older feature.", self.release["body"])

    def test_development_release_requires_nonempty_matching_version_notes(self):
        for changelog in ("## 0.1.6\n\n- Older feature.\n", "## 0.2.0\n\n"):
            with self.subTest(changelog=changelog):
                self.changelog = changelog
                with self.assertRaisesRegex(ValueError, "nonempty '0[.]2[.]0' section"):
                    self.publish()
                self.assertIsNone(self.ref)
                self.assertIsNone(self.release)
                self.upload.assert_not_called()

    def test_development_release_rejects_empty_unreleased_notes(self):
        self.changelog = "## Unreleased\n\n## 0.2.0\n\n- Final feature.\n"
        with self.assertRaisesRegex(ValueError, "nonempty 'Unreleased' section"):
            self.publish()
        self.assertIsNone(self.ref)
        self.assertIsNone(self.release)
        self.upload.assert_not_called()

    def test_release_notes_include_versions_since_latest_stable(self):
        for channel, heading in (("stable", "0.3.1"),
                                 ("development", "Unreleased"),
                                 ("development", "0.3.1")):
            with self.subTest(channel=channel, heading=heading):
                self.ref = self.release = None
                version = "0.3.1" if channel == "stable" else "0.3.1-dev.184.g233be51"
                self.info.update(base_version="0.3.1", version=version, channel=channel)
                self.make_assets()
                self.latest = {"tag_name": "v0.2.0"}
                self.changelog = (f"## {heading}\n\n- Current feature.\n\n"
                                  "## 0.4.0\n\n- Future feature.\n\n"
                                  "## 0.3.0 — 2026-10-09 — Faster analysis\n\n- Skipped feature.\n\n"
                                  "## 0.2.0\n\n- Published feature.\n\n"
                                  "## 0.1.6\n\n- Older feature.\n")
                self.publish()
                body = self.release["body"]
                self.assertIn("- Current feature.", body)
                self.assertIn("## 0.3.0 — 2026-10-09 — Faster analysis", body)
                self.assertIn("- Skipped feature.", body)
                self.assertNotIn("- Future feature.", body)
                self.assertNotIn("- Published feature.", body)
                self.assertNotIn("- Older feature.", body)
                self.assertLess(body.index("- Current feature."), body.index("- Skipped feature."))
                self.assertEqual(self.release["prerelease"], channel == "development")

    def test_first_release_includes_earlier_changelog_sections(self):
        self.info.update(channel="stable", version="0.2.0")
        self.make_assets()
        self.changelog += "\n## 0.1.6\n\n- Earlier feature.\n"
        self.publish()
        self.assertIn("- Final feature.", self.release["body"])
        self.assertIn("- Earlier feature.", self.release["body"])
        self.assertNotIn("- New feature.", self.release["body"])

    def test_stable_release_requires_numbered_notes(self):
        self.info.update(channel="stable", version="0.2.0")
        self.make_assets()
        self.changelog = "## Unreleased\n\n- New feature.\n"
        with self.assertRaisesRegex(ValueError, "nonempty '0[.]2[.]0' section"):
            self.publish()
        self.assertIsNone(self.ref)
        self.assertIsNone(self.release)
        self.upload.assert_not_called()

    def test_failed_upload_remains_a_draft_and_can_be_retried(self):
        self.upload.side_effect = subprocess.CalledProcessError(1, "gh release upload")
        with self.assertRaises(subprocess.CalledProcessError):
            self.publish()
        self.assertTrue(self.release["draft"])
        self.upload.side_effect = self.upload_files
        self.publish()
        self.assertFalse(self.release["draft"])
        self.assertEqual(sum(method == "POST" and path.endswith("/releases")
                             for path, method, _ in self.calls), 1)

    def test_successful_retry_preserves_published_assets(self):
        self.publish()
        self.upload.reset_mock()
        original = copy.deepcopy(self.release)
        self.publish()
        self.upload.assert_not_called()
        self.assertEqual(self.release, original)

    def test_partial_upload_is_not_published(self):
        def partial_upload(command, **kwargs):
            self.upload_files(command, **kwargs)
            self.release["assets"].pop()
        self.upload.side_effect = partial_upload
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.publish()
        self.assertTrue(self.release["draft"])

    def test_bad_checksum_fails_before_creating_a_release(self):
        next((self.assets / "reports-release/checksums").glob("*.sha256")).write_text("wrong  file\n")
        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            self.publish()
        self.assertFalse(self.calls)
        self.upload.assert_not_called()

    def test_tag_collision_is_never_overwritten(self):
        self.ref = {"object": {"sha": "b" * 40}}
        with self.assertRaisesRegex(ValueError, "different commit"):
            self.publish()
        self.assertIsNone(self.release)

    def test_old_stable_build_does_not_replace_latest(self):
        self.info.update(channel="stable", version="0.2.0")
        self.make_assets()
        self.latest = {"tag_name": "v0.3.0"}
        self.publish()
        self.assertFalse(self.release["prerelease"])
        self.assertEqual(self.release["make_latest"], "false")
        self.assertIn("- Final feature.", self.release["body"])

    def test_new_stable_release_becomes_latest(self):
        self.info.update(channel="stable", version="0.2.0")
        self.make_assets()
        self.latest = {"tag_name": "v0.1.6"}
        self.publish()
        self.assertEqual(self.release["make_latest"], "true")

    def test_newer_stable_release_published_during_upload_remains_latest(self):
        self.info.update(channel="stable", version="0.2.0")
        self.make_assets()
        self.latest = {"tag_name": "v0.1.6"}

        def upload_with_newer_release(command, **kwargs):
            self.upload_files(command, **kwargs)
            self.latest = {"tag_name": "v0.3.0"}

        self.upload.side_effect = upload_with_newer_release
        self.publish()
        self.assertEqual(self.release["make_latest"], "false")

    def test_stable_tag_requires_matching_version_notes_and_main(self):
        root = self.assets
        (root / "CMakeLists.txt").write_text("project(CroccoSpectrum VERSION 0.2.0 LANGUAGES CXX)")
        (root / "CHANGELOG.md").write_text(self.changelog)
        self.upload.side_effect = None
        publisher.validate_tag("v0.2.0", root)
        self.upload.assert_called_once_with(
            ["git", "merge-base", "--is-ancestor", "HEAD", "origin/main"], cwd=root, check=True)
        with self.assertRaisesRegex(ValueError, "must match"):
            publisher.validate_tag("v0.3.0", root)
        (root / "CHANGELOG.md").write_text("## Unreleased\n\n- Feature.\n")
        with self.assertRaisesRegex(ValueError, "nonempty"):
            publisher.validate_tag("v0.2.0", root)
        (root / "CHANGELOG.md").write_text(self.changelog)
        self.upload.side_effect = subprocess.CalledProcessError(1, "git merge-base")
        with self.assertRaises(subprocess.CalledProcessError):
            publisher.validate_tag("v0.2.0", root)


if __name__ == "__main__":
    unittest.main()
