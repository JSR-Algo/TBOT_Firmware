# Server Release Inventory Refresh Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for this bounded release-preparation task.

**Goal:** Bind deterministic verification to the reviewed server drain source, including new tests, without weakening release checks.

**Architecture:** Preserve the existing runtime and Git-bound executor. Expand the approved test list, regenerate exact collected node IDs and resource hashes; do not change validator policy, interpreter pins or dependency inventory.

**Tech Stack:** Python3.14.6, pytest9.1.1, existing JSON resource and text node manifests.

Server root: `/Users/manhhodinh/Documents/TBOT/robot/esp32-server/.worktrees/agent-only-gemini-key/main/tbot-server`.
Reviewed source commit: `1cb1cebf5718c94f00221c8b15a2ce1863881afb`.

- [x] Add a failing test to `tests/test_google_live_deterministic_evidence.py`
  asserting both `tests/test_google_live_conversation_playout.py` and
  `tests/test_lesson_audio_drain_ack.py` belong to APPROVED_TEST_FILES and have
  canonical node entries. Add both new playout resource paths to the existing
  inventory coverage test in `tests/test_google_live_command_runner.py`.
- [x] Run these two tests and retain their expected missing-membership failures.
- [x] Append those two test paths to APPROVED_TEST_FILES. Collect using the exact
  current approved list with `python3 -m pytest --collect-only -qq`; retain only
  NODE_PATTERN full matches, validate with parse_manifest, write the generated
  `tests/fixtures/google_live_deterministic_nodes.txt`. No invented counts/skips.
- [x] Mechanically regenerate existing resource entries from file bytes using
  Git blob SHA1(header+bytes), SHA256 and size, adding only the new playout source
  and test. Preserve all existing entries, dependency/runtime pins and limits;
  recompute canonical resourceInventorySha256. Parse the resulting manifest with
  the existing strict parser. Do not include secrets or the manifest itself.
- [x] Rerun new tests and direct approved suite. Review exact source/list/resource
  deltas independently before root makes a scoped local commit (no push/merge).
- [x] Run the two real Git-bound journeys on that clean committed candidate,
  then full release-tool regression. Any new failure requires diagnosis; never
  fake provenance, edit expected PASS outcomes or substitute a base image digest
  for a candidate. This only qualifies software evidence, not robot acoustics.

Evidence: initial inventory commit2b2b722b after spec/quality PASS, direct845
passed42.67s and deterministic/command runner477passed37.02s. Follow-up runtime
commit04cc36353903447589b0542ac330cd7b596b2bf1 after spec/quality PASS,
3focusedpassed1.07s and464gate/runner tests passed60.29s with only the clean-Git
synthetic journey deferred until commit. Final real journeys are running against
clean04cc3635 with normal executor (no diagnostic tracing). No push/merge/deploy.

Follow-up after real Git-bound execution: aggregation still pinned812 while
collection is845; isolated collection failed importing `mcp` for the newly
included existing lesson-drain suite's actual ConnectionHandler. Extend scope
only to synchronize the strict count to845 and pin the installed mcp1.29.0
distribution (117files816919bytes); preserve all prior runtime/dependency entries
and all rejection policy. Add RED/GREEN canonical-scope and dependency membership
tests, exercise real mcp import in the materialized closure, re-review and commit
before rerunning the real journeys. Do not remove the lesson suite or mock away
its ConnectionHandler import. Diagnostic trace runs are not release evidence.

Next real collection reached missing Jinja2 in PromptManager, then actual isolated
ConnectionHandler regression exposed aiohttp in VoiceprintProvider. Pin existing
Jinja2/MarkupSafe and aiohttp's marker-evaluated mandatory dependency closure
(aiohappyeyeballs,aiosignal,frozenlist,multidict,propcache,yarl), preserving existing
versions/limits/policy. The new test includes only repo source, materialized pinned
dependencies and stdlib, not host site-packages; full Git proof remains separate.
Review this two-file extension before local commit and real executor rerun.

Final software verification at clean bf308b25451c906112d6ffc52e683d6a832f2650:
independent spec/quality PASS, normal actual Git-bound journey1passed80.66s;
full release_gate, command_runner, evidence_runner and deterministic_evidence
suite733passed437.12s (exit0), including both real Git-bound journeys. No tracing,
process overrides or test deselection. Earlier opaque child exit1 remains an
unexplained intermittent failure; this successful run is not a zero-flake claim.
Exact final image packaging, real API/robot acoustic and soak gates remain open.
