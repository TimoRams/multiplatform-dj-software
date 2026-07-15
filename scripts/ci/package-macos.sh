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

"$macdeployqt" "$app" -qmldir="$repo_root/src/qml" -always-overwrite -verbose=2
mkdir -p "$app/Contents/Frameworks"
dylibbundler -od -b -x "$binary" \
    -d "$app/Contents/Frameworks" \
    -p '@executable_path/../Frameworks/'

actual_archs="$(lipo -archs "$binary")"
if [[ " $actual_archs " != *" $expected_arch "* ]]; then
    echo "expected $expected_arch app, got: $actual_archs" >&2
    exit 1
fi

while IFS= read -r mach_o; do
    if ! file "$mach_o" | grep -q 'Mach-O'; then
        continue
    fi

    bundled_archs="$(lipo -archs "$mach_o")"
    if [[ " $bundled_archs " != *" $expected_arch "* ]]; then
        echo "bundled Mach-O file lacks $expected_arch architecture: $mach_o ($bundled_archs)" >&2
        exit 1
    fi

    dependencies="$(otool -L "$mach_o")"
    if grep -E '/opt/homebrew|/usr/local/(Cellar|opt)|/Users/runner' <<<"$dependencies"; then
        echo "external Homebrew/runner dependency remains in bundle: $mach_o" >&2
        exit 1
    fi
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
QT_QPA_PLATFORM=offscreen BROCKDJ_RHI_BACKEND=auto \
    "$packaged_binary" --ci-smoke-test
