# Voice 60m Reliability Implementation Plan

**Goal:** Keep wake-word voice turns from getting stuck in `Listening` and collect enough runtime evidence for a 60-minute soak test.

**Approach:** Preserve the existing half-duplex flow. AutoStop turns return to `Idle`; lesson/manual turns may re-enter `Listening`. Add a bounded listening watchdog that exits stale listening sessions, stops backend listening, disables voice processing, and logs queue/drop counters.

**Validation:**
- Add regression tests in `tests/test_realtime_voice_state.py`.
- Run focused pytest suite.
- Build `build-prod`.
- Flash `/dev/cu.usbmodem101`.
- Monitor serial log; a true 60-minute claim requires a 60-minute soak log with periodic speech.
