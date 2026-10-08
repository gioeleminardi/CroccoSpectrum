# Third-party dependencies

Application: GPL-3.0-or-later (`LICENSE`).

- Qt 6.11.2 Core/Gui/Widgets and needed platform/image modules: use applicable
  LGPL/GPL options and retain module/third-party notices. Shared libraries are
  bundled and replaceable; the application source/build instructions are provided.
- FFTW 3.3.10: GPL-2.0-or-later (compatible with the application GPLv3 choice).
- ICU 73.2 from the Qt SDK: Unicode/ICU license.
- DejaVu Sans: its font notices accompany bundled fonts.
- AppImage type-2 runtime: pinned commit and binary checksum in the manifest.
  The accompanying source includes its FUSE patches/build scripts; libfuse
  3.15.0 and squashfuse 0.5.2 sources are included alongside permissive runtime
  dependency sources/notices.
- Other ELF dependencies: the generated `bundle-manifest.json` records copied
  libraries/hashes and Ubuntu package versions; notices are in `share/licenses`.

No Python runtime, aqtinstaller, compiler, CMake, or container engine is required
to run the packaged application. Those are build/test tools.

Sources for redistributable libraries and the complete application/build inputs
must accompany a distributed release through the applicable source provisions.
Do not treat a list of license names or copying notices alone as a substitute
for the required source. The generated `dist/` artifacts include:

- `croccospectrum-0.1.4-source.tar.gz`: complete application, tests, docs and build scripts.
- `dependency-sources.tar.gz`: pinned Qt/ICU source, exact Ubuntu source packages
  including their patches/.dsc descriptors, and AppImage runtime sources.
- `dependency-sources/sources.json`: versions, upstream URLs and SHA-256 hashes.
- `share/licenses` inside each binary package: Ubuntu copyright files and
  license/notice files collected from accompanying upstream archives.

Keep these source provisions with a redistributed release. The folder's shared
libraries remain replaceable. Qt's SDK is an official prebuilt Linux gcc_64 SDK;
the application and Ubuntu libraries target the Ubuntu 24.04 ABI. A reproducible
build procedure is provided; bit-for-bit identical binary archives are not claimed.

Upstream references: [Qt licensing](https://doc.qt.io/qt-6/licensing.html),
[Qt source archives](https://download.qt.io/archive/qt/6.11/6.11.2/submodules/),
[FFTW source](https://www.fftw.org/download.html),
[ICU sources](https://github.com/unicode-org/icu/releases/tag/release-73-2).
