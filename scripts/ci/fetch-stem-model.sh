#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <test-model-directory>" >&2
    exit 2
fi

model="htdemucs_fp16weights.onnx"
sha="d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a"
destination="$1"
if command -v cygpath >/dev/null 2>&1; then
    destination="$(cygpath -u "$destination")"
fi
mkdir -p "$destination"
temporary="$(mktemp -d "$destination/download.XXXXXX")"
trap 'rm -r -- "$temporary"' EXIT
curl --fail --location --retry 5 --output "$temporary/$model" \
    "https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/main/$model"
if command -v sha256sum >/dev/null 2>&1; then
    echo "$sha  $temporary/$model" | sha256sum -c -
elif command -v shasum >/dev/null 2>&1; then
    echo "$sha  $temporary/$model" | shasum -a 256 -c -
else
    echo "A SHA-256 checker (sha256sum or shasum) is required." >&2
    exit 1
fi
mv "$temporary/$model" "$destination/$model"
