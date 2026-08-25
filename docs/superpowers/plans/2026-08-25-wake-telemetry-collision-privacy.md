# Wake Telemetry Collision and Privacy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish a wake interval on the first non-busy snapshot after an SPSC collision and enforce direct raw-sample dataflow privacy without formatting-sensitive source gates.

**Architecture:** Keep the two independent two-slot SPSC channels and producer-active flags. Add consumer-owned collision state so a retry drains the slot the producer actually completed, while preserving a different older pending slot for a later exactly-once drain. Replace exact source ordering and call-name allowlists with unordered structural schemas plus a token-normalized `ObserveFeedChunk` dataflow contract.

**Tech Stack:** C++17 atomics and native host tests, Python 3/pytest source-contract tests, Bash host runner, ESP-IDF production build.

---

### Task 1: Deterministic Collision Regression

**Files:**
- Modify: `tests/native/wake_word_telemetry_test.cc`

- [ ] **Step 1: Expose telemetry internals only to the native test**

Include the standard headers used by the telemetry header, temporarily redefine `private` as `public`, include `wake_word_telemetry.h`, then restore `private`. This permits deterministic protocol staging without adding a production callback or test hook.

- [ ] **Step 2: Add the exact failing collision sequence**

Stage `producer_active = 1`, call `TakeSnapshot()` so the consumer requests the other slot and defers, then let the simulated producer read `requested_slot`, set `producer_slot`, populate that interval, and clear `producer_active`. Assert the collided snapshot is empty and the next snapshot reports the populated interval.

- [ ] **Step 3: Add repeated-drain accounting**

Prepopulate the original slot before the collision, then drain until empty and assert the old and collided observations appear exactly once in total. Check chunk counts, above-floor counts, and persistent totals.

- [ ] **Step 4: Run the native test and record RED**

Run:

```bash
scripts/run_host_native_wake_word_lifecycle_test.sh
```

Expected: FAIL at the new assertion that the first post-collision snapshot contains the producer interval; the old implementation returns it one snapshot later.

### Task 2: Collision-Aware SPSC Drain

**Files:**
- Modify: `main/audio/wake_words/wake_word_telemetry.h`
- Test: `tests/native/wake_word_telemetry_test.cc`

- [ ] **Step 1: Add consumer-owned collision state**

Add one boolean collision flag for each channel and pass it by reference to `DrainOne`. The flags are consumer-only and never touched by producers.

- [ ] **Step 2: Drain the producer-completed slot after collision**

When the prior call collided, first return if the producer remains active. Otherwise read the synchronized `producer_slot`, request its opposite, recheck `producer_active`, drain and clear that completed slot, and clear the collision flag. Clear `pending_slot` only if it names the drained slot; otherwise redirect production away from the retained pending slot before returning.

- [ ] **Step 3: Mark collisions without waiting**

In the normal path, keep the requested-slot flip and pending slot. If `producer_active` is nonzero, set the collision flag and return immediately. Do not add loops, sleeps, mutexes, `atomic_flag`, or `test_and_set`.

- [ ] **Step 4: Run the native suite and record GREEN**

Run:

```bash
scripts/run_host_native_wake_word_lifecycle_test.sh
```

Expected: all four native tests print `OK`, including the deterministic collision and existing concurrent exactly-once checks.

- [ ] **Step 5: Run ThreadSanitizer**

Compile and run `tests/native/wake_word_telemetry_test.cc` with `-std=c++17 -pthread -fsanitize=thread -fno-omit-frame-pointer -I main`. Expected: exit 0 with no ThreadSanitizer report.

### Task 3: Direct Raw-Sample Dataflow Contract

**Files:**
- Modify: `tests/test_realtime_voice_state.py`

- [ ] **Step 1: Add failing mutation and formatting-acceptance tests**

Add a mutation that inserts a second `samples[0]` read into an assignment that packs the raw value into the upper bits of `feed_channel_.producer_slot`; require a `raw sample dataflow changed` assertion. Add mutations that wrap the `ObserveFeedChunk` signature and add whitespace around both `static_cast` expressions; these must remain accepted.

- [ ] **Step 2: Run focused pytest and record RED**

Run the new mutation test directly. Expected: FAIL because the current structural checker does not inspect raw-sample expression flow.

- [ ] **Step 3: Add normalized function/token helpers**

Locate a function body by identifier instead of an exact signature string. Tokenize identifiers, numbers, operators, and punctuation after comment/literal sanitization so line wrapping and cast spacing normalize to the same representation.

- [ ] **Step 4: Enforce canonical sample flow**

Require exactly one statement containing `samples [ ... ]`, with tokens equal to `const int32_t sample = samples [ i ] ;`. Require every later `sample` token to occur only in the canonical `magnitude` declaration using the two `static_cast<uint32_t>` conversions. Any other borrowed/raw sample statement raises `raw sample dataflow changed`.

- [ ] **Step 5: Replace brittle structural gates**

Compare allowed includes, top-level declarations, nested types, public methods, and aggregate member schemas without declaration-order sensitivity. Remove exact source-line static function lists, full call-name allowlists, and atomic load/store order assertions. Retain bans on extra global/static storage, arbitrary nested types or members, raw arrays/pointers, owning containers, aliases, callbacks, allocation, logging, file I/O, and transport.

- [ ] **Step 6: Run focused pytest and record GREEN**

Run:

```bash
python3 -m pytest tests/test_realtime_voice_state.py -q
```

Expected: all focused tests pass, including the raw upper-bit mutation and harmless formatting acceptance.

### Task 4: Final Verification and Commit

**Files:**
- Verify: `main/audio/wake_words/wake_word_telemetry.h`
- Verify: `tests/native/wake_word_telemetry_test.cc`
- Verify: `tests/test_realtime_voice_state.py`
- Verify unchanged: `main/application.cc`

- [ ] **Step 1: Run all focused verification**

Run the focused pytest file, native lifecycle runner, standalone telemetry TSAN build/run, `git diff --check`, and `git status --short`.

- [ ] **Step 2: Compile production code**

Run the existing ESP-IDF production build in `build-production-offline-wake` after sourcing the configured ESP-IDF environment. If the full environment is unavailable, compile a standalone translation unit including the production header and report the full-build limitation explicitly.

- [ ] **Step 3: Confirm runtime log and scope**

Verify `main/application.cc` has no diff and inspect the final diff to confirm changes are limited to the telemetry helper, native regression, privacy tests, and approved documentation.

- [ ] **Step 4: Commit implementation**

```bash
git add main/audio/wake_words/wake_word_telemetry.h \
  tests/native/wake_word_telemetry_test.cc \
  tests/test_realtime_voice_state.py
git commit -m "fix: publish collided wake intervals promptly"
```

- [ ] **Step 5: Repeat post-commit verification**

Repeat the focused pytest, native suite, TSAN run, production compile/build, `git diff --check`, `git status --short`, `git rev-parse HEAD`, and `git show --stat --oneline --summary HEAD` before reporting completion.
