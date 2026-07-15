# Packaging and CI artifacts

The cross-platform workflow separates compilation from deployment. Five build
jobs compile, test and run the build-tree smoke test. Five dependent package
jobs download those products and deploy them on the same OS and architecture.
There is no `continue-on-error`; a failed platform is a failed workflow.

Pull requests run workflow lint plus all five build/test jobs. Packaging runs
only for pushes to `main`, `v*` tags and manual `workflow_dispatch` runs.

## Final artifacts

| Platform | GitHub artifact | File |
| --- | --- | --- |
| Linux x86_64 | `BrockDJ-linux-x86_64-AppImage` | `BrockDJ-Linux-x64.AppImage` |
| Linux ARM64 | `BrockDJ-linux-arm64-AppImage` | `BrockDJ-Linux-ARM64.AppImage` |
| macOS Apple Silicon | `BrockDJ-macos-arm64` | `BrockDJ-macOS-Apple-Silicon.zip` |
| macOS Intel | `BrockDJ-macos-x86_64` | `BrockDJ-macOS-Intel.zip` |
| Windows x64 | `BrockDJ-windows-x86_64` | `BrockDJ-Windows-x64.zip` |

Every upload uses `if-no-files-found: error`, and each packaging script rejects
a missing or empty output before upload.

## Linux AppImage

`scripts/ci/package-appimage.sh` stages a fresh AppDir with `linuxdeploy`, the
Qt plugin and the AppImage output plugin. Those tools use immutable release
tags, and the Type-2 runtime is pinned to release `20251108`; the workflow never
downloads a mutable `continuous` asset while packaging.

Qt deployment scans `src/qml`, so Qt Quick Controls and other imported plugins
are included. A temporary plugin view excludes unrelated system Qt plugins
whose own optional dependencies are unresolved. Cleanup is confined to that
staging area and the AppDir, and removes only unused SQL drivers; QSQLITE
remains. Nothing is deleted from the runner's global Qt installation. Optional
stripping is disabled because linuxdeploy's embedded binutils may predate
modern ELF `DT_RELR` sections. AppStream metadata is checked with the runner's
`appstreamcli`; the older validator embedded in the pinned appimagetool is
disabled. The finished AppImage is executed with
`APPIMAGE_EXTRACT_AND_RUN=1` and `--ci-smoke-test`, avoiding any FUSE
requirement in CI.

## macOS bundles

CMake creates `BrockDJ.app` directly for both architectures. The package job:

1. runs `macdeployqt` with the repository QML source directory;
2. copies and rewrites non-Qt Homebrew dependencies with `dylibbundler`;
3. rejects remaining `/opt/homebrew`, `/usr/local/Cellar`, `/usr/local/opt` or
   runner-user references from Mach-O load commands;
4. verifies the requested architecture on the app and every bundled Mach-O
   file using `lipo`;
5. applies an ad-hoc recursive signature and verifies it with `codesign`;
6. creates the ZIP with `ditto`, extracts that exact ZIP into a clean temporary
   directory, verifies its signature again, and runs its executable smoke test.

Ad-hoc signing makes the bundle structurally verifiable but is not Apple
notarization. A future public release can add Developer ID signing and notary
credentials without changing the build/package separation.

## Windows ZIP

Windows uses MSVC 2022, Qt's `win64_msvc2022_64` package and the pinned vcpkg
manifest. `scripts/ci/package-windows.ps1` runs `windeployqt` with `src/qml`,
then follows `dumpbin /dependents` recursively. It copies only DLLs actually
referenced from the vcpkg runtime directory and fails on an unresolved
non-system dependency. It does not copy the entire vcpkg `bin` directory.

The completed ZIP is extracted into a clean temporary directory and its
`BrockDJ.exe --ci-smoke-test` must pass. This checks the exact uploaded archive,
including Qt platform/QML/SQLite deployment, without requiring an audio device
or touching user data.

## Local package checks

The scripts are CI-oriented and expect native dependencies already installed.
Typical invocations are:

```bash
mkdir -p dist
```

```bash
./scripts/ci/package-appimage.sh build-release/bin/BrockDJ dist/BrockDJ-Linux-x64.AppImage
```

```bash
./scripts/ci/package-macos.sh build-ci-macos-arm64/bin/BrockDJ.app arm64 dist/BrockDJ-macOS-Apple-Silicon.zip
```

```powershell
./scripts/ci/package-windows.ps1 `
  -Executable build-ci-windows-x64/bin/BrockDJ.exe `
  -VcpkgBin build-ci-windows-x64/vcpkg_installed/x64-windows/bin `
  -QmlDir src/qml `
  -Output dist/BrockDJ-Windows-x64.zip
```
