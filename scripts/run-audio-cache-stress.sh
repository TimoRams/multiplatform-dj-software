#!/usr/bin/env bash
set -euo pipefail

# Scope-1 reproduction harness.  Defaults: 1-hour source, 60 seconds of rapid
# wide scratch reversals.  Example for a quicker local smoke run:
# BROCKDJ_CACHE_STRESS_SECONDS=10 BROCKDJ_CACHE_STRESS_TRACK_SECONDS=300 \
#   ./scripts/run-audio-cache-stress.sh
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

build_dir="${repo_root}/build-tests"
cmake -S "${repo_root}" -B "${build_dir}" -DBUILD_TESTING=ON -DBROCKDJ_TESTS_ONLY=ON
cmake --build "${build_dir}" --parallel
# Check the cache regressions before the long-running performance reproduction.
ctest --test-dir "${build_dir}" -R '^(audio_page_cache|scratch_cache|cached_playback)$' --output-on-failure
exec "${build_dir}/BrockDJ_audio_cache_stress"
