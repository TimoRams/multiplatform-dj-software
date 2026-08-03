#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
