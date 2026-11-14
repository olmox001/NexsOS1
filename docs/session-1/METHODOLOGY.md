# Session 1 mandatory development methodology

This document operationalizes the owner directive in `/README-LLM.md`. It is
mandatory for this session and future sessions unless the owner explicitly
changes it.

## 1. Bounded work unit

Every implementation task is a small bounded block. Before editing:

1. state the single objective;
2. list the files and ownership layers expected to be involved;
3. read the relevant current files in full, not only diffs or grep matches;
4. read the relevant active and legacy design documents in full;
5. inspect callers, tests, build integration and similar implementations;
6. catalogue missing behavior and unrelated defects discovered during study.

No implementation starts from an assumption about a file, subsystem or prior
agent's intent.

## 2. No parallel replacement architecture

Reuse or extend the canonical existing mechanism. A temporary compatibility
bridge is allowed only when the migration explicitly requires it and the bridge
has a documented removal gate. Do not create a second registry, scene table,
device catalogue, buffer owner, capability model, timer model or execution
policy beside the canonical one.

When moving an implementation, move or reconstruct its tests around the new
ownership boundary. A stale test that exercises the old layer is not evidence
that the migrated implementation works.

## 3. Verification before staging

The minimum sequence is:

1. focused host/static tests;
2. contract/snapshot gates;
3. Power12 changed-scope gate for kernel/HAL code;
4. clean integration build when the task crosses repository boundaries;
5. graphical boot on every affected architecture;
6. nxadb runtime verification;
7. at least two meaningful nxadb commands per architecture when runtime is
   affected;
8. inspect build and runtime logs, including warnings and fallbacks;
9. update `STATE.md`, `ISSUES.md` and `VERIFICATION.md`;
10. stage only the exact verified files.

A passing compile is not enough.

## 4. Tests are incomplete until they prove the observed behavior

When an error is reported or observed but the existing test suite does not see
it, the suite is incomplete. Before claiming the defect fixed, add or extend a
test that fails for the observed bad behavior and passes for the corrected
behavior at the real implementation boundary.

Synthetic tests may validate algorithms, bounds and state machines, but they do
not replace system tests for interactions that depend on boot, device delivery,
focus, graphics, scheduling, DMA, capability routing or service lifecycle.

For graphical/input work, the acceptance path is the graphical system. UART is
debug evidence only. A test that proves only UART shell behavior does not prove
NexsOS1 graphical behavior.

## 5. Architecture matrix

Runtime-affecting kernel/HAL/SDK/USERKIT changes are not complete until the
affected architecture matrix passes. The default target is both AArch64 and
AMD64. A platform-specific change still requires proving that the other
architecture did not regress unless the owner explicitly narrows scope.

Clean means the build outputs are removed first with the project-supported clean
target. The build log must be retained or referenced from the verification
record.

## 6. Safety and recovery

Before invasive work:

- verified work is staged;
- the staged diff/hash is frozen in the Session 1 recovery area;
- unverified work is archived before destructive cleanup;
- branch changes are forbidden without owner authorization.

Never delete an implementation merely because its ownership is wrong. First
identify useful behavior, callers, protocols, tests and the correct destination.

## 7. Power12

Power12 is mandatory for kernel/HAL code. Do not weaken the checker, suppress a
rule, falsify a test or use empty scaffolding to obtain a pass. A scoped changed
gate proves only the scoped block. Full-project qualification is claimed only
when the full strict gate passes.

Every error or warning discovered by Power12 is catalogued even when outside the
current task. Fixing broad historical debt is performed in bounded batches with
real regression tests.

## 8. Documentation is part of the task

A task is not complete until its objective, design evidence, changed files,
tests, runtime result, remaining limitations and newly discovered defects are
recorded. Documentation is updated during the block, not reconstructed later.
