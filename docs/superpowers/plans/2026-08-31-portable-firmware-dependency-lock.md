# Portable Firmware Dependency Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Commit a portable ESP-IDF dependency lock on top of `ed76b3a` and prove two clean checkouts build identical production firmware.

**Architecture:** Repository-owned components continue to load through ESP-IDF's standard `components/` discovery. Component Manager owns the committed root lock, while a Python source guard rejects machine paths, local-source lock entries, missing hashes, target/IDF drift, or missing local components.

**Tech Stack:** ESP-IDF 5.5.4, IDF Component Manager lock format 2.0.0, CMake, pytest, Git worktrees.

---

### Task 1: Add The Reproducibility Guard

**Files:**
- Create: `tests/test_dependency_lock_reproducibility.py`

- [ ] Write tests that require a tracked `dependencies.lock`, reject absolute/machine/local paths, require target `esp32s3`, require IDF `5.5.4`, require a SHA-256 component hash for every downloadable dependency, and require both repository component manifests.
- [ ] Run `python3 -m pytest -q tests/test_dependency_lock_reproducibility.py` and confirm it fails because the lock is absent/ignored.

### Task 2: Generate The Minimal Portable Lock

**Files:**
- Modify: `.gitignore`
- Modify: `main/idf_component.yml`
- Create: `dependencies.lock` through ESP-IDF Component Manager

- [ ] Remove only `/dependencies.lock` from `.gitignore`.
- [ ] Remove only the `override_path` fields for `78/esp-wifi-connect` and `78/esp-ml307`.
- [ ] Run a clean ESP-IDF reconfigure using the production defaults and exact IDF checkout.
- [ ] Confirm configuration selects `components/esp-wifi-connect` and `components/esp-ml307` while the generated lock contains no local path entries.
- [ ] Run the guard and confirm it passes.
- [ ] Commit the spec, plan, test, manifest, ignore rule, and generated lock.

### Task 3: Source And Regression Gates

**Files:**
- Verify only

- [ ] Run `python3 -m pytest -q tests` after managed components are hydrated.
- [ ] Run targeted BluFi lifecycle, internal-RAM, websocket, claim, and course-mode tests.
- [ ] Run `git diff --check` and confirm source status contains only intentional tracked files.

### Task 4: Two Clean Reproducible Builds

**Files:**
- Create evidence only under `task-artifacts/course-mode-production-readiness/`

- [ ] Create two detached clean verification worktrees from the candidate commit.
- [ ] Build each into a separate external build directory with ESP-IDF commit `8e48797f0c7e5849050e88e42100164f5898f9db`, ccache disabled, and identical production defaults.
- [ ] Confirm neither build changes `dependencies.lock`.
- [ ] Compare BIN, ELF, partition table, and sdkconfig byte-for-byte and record SHA-256 and sizes.
- [ ] Record app partition capacity and free margin.

### Task 5: Review And Candidate Evidence

**Files:**
- Create: external qualification report and checksums

- [ ] Request independent read-only review of lock portability, local component selection, BluFi lifecycle preservation, internal-RAM preservation, and evidence completeness.
- [ ] Address any P1/P2 finding with a new RED/GREEN cycle.
- [ ] Commit final minimal changes and record the new candidate SHA.
- [ ] Remove temporary verification worktrees while preserving evidence.
