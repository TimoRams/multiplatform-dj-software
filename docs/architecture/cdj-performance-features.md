# CDJ-style performance features

Concept-level parity with CDJ-3000 / XDJ-AZ performance workflows.

| Feature | Where | Behaviour |
|---|---|---|
| BEAT LOOP pads | `DjEngine::setBeatLoop`, pad tab 5 | 1/4 … 32 beats. Pressing the active length exits; another length resizes from the loop in-point. Loops clipped at the track end can still be exited by the same pad. |
| SLIP LOOP (roll) pads | `beginSlipLoop` / `endSlipLoop`, pad tab 6 | 1/32 … 4 beats while held. Slip is enabled for the roll; on release playback returns to the slip position and the user's previous slip state is restored. |
| KEY SYNC | `deck/KeySync.h`, `DjEngine::syncKeyTo`, deck `KEY` button | Parses Camelot and musical keys, accounts for the partner's audible pitch (tempo without keylock + Key Shift), matches the relative key and picks the smallest shift (exact or a fifth). Enables keylock. Right-click resets. |
| Instant Doubles | `DjEngine::instantDoubleFrom`, Settings → Instant Doubles | Off by default. Loading a file already on another deck mirrors that deck's playhead, tempo (unless SYNC owns tempo), keylock, Key Shift and play state once the load completes. A newer load cancels a pending double. |

All features respect load lock and are covered by `key_sync` tests, the API
contract test and `--ci-smoke-test`.
