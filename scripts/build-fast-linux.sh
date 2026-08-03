#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"
cpu_count="$(nproc)"
default_jobs="${cpu_count}"
if (( default_jobs > 8 )); then
    default_jobs=8
fi
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-${default_jobs}}"
use_ninja=false

if command -v ninja >/dev/null 2>&1; then
    use_ninja=true
fi

cache_file="${build_dir}/CMakeCache.txt"
need_configure=true

if [[ -f "${cache_file}" ]]; then
    if ${use_ninja}; then
        if grep -q 'CMAKE_GENERATOR:INTERNAL=Ninja' "${cache_file}"; then
            need_configure=false
        else
            rm -rf "${build_dir}"
        fi
    elif grep -q 'CMAKE_GENERATOR:INTERNAL=Unix Makefiles' "${cache_file}"; then
        need_configure=false
    else
        rm -rf "${build_dir}"
    fi
fi

if [[ -f "${cache_file}" ]] && ! grep -q 'BUILD_TESTING:BOOL=OFF' "${cache_file}"; then
    need_configure=true
fi

if ${need_configure}; then
    if ${use_ninja}; then
        cmake --preset linux-dev-fast
    else
        printf 'ninja not found — using Unix Makefiles (install ninja for faster builds, e.g. pacman -S ninja)\n' >&2
        cmake -S "${repo_root}" -B "${build_dir}" \
            -G "Unix Makefiles" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DBUILD_TESTING=OFF \
            -DBROCKDJ_TESTS_ONLY=OFF \
            -DRDBJ_ENABLE_QML_CACHEGEN=OFF
    fi
fi

if ${use_ninja}; then
    cmake --build --preset linux-dev-fast --target BrockDJ --parallel "${jobs}"
else
    cmake --build "${build_dir}" --parallel "${jobs}"
fi

printf '\nBuilt: %s\n' "${build_dir}/bin/BrockDJ"
