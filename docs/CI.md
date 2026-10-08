# CI builds and downloadable artifacts

GitHub is the primary host and runs `.github/workflows/ci.yml`.
The workflow runs on pushes to `devel` and `main`, pull requests targeting either
branch, manual dispatch, and publication of a release or pre-release. Tag pushes
do not trigger a build. It uses the CI scripts and Ubuntu 24.04 build container,
regardless of the runner's host OS.

## Downloads

Open **Actions → a workflow run → Artifacts** on GitHub.
Successful builds upload these artifacts with a 30-day requested retention:

| Artifact | Contents |
| --- | --- |
| `CroccoSpectrum-linux-amd64-AppImage` | One runnable `croccospectrum-VERSION-x86_64.AppImage` |
| `CroccoSpectrum-linux-amd64-portable` | One extract-and-run `croccospectrum-VERSION-x86_64.tar.gz` with GUI, CLI, and bundled dependencies |
| `CroccoSpectrum-linux-amd64-sources` | Complete application source and dependency source archives |
| `reports-release` | Build logs, CTest log/JUnit XML, build identity, dependency manifest, package/source SHA-256 checksums, and distribution smoke logs |

The GitHub workflow builds Release once, runs all tests and package checks, then
uploads the artifacts. It compiles the CMake ALL target: GUI, CLI, both static
libraries, both test tools, and the sustained-use test executable. Long-duration
stress testing runs separately from routine CI. TSAN with instrumented Qt is a separate testing
procedure documented in [TESTING.md](TESTING.md).

Debug and ASAN/UBSAN builds remain available through the local pipeline commands
below. Development archives require matching Qt/FFTW/compiler runtimes. For a
portable application, use the Release AppImage or folder archive, which includes
both the GUI and CLI with their dependencies. Release packages are checked in clean,
network-disabled Ubuntu 24.04/26.04 and Fedora 44 containers before upload.

Artifacts are ZIP containers around the listed files. Extract that ZIP, then
extract a tarball to preserve executable permissions; for a standalone AppImage,
run `chmod +x` before launching. Release checksums are in the `checksums/` folder
of `reports-release`; copy the matching `.sha256` beside a downloaded file and
run `sha256sum --check FILE.sha256`. Keep application and dependency sources with
redistributed binaries; see [THIRD_PARTY.md](THIRD_PARTY.md).

After extracting the artifact ZIP, launch the AppImage:

```sh
chmod +x croccospectrum-VERSION-x86_64.AppImage
./croccospectrum-VERSION-x86_64.AppImage
```

Or extract the portable tarball and launch its bundled GUI or CLI:

```sh
tar -xzf croccospectrum-VERSION-x86_64.tar.gz
./croccospectrum-VERSION-x86_64/AppRun
./croccospectrum-VERSION-x86_64/croccospectrum-cli --help
```

Replace `VERSION` with the version in the downloaded filename. Neither package
requires installing Qt or FFTW. The AppImage also supports
`--appimage-extract-and-run` on hosts without FUSE.

Downloads use the hosting service's normal sign-in/access policy. Server retention
limits can override the requested 30 days for workflow artifacts. Published
releases and pre-releases also receive the AppImage, portable tarball, application
and dependency source archives, and their SHA-256 checksums as release assets.
The upload job waits for the build job to succeed and uses artifacts from that
same run. Reports remain available under Actions. The pipeline does not create
releases or publish to a package registry.

## Publish a release or pre-release

Use `devel` for development and `main` for release-ready code.

1. Update the application version in `CMakeLists.txt` and
   `packaging/dependencies.json` together when preparing a new version. The tag
   name does not change the version embedded in the application or package names.
2. Merge the release-ready changes from `devel` into `main`. The tagged commit
   must contain the updated workflow.
3. On GitHub, open **Releases → Draft a new release**, choose a new tag, and select
   `main` as the target. Add the title and release notes; select **Set as a
   pre-release** when appropriate.
4. Publish the release and wait for **Build, test and package** to finish. The
   build tests the tagged commit, then the upload job attaches the files to the
   release's **Assets** section automatically.

Only release events run the upload job. Pushes, pull requests, and manual runs
produce Actions artifacts without attaching files to a release. Existing
releases are not updated automatically. Uploads do not overwrite existing assets
with the same filename; remove those assets before rerunning a failed upload.

This workflow requires releases that allow asset uploads after publication.
If [immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases)
are enabled, assets must be attached to a draft before publishing, which requires
a different publishing workflow.

## Runner setup

GitHub uses its `ubuntu-24.04` hosted runner.

The source and packages are copied through the container API. A job container's
workspace therefore does not need to exist at the same path on the Docker host.
The release upload uses the built-in `GITHUB_TOKEN`; no custom release secret is
required. Only the upload job receives `contents: write`; the build job keeps
`contents: read`. Use isolated runners for untrusted pull requests, as with other
jobs that compile and execute repository code.

CI rebuilds the Ubuntu builder without the container layer cache on every run.
Persistent runners can otherwise retain older binary dependencies after Ubuntu
mirrors stop indexing their exact source versions. Dependency installation runs
each time to keep the bundled libraries and accompanying sources aligned.

Builds fetch the pinned Qt SDK and checksum-verified packaging tools. The runtime
asset has an upstream continuous URL; if it changes, CI fails the pinned hash
check. Review and update the runtime binary hash, commit, source pins, and tool
compatibility together rather than bypassing verification.

Upstream source downloads enter the cache only after SHA-256 verification.
Invalid cached sources are downloaded again. Downloads get up to three attempts
before a persistent mismatch fails with the expected and received
hashes and source URL. zlib uses its official versioned GitHub release asset
with the checksum published by upstream.

## Run the pipeline locally

```sh
bash ci/build.sh debug
bash ci/build.sh asan
bash ci/build.sh release
```

Release `packages/` contains exactly the AppImage and portable tarball; source
archives are in `sources/` and their checksums are in `reports/checksums/`.
Debug/ASAN packages and reports appear under `dist/ci/CONFIGURATION/`. A fresh
checkout is recommended. `RF_CI_JOBS` controls compile parallelism (default 2).
For local checks on a Podman workstation, `RF_CI_ENGINE=podman` selects that CLI.

Every dependency source provision is collected before Release upload. Package
names read the application version from `packaging/dependencies.json`; CI checks
that it matches the CMake project version. Actions are pinned to immutable commit
IDs. Diagnostics are uploaded after a failure; binary packages are uploaded only
after that job's complete build/test/verification sequence succeeds.

References: [GitHub artifact action](https://github.com/actions/upload-artifact).
