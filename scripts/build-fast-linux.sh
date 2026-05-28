#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"

cmake --preset linux-dev-fast
cmake --build --preset linux-dev-fast --parallel "${jobs}"

printf '\nBuilt: %s\n' "${repo_root}/build-dev/bin/BrockDJ"
