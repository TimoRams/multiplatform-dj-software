#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <BrockDJ.app> <arm64|x86_64> <output.zip>" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
expected_arch="$2"
output="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
binary="$app/Contents/MacOS/BrockDJ"

[[ -x "$binary" ]] || { echo "macOS app executable is missing: $binary" >&2; exit 1; }
qt_bin="${QT_ROOT_DIR:-}/bin"
macdeployqt="${qt_bin}/macdeployqt"
if [[ ! -x "$macdeployqt" ]]; then
    macdeployqt="$(command -v macdeployqt || true)"
fi
[[ -x "$macdeployqt" ]] || { echo "macdeployqt was not found" >&2; exit 1; }
command -v dylibbundler >/dev/null || { echo "dylibbundler was not found" >&2; exit 1; }

qmake="$(dirname "$macdeployqt")/qmake"
[[ -x "$qmake" ]] || { echo "qmake beside macdeployqt was not found" >&2; exit 1; }
qt_prefix="$("$qmake" -query QT_INSTALL_PREFIX)"
qt_plugins="$("$qmake" -query QT_INSTALL_PLUGINS)"
qt_libs="$("$qmake" -query QT_INSTALL_LIBS)"
[[ -f "$qt_plugins/sqldrivers/libqsqlite.dylib" ]] || {
    echo "Qt SQLite source plugin was not found" >&2
    exit 1
}

