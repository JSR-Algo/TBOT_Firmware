# TVideo Static Fallback Parity Contract Design

## Purpose

Provide a portable, immutable regression contract that proves the manager's
static TVideo fallback matches firmware: it snaps to `arriveNear`, immediately
shows teaching content, and stays paused on that arrived frame.

## Interface

The contract consists of a JSON fixture and a standalone Node.js checker in the
Goal-1 firmware worktree. The checker accepts `--manager-root` pointing at an
extracted manager commit and `--fixture` pointing at the JSON contract. It never
requires the active Template worktree or a browser.

The fixture names the required manager files and declares the observable cases:

- static fallback maps any requested phase to `arriveNear`;
- teaching content is visible at `arriveNear` only for static fallback;
- ordinary playback reveals content only at `revealTeachingContent`;
- component replay pauses when fallback is active.

## Validation

The checker loads the manager's CommonJS layout module, invokes its exported
behavior helpers, and verifies the component uses those helpers and pauses the
fallback replay. Missing exports, missing files, invalid fixture fields, or any
behavior mismatch return a nonzero exit with a specific assertion.

The regression proof must demonstrate:

1. immutable manager commit `edec89d88e3b81219b5fde40cb7b9d7e1ee13a07`
   fails the contract;
2. corrected clean manager commit
   `e13c7eb96761e71705f72afbb13e8154d180e1e4` passes it.

## Scope And Safety

All files remain in the Goal-1-owned firmware worktree and evidence directory.
Validation uses temporary `git archive` extractions. It does not inspect or
modify the active Template worktree and performs no robot, lock, serial, SD,
device API, flash, or playback operation.
