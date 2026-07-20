# TVideo Static Fallback Parity Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a portable fixture and checker that fail on the old manager static fallback and pass on the corrected immutable manager commit.

**Architecture:** Store expected fallback behavior in JSON and implement a dependency-free Node.js checker that loads manager layout helpers and inspects the preview component's integration markers. Use temporary immutable Git archives for red/green commit validation and record exact hashes in Goal-1 evidence.

**Tech Stack:** Node.js, JSON, Python pytest contract tests, Git object archives.

---

### Task 1: Lock The Portable Contract

**Files:**
- Create: `tests/fixtures/tvideo-static-fallback-immediate-reveal.json`
- Create: `tests/test_tvideo_static_fallback_parity_checker.py`

- [ ] **Step 1: Write a fixture contract test before the fixture exists**

Add a pytest that requires schema version `1`, the two manager-relative paths,
the `arriveNear` fallback phase, the `revealTeachingContent` reveal phase, and
the three behavior cases described in the design.

- [ ] **Step 2: Run the focused pytest and require the expected missing-fixture failure**

Run: `python3 -m pytest tests/test_tvideo_static_fallback_parity_checker.py -q`

Expected: nonzero with `FileNotFoundError` for the fixture.

- [ ] **Step 3: Add the minimal JSON fixture**

The fixture must contain only stable observable behavior and manager-relative
paths; it must not contain commit-specific paths or source-code snippets.

- [ ] **Step 4: Run the focused pytest and require the fixture schema test to pass**

Run: `python3 -m pytest tests/test_tvideo_static_fallback_parity_checker.py -q`

Expected: exit `0`.

### Task 2: Implement The Standalone Checker Test-First

**Files:**
- Create: `scripts/check_tvideo_static_fallback_parity.mjs`
- Modify: `tests/test_tvideo_static_fallback_parity_checker.py`

- [ ] **Step 1: Add tests for missing arguments, old-commit failure, and corrected-commit success**

The test extracts each manager commit with `git archive`, invokes the checker,
and asserts the old commit is nonzero with a missing behavioral export while
the corrected commit is exit `0` with a JSON `PASS` result.

- [ ] **Step 2: Run the tests and require failure because the checker is absent**

Run: `python3 -m pytest tests/test_tvideo_static_fallback_parity_checker.py -q`

Expected: nonzero because `scripts/check_tvideo_static_fallback_parity.mjs`
does not exist.

- [ ] **Step 3: Implement the minimal checker**

Parse `--manager-root` and `--fixture`; validate the fixture; load the manager
layout module with `createRequire`; execute every declared behavior case; read
the preview component; require helper integration and fallback pause; print one
JSON result; return nonzero on any mismatch.

- [ ] **Step 4: Run the focused tests and require red/green proof**

Run: `python3 -m pytest tests/test_tvideo_static_fallback_parity_checker.py -q`

Expected: exit `0`, with the test proving `edec89d...` fails and `e13c7eb...`
passes.

- [ ] **Step 5: Commit the fixture, checker, tests, design, and plan**

```bash
git add docs/superpowers/specs/2026-07-18-tvideo-static-fallback-parity-contract-design.md \
  docs/superpowers/plans/2026-07-18-tvideo-static-fallback-parity-contract.md \
  tests/fixtures/tvideo-static-fallback-immediate-reveal.json \
  tests/test_tvideo_static_fallback_parity_checker.py \
  scripts/check_tvideo_static_fallback_parity.mjs
git commit -m "test(firmware): add tvideo fallback parity contract"
```

### Task 3: Record The Corrected Immutable Contract

**Files:**
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/docs/evidence/artifacts/lesson-preview-parity/20260718T041659Z/software/tvideo-handoff-checklist.json`
- Modify: `/Users/manhhodinh/Documents/TBOT/robot/docs/evidence/lesson-preview-parity.md`

- [ ] **Step 1: Run the checker directly against fresh immutable archives**

Require a failing JSON result for `edec89d...` and a passing JSON result for
`e13c7eb...`; record commit and checker/fixture SHA-256 values.

- [ ] **Step 2: Update the checklist**

Change `staticFallbackImmediateRevealParity` from `GAP` to `PASS`, set the
manager commit to the corrected full hash, retain `NOT_READY_FOR_HARDWARE`, and
leave Goal 2's SD blocker and explicit PASS handoff requirement unchanged.

- [ ] **Step 3: Append a concise evidence continuation**

Record the immutable red/green proof and state that no hardware action occurred.

- [ ] **Step 4: Validate all evidence**

Run `python3 -m json.tool` on the checklist, run a consistency assertion over
the corrected commit and PASS status, run `git diff --check` on touched files,
and confirm the Goal-1 firmware worktree is clean after commit.