# macdeployqt deploys every SQL driver by default, including unrelated vendor
# SDKs. Give only the deployment tool a private Qt plugin view; never alter Qt.
deployment_stage="$(mktemp -d "${RUNNER_TEMP:-/tmp}/brockdj-macos-deploy.XXXXXX")"
trap 'rm -r -- "$deployment_stage"' EXIT
mkdir -p "$deployment_stage/bin" "$deployment_stage/plugins/sqldrivers"
cp "$macdeployqt" "$deployment_stage/bin/macdeployqt"
ln -s "$qt_libs" "$deployment_stage/lib"
for plugin_category in "$qt_plugins"/*; do
    [[ -d "$plugin_category" ]] || continue
    [[ "$(basename "$plugin_category")" == sqldrivers ]] && continue
    ln -s "$plugin_category" "$deployment_stage/plugins/$(basename "$plugin_category")"
done
ln -s "$qt_plugins/sqldrivers/libqsqlite.dylib" \
    "$deployment_stage/plugins/sqldrivers/libqsqlite.dylib"
printf '[Paths]\nPrefix=%s\nPlugins=%s\n' "$qt_prefix" "$deployment_stage/plugins" \
    > "$deployment_stage/bin/qt.conf"

# Bundle native dependencies before Qt rewrites their paths. -of overwrites
# individual libraries, unlike -od which deletes the entire Frameworks tree.
mkdir -p "$app/Contents/Frameworks"
dylibbundler -of -b -x "$binary" \
    -d "$app/Contents/Frameworks" \
    -p '@executable_path/../Frameworks/'
"$deployment_stage/bin/macdeployqt" "$app" -qmldir="$repo_root/src/qml" \
    -always-overwrite -verbose=2

actual_archs="$(lipo -archs "$binary")"
if [[ " $actual_archs " != *" $expected_arch "* ]]; then
    echo "expected $expected_arch app, got: $actual_archs" >&2
    exit 1
fi

[[ -n "$(find "$app/Contents" -type f -iname 'libqsqlite.dylib' -print -quit)" ]] || {
    echo "QSQLITE plugin is missing from the macOS bundle" >&2
    exit 1
}
[[ -n "$(find "$app/Contents" -type f -iname '*sqlcipher*.dylib' -print -quit)" ]] || {
    echo "SQLCipher runtime is missing from the macOS bundle" >&2
    exit 1
}

load_rpaths() {
    otool -arch "$expected_arch" -l "$1" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { rpath = 1; next }
        rpath && $1 == "path" {
            sub(/^[[:space:]]*path /, "")
            sub(/ \(offset [0-9]+\)$/, "")
            print
            rpath = 0
        }'
}

expand_loader_path() {
    local path="$1" loader="$2"
    case "$path" in
        @executable_path*) printf '%s%s\n' "$(dirname "$binary")" "${path#@executable_path}" ;;
        @loader_path*) printf '%s%s\n' "$(dirname "$loader")" "${path#@loader_path}" ;;
        /*) printf '%s\n' "$path" ;;
        *) return 1 ;;
    esac
}

binary_rpaths="$(load_rpaths "$binary")"
app_real="$(cd "$app" && pwd -P)"
check_dependency() {
    local loader="$1" dependency="$2" candidate="" rpath expanded owner rpaths
    case "$dependency" in
        /System/Library/*|/usr/lib/*) return 0 ;; # Includes dyld's shared cache.
        @rpath/*)
            # dyld searches this image's runpaths, then those of its executable.
            for owner in "$loader" "$binary"; do
                if [[ "$owner" == "$binary" ]]; then
                    rpaths="$binary_rpaths"
                else
                    rpaths="$(load_rpaths "$owner")"
                fi
                while IFS= read -r rpath; do
                    [[ -n "$rpath" ]] || continue
                    if expanded="$(expand_loader_path "$rpath" "$owner")"; then
                        if [[ -f "$expanded/${dependency#@rpath/}" ]]; then
                            candidate="$expanded/${dependency#@rpath/}"
                            break
                        fi
                    fi
                done <<< "$rpaths"
                [[ -z "$candidate" ]] || break
            done
            ;;
        *)
            candidate="$(expand_loader_path "$dependency" "$loader")" || {
                echo "unsupported Mach-O dependency: $dependency (required by $loader)" >&2
                return 1
            }
            ;;
    esac
    if [[ ! -f "$candidate" ]]; then
        echo "unresolved Mach-O dependency: $dependency (required by $loader)" >&2
        return 1
    fi
    candidate="$(realpath "$candidate")"
    if [[ "$candidate" != "$app_real/"* ]]; then
        echo "dependency outside macOS bundle: $dependency -> $candidate (required by $loader)" >&2
        return 1
    fi
}

while IFS= read -r mach_o; do
    if ! file "$mach_o" | grep -q 'Mach-O'; then
        continue
    fi

    bundled_archs="$(lipo -archs "$mach_o")"
    if [[ " $bundled_archs " != *" $expected_arch "* ]]; then
        echo "bundled Mach-O file lacks $expected_arch architecture: $mach_o ($bundled_archs)" >&2
        exit 1
    fi

    # otool's first line is the inspected file's path, not a dependency.
    dependencies="$(otool -arch "$expected_arch" -L "$mach_o" \
        | sed '1d; s/^[[:space:]]*//; s/ (compatibility version.*$//')"
    if grep -E '/opt/homebrew|/usr/local/(Cellar|opt)|/Users/runner' <<<"$dependencies"; then
        echo "external Homebrew/runner dependency remains in bundle: $mach_o" >&2
        exit 1
    fi
    while IFS= read -r dependency; do
        check_dependency "$mach_o" "$dependency"
    done <<< "$dependencies"
done < <(find "$app/Contents" -type f)

codesign --force --deep --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

rm -f "$output"
ditto -c -k --sequesterRsrc --keepParent "$app" "$output"
[[ -s "$output" ]] || { echo "macOS ZIP was not created: $output" >&2; exit 1; }

# Verify and launch the app from the exact archive users will download.
verification_dir="${RUNNER_TEMP:-/tmp}/brockdj-macos-package-smoke-${expected_arch}"
rm -rf "$verification_dir"
mkdir -p "$verification_dir"
ditto -x -k "$output" "$verification_dir"
packaged_app="$verification_dir/BrockDJ.app"
packaged_binary="$packaged_app/Contents/MacOS/BrockDJ"
codesign --verify --deep --strict --verbose=2 "$packaged_app"
env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH \
    -u QTDIR -u QT_ROOT_DIR -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH \
    -u QML_IMPORT_PATH -u QML2_IMPORT_PATH QT_QPA_PLATFORM=offscreen \
    BROCKDJ_RHI_BACKEND=auto \
    "$packaged_binary" --ci-smoke-test
