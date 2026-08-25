# Source Cleanup Follow-up Plan

The broad source-layout consolidation is complete. The current implemented
structure is documented in `source-structure.md`; completed moves and merges are
recorded in `source-cleanup-report.md`.

Remaining work requires behavior-specific review rather than more mechanical
file movement:

1. Heavy settings/mapping surfaces are now created on demand. Extract the
   remaining shared settings content only after desktop/AIO parity tests cover
   focus, popup ownership, device reconciliation and Apply semantics.
2. Split `Library.qml`, `TopHeader.qml` and `DeckControl.qml` only along proven
   reusable visual boundaries; keep state/Connections in one owner.
3. Continue reducing `DjEngine` public surface only as a separate API refactor.
4. Revisit large internal waveform/MIDI translation units only with compile-time
   or ownership evidence; do not recreate public helper headers.
5. Consider moving root test declarations to a CMake include file as a build-only
   cleanup. Do not merge test executables without measuring isolation and build
   time.

Protected audio owners, joined worker classes and hardware transports are not
cleanup candidates merely because they can be textually merged.
