#!/usr/bin/env bash
# Downloads the official ONNX Runtime CPU SDK into libs/onnxruntime so the
# optional STEMS feature can be built. Without it BrockDJ builds without stems.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/libs/onnxruntime"

if [[ -e "$DEST" || -L "$DEST" ]]; then
    echo "Refusing to overwrite the existing ONNX Runtime SDK at $DEST." >&2
    echo "Move it elsewhere first if you want to install a different version." >&2
    exit 1
fi

VERSION="1.31.1"
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)  ASSET="onnxruntime-linux-x64-$VERSION.tgz"
                   SHA="c849e36a1ef3e22d4ff217b9d1ce4a3cf5b9467b975610913ae959ad5b16bdc9" ;;
    Linux-aarch64|Linux-arm64)
                   ASSET="onnxruntime-linux-aarch64-$VERSION.tgz"
                   SHA="5317505e2da99be65f8e2775ba3c2afd6d2961fd78aba1bd8f56159f48820c78" ;;
    Darwin-arm64)  ASSET="onnxruntime-osx-arm64-$VERSION.tgz"
                   SHA="2e1344a1926ee50eb25b38153cb6992c3567515aef0e677a1a520a1798896d6f" ;;
    Darwin-x86_64) VERSION="1.20.1"
                   ASSET="onnxruntime-osx-x86_64-$VERSION.tgz"
                   SHA="0f73006813af2a1a5d1723ed7dfb694fc629d15037124081bb61b7bf7d99fc78" ;;
    *) echo "Unsupported platform $(uname -s)-$(uname -m); install onnxruntime-devel from your package manager." >&2
       exit 1 ;;
esac

TMP="$(mktemp -d "${TMPDIR:-/tmp}/brockdj-onnxruntime.XXXXXX")"
trap 'rm -r -- "$TMP"' EXIT
curl -fL --retry 3 -o "$TMP/$ASSET" \
    "https://github.com/microsoft/onnxruntime/releases/download/v$VERSION/$ASSET"
if command -v sha256sum >/dev/null 2>&1; then
    echo "$SHA  $TMP/$ASSET" | sha256sum -c -
elif command -v shasum >/dev/null 2>&1; then
    echo "$SHA  $TMP/$ASSET" | shasum -a 256 -c -
else
    echo "A SHA-256 checker (sha256sum or shasum) is required." >&2
    exit 1
fi
tar -xzf "$TMP/$ASSET" -C "$TMP"
mv "$TMP/${ASSET%.tgz}" "$DEST"
echo "ONNX Runtime $VERSION installed to $DEST — re-run CMake to enable STEMS."
