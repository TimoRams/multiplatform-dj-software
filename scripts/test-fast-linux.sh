#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

required_submodule_files=(
    "libs/JUCE/CMakeLists.txt"
    "libs/link/AbletonLinkConfig.cmake"
    "libs/signalsmith-dsp/CMakeLists.txt"
    "libs/signalsmith-linear/CMakeLists.txt"
    "libs/signalsmith-stretch/CMakeLists.txt"
)
for required_file in "${required_submodule_files[@]}"; do
    if [[ ! -f "${repo_root}/${required_file}" ]]; then
        printf 'Required Git submodules are not initialized.\n\nRun:\ngit submodule update --init --recursive\n' >&2
        exit 1
    fi
done
if git -C "${repo_root}" submodule status --recursive | grep -q '^-'; then
    printf 'Required Git submodules are not initialized.\n\nRun:\ngit submodule update --init --recursive\n' >&2
    exit 1
fi

build_dir="${repo_root}/build-tests"
cpu_count="$(nproc)"
default_jobs="${cpu_count}"
if (( default_jobs > 8 )); then
    default_jobs=8
fi
build_jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-${default_jobs}}"
test_jobs="${CTEST_PARALLEL_LEVEL:-${build_jobs}}"

cache_file="${build_dir}/CMakeCache.txt"
need_configure=false
if [[ ! -f "${cache_file}" ]] \
    || ! grep -q 'CMAKE_GENERATOR:INTERNAL=Ninja' "${cache_file}" \
    || ! grep -q 'BUILD_TESTING:BOOL=ON' "${cache_file}" \
    || ! grep -q 'BROCKDJ_TESTS_ONLY:BOOL=ON' "${cache_file}"; then
    need_configure=true
fi

if ${need_configure}; then
    cmake --preset linux-tests-fast
fi

cmake --build --preset linux-tests-fast --parallel "${build_jobs}"
ctest --preset linux-tests-fast --parallel "${test_jobs}" "$@"
