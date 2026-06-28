#!/usr/bin/env bash
# Run after AIO UI changes — automated smoke + manual desktop regression prompts.
# See .cursor/memory/progress.md § Desktop regression checklist.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== BrockDJ desktop regression (post-AIO feature) ==="
echo

echo "[1/2] Build + unit/smoke tests..."
./build-fast
ctest --test-dir build --output-on-failure
echo "      OK"
echo

echo "[2/2] Manual checks (~15 min) — verify in DESK mode unless noted:"
echo
cat <<'EOF'
  Library (side-by-side)
    [ ] Library panel opens; track list scrolls
    [ ] Preview: P toggles; scrub bar stop/scrub works
    [ ] Load track to deck A/B (keyboard or context menu)

  Hotkeys
    [ ] P — library preview toggle
    [ ] Space / deck play (if bound) still works

  AIO → DESK restore
    [ ] Switch AIO → DESK via header
    [ ] Mixer visibility matches pre-AIO intent (_desktopShowMixer)
    [ ] FX bar visibility matches pre-AIO intent (_desktopShowFxBar)

  AIO smoke @ reference sizes (optional but recommended)
    [ ] 1280×800 — Performance / LIB / ⚙ tabs; load track; return to Performance
    [ ] 1024×600 — same flow; no clipped controls

  Core audio (either mode)
    [ ] Play on deck A; SYNC between two decks; master handoff on disable
EOF

echo
echo "Done. Mark checklist in progress.md when complete."
