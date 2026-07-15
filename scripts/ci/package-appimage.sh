#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <BrockDJ-binary> <output.AppImage>" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$(realpath "$1")"
output="$(realpath -m "$2")"
runner_temp="${RUNNER_TEMP:-/tmp}"

case "$(uname -m)" in
    x86_64) tool_arch="x86_64" ;;
    aarch64|arm64) tool_arch="aarch64" ;;
    *) echo "unsupported AppImage architecture: $(uname -m)" >&2; exit 1 ;;
esac

[[ -x "$binary" ]] || { echo "BrockDJ binary is missing or not executable: $binary" >&2; exit 1; }
if ldd "$binary" | grep -q 'not found'; then
    ldd "$binary" >&2
    echo "BrockDJ has unresolved shared-library dependencies before packaging" >&2
    exit 1
fi

tools_dir="${runner_temp}/brockdj-appimage-tools-${tool_arch}"
appdir="${runner_temp}/BrockDJ-${tool_arch}.AppDir"
mkdir -p "$tools_dir" "$(dirname "$output")"
rm -rf "$appdir"

linuxdeploy="$tools_dir/linuxdeploy.AppImage"
qt_plugin="$tools_dir/linuxdeploy-plugin-qt.AppImage"
appimage_plugin="$tools_dir/linuxdeploy-plugin-appimage.AppImage"
appimage_runtime="$tools_dir/runtime-${tool_arch}"

download() {
    local url="$1" destination="$2"
    if [[ ! -s "$destination" ]]; then
        curl --fail --location --retry 5 --retry-all-errors --output "$destination" "$url"
    fi
    chmod +x "$destination"
}

# Immutable release tags: do not use the mutable "continuous" downloads.
download "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-${tool_arch}.AppImage" "$linuxdeploy"
download "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-${tool_arch}.AppImage" "$qt_plugin"
download "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-appimage-${tool_arch}.AppImage" "$appimage_plugin"
download "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-${tool_arch}" "$appimage_runtime"

export APPIMAGE_EXTRACT_AND_RUN=1
# linuxdeploy's bundled binutils can be older than the runner's ELF format
# (for example DT_RELR/.relr.dyn). Stripping is optional and must not corrupt or
# reject otherwise valid modern libraries.
export NO_STRIP=1
export LINUXDEPLOY_PLUGIN_QT="$qt_plugin"
export LINUXDEPLOY_PLUGIN_APPIMAGE="$appimage_plugin"
export QML_SOURCES_PATHS="$repo_root/src/qml"
# xcb is deployed by default; offscreen/minimal make the packaged smoke test
# independent of a display server and are also useful for safe diagnostics.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqminimal.so"

# linuxdeploy-plugin-qt otherwise probes distribution alternatives and can
# accidentally select Qt 5 on hosts where BrockDJ was built with Qt 6.
if [[ -n "${QT_ROOT_DIR:-}" && -x "${QT_ROOT_DIR}/bin/qmake" ]]; then
    QMAKE="${QT_ROOT_DIR}/bin/qmake"
elif command -v qmake6 >/dev/null 2>&1; then
    QMAKE="$(command -v qmake6)"
elif command -v qmake-qt6 >/dev/null 2>&1; then
    QMAKE="$(command -v qmake-qt6)"
elif command -v qmake >/dev/null 2>&1 && [[ "$(qmake -query QT_VERSION)" == 6.* ]]; then
    QMAKE="$(command -v qmake)"
else
    echo "Qt 6 qmake is required for AppImage deployment" >&2
    exit 1
fi
export QMAKE

qt_version="$("$QMAKE" -query QT_VERSION)"
[[ "$qt_version" == 6.* ]] || { echo "Qt 6 qmake required, found Qt $qt_version at $QMAKE" >&2; exit 1; }
echo "Using Qt $qt_version from $QMAKE for AppImage deployment"

# Distribution Qt installations may contain unrelated third-party plugins with
# missing optional dependencies. The Qt deployment plugin scans every plugin in
# a relevant category, so expose an isolated, dependency-clean view instead of
# mutating the system Qt tree.
real_qmake="$QMAKE"
real_plugins="$($real_qmake -query QT_INSTALL_PLUGINS)"
plugin_stage="${runner_temp}/brockdj-qt6-plugins-${tool_arch}"
rm -rf "$plugin_stage"
mkdir -p "$plugin_stage"

while IFS= read -r -d '' plugin; do
    if ldd "$plugin" 2>&1 | grep -q 'not found'; then
        echo "Skipping Qt plugin with unresolved optional dependencies: $plugin"
        continue
    fi

    relative_plugin="${plugin#"$real_plugins"/}"
    mkdir -p "$plugin_stage/$(dirname "$relative_plugin")"
    cp -L "$plugin" "$plugin_stage/$relative_plugin"
done < <(find "$real_plugins" -type f -name '*.so' -print0)

[[ -f "$plugin_stage/platforms/libqxcb.so" ]] || { echo "Qt xcb platform plugin is missing or unresolved" >&2; exit 1; }
[[ -f "$plugin_stage/platforms/libqoffscreen.so" ]] || { echo "Qt offscreen platform plugin is missing or unresolved" >&2; exit 1; }
[[ -f "$plugin_stage/sqldrivers/libqsqlite.so" ]] || { echo "Qt SQLite plugin is missing or unresolved" >&2; exit 1; }

export QMAKE_REAL="$real_qmake"
export QT_PLUGIN_STAGE="$plugin_stage"
export QMAKE="$repo_root/scripts/ci/qmake6-appimage-wrapper.sh"

# linuxdeploy derives the installed icon name from the source filename. Keep it
# aligned with Icon=BrockDJ in the desktop entry.
packaging_icon="$tools_dir/BrockDJ.png"
cp "$repo_root/resources/icons/generated/256.png" "$packaging_icon"

"$linuxdeploy" --appdir "$appdir" \
    --executable "$binary" \
    --desktop-file "$repo_root/resources/packaging/net.ramsbrock.BrockDJ.desktop" \
    --icon-file "$packaging_icon" \
    --plugin qt

# Prune only inside AppDir. QSQLITE remains because the application needs it.
find "$appdir" -type f \( \
    -name 'libqsqlmysql.so' -o \
    -name 'libqsqlodbc.so' -o \
    -name 'libqsqlpsql.so' \
\) -delete

mkdir -p "$appdir/usr/share/metainfo"
cp "$repo_root/resources/packaging/net.ramsbrock.BrockDJ.metainfo.xml" \
    "$appdir/usr/share/metainfo/net.ramsbrock.BrockDJ.appdata.xml"

export LDAI_OUTPUT="$output"
export LDAI_RUNTIME_FILE="$appimage_runtime"
# The pinned appimagetool embeds an older appstreamcli that rejects metadata
# accepted by current validators. The XML is validated in workflow-lint and is
# still shipped; skip only the stale embedded validation pass.
export LDAI_NO_APPSTREAM=1
"$linuxdeploy" --appdir "$appdir" --output appimage

[[ -s "$output" ]] || { echo "AppImage was not created: $output" >&2; exit 1; }
file "$output"
APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen BROCKDJ_RHI_BACKEND=auto \
    "$output" --ci-smoke-test
