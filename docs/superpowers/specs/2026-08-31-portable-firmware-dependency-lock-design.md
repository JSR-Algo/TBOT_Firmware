# Portable Firmware Dependency Lock Design

## Goal

Produce a production candidate descended exactly from `ed76b3a86c89987c72012b8d13329c06bcc3bead` whose ESP-IDF dependency graph can be rebuilt from a clean checkout without an ignored lock copied from another machine.

## Constraints

- Preserve the complete BluFi lifecycle sequence through `ed76b3a` and the internal-RAM improvement in `dccac1b`.
- Preserve all course-mode firmware behavior and assets.
- Do not vendor `managed_components`.
- Do not flash, open serial, or operate the physical robot.
- Keep the source diff minimal.

## Design

The two repository-owned components remain under the ESP-IDF standard `components/` directory. Their `override_path` declarations are removed from `main/idf_component.yml`; clean configuration must prove that ESP-IDF still discovers and builds those local components. The generated `dependencies.lock` is committed and `/dependencies.lock` is removed from `.gitignore`.

The accepted lock must contain exact versions and component hashes for downloadable dependencies, target `esp32s3`, and IDF `5.5.4`. It must not contain absolute paths, user-home paths, worktree paths, or local-source entries. A source guard test enforces these properties and confirms the two repository components and their manifests remain present.

## Verification

1. Run the new guard before implementation and record the expected failure.
2. Generate the lock only through ESP-IDF Component Manager.
3. Run the complete project `tests/` suite after dependency hydration.
4. Create two clean isolated checkouts of the candidate commit, build each with the same ESP-IDF commit and candidate sdkconfig defaults, and verify identical BIN, ELF, partition table, and sdkconfig.
5. Record source SHA, lock SHA, artifact hashes, app size, partition capacity, and free margin.
6. Obtain an independent read-only review before release recommendation.

## Release Rule

The candidate is `GO` only if the lock remains unchanged during clean configuration/build, both isolated builds are byte-identical, all project tests pass except documented topology skips, and review finds no P1/P2 issue in lock portability or preservation of the lifecycle fixes.
