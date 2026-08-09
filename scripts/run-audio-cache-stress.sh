#!/usr/bin/env bash
set -euo pipefail

# Scope-1 reproduction harness.  Defaults: 1-hour source, 60 seconds of rapid
# wide scratch reversals.  Example for a quicker local smoke run:
# BROCKDJ_CACHE_STRESS_SECONDS=10 BROCKDJ_CACHE_STRESS_TRACK_SECONDS=300 \
#   ./scripts/run-audio-cache-stress.sh
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Keep this harness on the same configure/build path as the normal test gate.
# `test-fast` builds the stress executable as part of the test build and checks
# the cache regressions before the long-running performance reproduction.
"${repo_root}/test-fast" -R '^(audio_page_cache|scratch_cache|cached_playback)$'
exec "${repo_root}/build-tests/BrockDJ_audio_cache_stress"
