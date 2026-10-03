#!/usr/bin/env bash
set -euo pipefail

readonly SOURCE_COMMIT="b33b5a88e04a5182dd19c38c57762925631118fd"
readonly SOURCE_SHA256="39256d7486ee19fbaad153af99d087ce7c1c94484f197959c3103226d36d1f9c"
readonly SOURCE_URL="https://codeload.github.com/ibsh/libKeyFinder/tar.gz/${SOURCE_COMMIT}"

prefix="${1:-/usr/local}"
temp_parent="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
[[ "$temp_parent" == /* ]] || {
    echo "temporary directory must be an absolute path: $temp_parent" >&2
    exit 2
}
[[ -d "$temp_parent" ]] || {
    echo "temporary directory does not exist: $temp_parent" >&2
    exit 2
}
temp_parent="$(cd -- "$temp_parent" && pwd -P)"
[[ "$temp_parent" != "/" ]] || {
    echo "refusing to create temporary build work directly under /" >&2
    exit 2
}
work_dir="$(mktemp -d -- "$temp_parent/brockdj-libkeyfinder.XXXXXX")"
trap 'rm -rf -- "$work_dir"' EXIT
archive="${work_dir}/libkeyfinder.tar.gz"

curl --fail --location --retry 5 --retry-all-errors \
    --output "$archive" "$SOURCE_URL"
printf '%s  %s\n' "$SOURCE_SHA256" "$archive" | sha256sum --check --status || {
    echo "libkeyfinder source archive checksum mismatch" >&2
    exit 1
}

source_dir="${work_dir}/libKeyFinder-${SOURCE_COMMIT}"
build_dir="${work_dir}/build"
mkdir -p "$source_dir"
tar -xzf "$archive" --strip-components=1 -C "$source_dir"

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel 2
sudo cmake --install "$build_dir"
sudo ldconfig

version="$(
    PKG_CONFIG_PATH="${prefix}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
        pkg-config --modversion libkeyfinder
)"
[[ "$version" == "2.2.8" ]] || {
    echo "Expected libkeyfinder 2.2.8 after installation, found $version" >&2
    exit 1
}
