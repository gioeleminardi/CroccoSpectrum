# CI builds and downloadable artifacts

GitHub is the primary host and runs `.github/workflows/ci.yml`.
The workflow runs on pushes to `devel` and `main`, pull requests targeting either
branch, manual dispatch, and stable `vX.Y.Z` tag pushes. Automatically generated
development tags are excluded. It uses the CI scripts and Ubuntu 24.04 build
container, regardless of the runner's host OS.

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
libraries, three test tools, and the sustained-use test executable. Long-duration
stress testing runs separately from routine CI. TSAN with instrumented Qt is a separate testing
procedure documented in [TESTING.md](TESTING.md).

Debug and ASAN/UBSAN builds remain available through the local pipeline commands
below. Development archives require matching Qt/FFTW/compiler runtimes. For a
portable application, use the Release AppImage or folder archive, which includes
both the GUI and CLI with their dependencies. Release packages are checked in clean,
network-disabled Ubuntu 24.04/26.04 and Fedora 44 containers before upload.
GUI smoke tests also require a usable TLS backend without contacting GitHub.
The update suite uses simulated replies, so routine CI remains offline during
tests. Packages include Qt's TLS backend and OpenSSL runtime libraries; online
checks use the host's CA certificate store.

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
same run. Reports remain available under Actions. Development pushes and stable
tag pushes create GitHub releases after verification. Each release includes
`BUILD-INFO.json` with the full commit, version, channel, build number, UTC build
date, and CI run link. Publication uploads to a draft first, verifies every
expected asset, then publishes. Failed uploads remain drafts; rerunning the
workflow resumes them. Published assets and tags are preserved.

## Development builds and versions

`CMakeLists.txt` is the source of the next release's numeric version. Keep feature
notes under `## Unreleased` in `CHANGELOG.md`. Each successful push to `devel`
publishes a prerelease such as `v0.2.0-dev.184.g233be51`, using the workflow run
number and seven characters of the built commit. The GUI, CLI, exported metadata,
package names, accompanying source archive, and manifests use the same version.
There is no version bump commit for each push.

The website offers the latest stable release and latest complete development build
separately. Development selection uses the build number, so a slower older build
cannot replace a newer successful build. Failed builds leave earlier downloads
available. Prereleases never become GitHub's Latest stable release.

Stable update notifications use GitHub's latest-stable endpoint. A development
version recognizes the final stable version of the same release as an update;
an older stable version is never offered as an upgrade.

## Publish a stable release

Use `devel` for development and `main` for release-ready code.

1. Finalize the numeric version in `CMakeLists.txt`. Rename `Unreleased` to that
   version and release date, for example `## 0.2.0 — 2026-10-09`, with nonempty notes.
2. Merge the release-ready commit into `main`, including the release workflow.
3. Tag that commit and push the tag:

   ```sh
   git switch main
   git pull --ff-only
   git tag -a v0.2.0 -m "CroccoSpectrum 0.2.0"
   git push origin v0.2.0
   ```

4. Wait for **Build, test and package**. CI validates the tag against CMake and
   the changelog, and checks that its commit is part of `main`. It builds version
   `0.2.0`, uploads packages, sources, checksums, and build information to a draft,
   then publishes the stable release. An older rerun cannot replace a newer Latest.
5. On `devel`, increase the numeric version for the next planned release and add
   a fresh `Unreleased` section.

Use the appropriate version instead of the example. Pushing `main`, opening a
pull request, or manually running CI produces artifacts without publishing a
release. Publishing a release through GitHub's UI does not trigger this workflow.
Draft-first publication also works when immutable releases are enabled.

## Runner setup

GitHub uses its `ubuntu-24.04` hosted runner.

The source and packages are copied through the container API. A job container's
workspace therefore does not need to exist at the same path on the Docker host.
The publisher uses the built-in `GITHUB_TOKEN` when workflow definitions match
`main`. Only the upload job receives `contents: write`; the build job keeps
`contents: read`. Use isolated runners for untrusted pull requests, as with other
jobs that compile and execute repository code.

GitHub requires Workflows write permission when creating or updating a release
whose commit changes `.github/workflows/` relative to the default branch. The
built-in token cannot receive that permission. For development builds in that
situation, either merge the workflow definitions into `main` first, or configure
a repository Actions secret named `RELEASE_TOKEN` containing a token with
Contents and Workflows write permissions for this repository. A GitHub App
installation token or fine-grained personal access token can provide those
permissions. CI checks this prerequisite and reports it before publication;
verified build artifacts remain available if the publisher is blocked.

The website change must also reach `main` for the Pages workflow to deploy it.
After that deployment, the site discovers new releases dynamically and needs no
redeployment for development pushes. See
[GitHub release API permissions](https://docs.github.com/en/rest/releases/releases#create-a-release).

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

Local builds default to `0.2.0-dev.local.gCOMMIT`, with `.dirty` for a modified
checkout. A source tree without Git records `gunknown`. CMake writes the identity
to `BUILD-INFO.json` in its build directory. CI passes commit, channel, build
number, and date through the outer runner into the container, which has no `.git`.
To reproduce an archived build's version, pass its recorded values to CMake:

```sh
cmake --preset release -DRF_BUILD_CHANNEL=development -DRF_BUILD_NUMBER=184 \
  -DRF_BUILD_COMMIT=FULL_COMMIT_FROM_BUILD_INFO -DRF_BUILD_DATE=UTC_DATE_FROM_BUILD_INFO
```

For an explicitly requested local stable build, use `-DRF_BUILD_CHANNEL=stable`.
The `release` build configuration controls optimization and portable packaging;
the build channel controls version labeling and publication.

Every dependency source provision is collected before Release upload. Package
names read the generated CMake build identity; dependency pins contain no separate
application version. CI checks both executable versions against that identity.
Actions are pinned to immutable commit IDs. Diagnostics are uploaded after a
failure; binary packages are uploaded only
after that job's complete build/test/verification sequence succeeds.

References: [GitHub artifact action](https://github.com/actions/upload-artifact).
