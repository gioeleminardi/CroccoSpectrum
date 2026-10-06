# CI builds and downloadable artifacts

GitHub runs `.github/workflows/ci.yml`; Gitea runs `.gitea/workflows/ci.yml`.
Both trigger on pushes, pull requests, and manual dispatch. They use the same
scripts and Ubuntu 24.04 build container, regardless of the runner's host OS.

## Downloads

Open **Actions → a workflow run → Artifacts** on either hosting service.
Successful builds upload these artifacts with a 30-day requested retention:

| Artifact | Contents |
| --- | --- |
| `CroccoSpectrum-linux-amd64-release` | AppImage, portable folder tarball, all Release target outputs, complete application source, dependency source archive, and SHA-256 checksums |
| `CroccoSpectrum-linux-amd64-debug` | All Debug executables/static libraries, build identity, and checksum |
| `CroccoSpectrum-linux-amd64-asan` | All ASAN/UBSAN executables/static libraries, build identity, and checksum |
| `reports-release`, `reports-debug`, `reports-asan` | Build logs, CTest log/JUnit XML, build identity; Release also has dependency manifest and distribution smoke logs |

Release packaging waits for Debug and ASAN/UBSAN checks to pass. Each build
compiles the CMake ALL target: GUI, CLI, both static libraries, both test tools,
and the sustained-use test executable. Long-duration stress testing runs
separately from routine CI. TSAN with instrumented Qt is a separate testing
procedure documented in [TESTING.md](TESTING.md).

Development archives require matching Qt/FFTW/compiler runtimes. For a portable
application, use the Release AppImage or folder archive, which includes both the
GUI and CLI with their dependencies. Release packages are checked in clean,
network-disabled Ubuntu 24.04/26.04 and Fedora 44 containers before upload.

Artifacts are ZIP containers around the listed files. Extract that ZIP, then
extract a tarball to preserve executable permissions; for a standalone AppImage,
run `chmod +x` before launching. Check a downloaded file with
`sha256sum --check FILE.sha256`. Keep application and dependency sources with
redistributed binaries; see [THIRD_PARTY.md](THIRD_PARTY.md).

Downloads use the hosting service's normal sign-in/access policy. Server retention
limits can override the requested 30 days. These are workflow artifacts; the
pipeline does not create a tagged release or publish to a package registry.

## Runner setup

GitHub uses its `ubuntu-24.04` hosted runner. Gitea needs an **amd64 runner** with
the `ubuntu-latest` label, Bash, Git, tar, Node 20 or newer for the pinned actions,
and a Docker CLI connected to a usable daemon. Enable Actions in the repository.
Use Gitea 1.22 or later for the compatible v4 artifact action and register the
runner against the instance's public URL. If the job itself runs in a container,
make its Docker daemon reachable through the runner's normal configuration.

The source and packages are copied through the container API. A job container's
workspace therefore does not need to exist at the same path on the Docker host.
No publishing token or release secret is required. Use isolated runners for
untrusted pull requests, as with other jobs that compile and execute repository
code.

Builds fetch the pinned Qt SDK and checksum-verified packaging tools. The runtime
asset has an upstream continuous URL; if it changes, CI fails the pinned hash
check. Review and update the runtime binary hash, commit, source pins, and tool
compatibility together rather than bypassing verification.

## Run the pipeline locally

```sh
bash ci/build.sh debug
bash ci/build.sh asan
bash ci/build.sh release
```

Outputs appear under `dist/ci/CONFIGURATION/packages` and `reports`. A fresh
checkout is recommended. `RF_CI_JOBS` controls compile parallelism (default 2).
For local checks on a Podman workstation, `RF_CI_ENGINE=podman` selects that CLI;
the Gitea runner instructions above require its supported Docker setup.

Every dependency source provision is collected before Release upload. Package
names read the application version from `packaging/dependencies.json`; CI checks
that it matches the CMake project version. Actions are pinned to immutable commit
IDs. Diagnostics are uploaded after a failure; binary packages are uploaded only
after that job's complete build/test/verification sequence succeeds.

References: [GitHub artifact action](https://github.com/actions/upload-artifact),
[Gitea artifacts](https://docs.gitea.com/usage/actions/artifacts/),
[Gitea-compatible artifact action](https://github.com/ChristopherHX/gitea-upload-artifact),
[Gitea runner setup](https://docs.gitea.com/runner/installation/docker/).
