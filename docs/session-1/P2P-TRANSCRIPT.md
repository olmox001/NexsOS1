# Session 1 P2P Operational Transcript

This document is the English operational transcript for Session 1. It preserves
message-number ranges, decisions, verification evidence, and owner directives in
English. The exact raw message bodies remain authoritative in
`.git/p2p-dc/history.jsonl`.

Session 1 boundary: P2P message **#77**. Messages after #77 belong to Session 2.

## #4–#14 — P2P recovery and interactive-channel repair

- P2P protocol v2 was brought online with persistent per-session history,
  sequence/ack tracking, reconnect replay, and an interactive line editor.
- The owner confirmed the channel was live and reported that sent messages were
  not immediately visible and that the input box broke on wrap or incoming
  messages.
- The P2P client was corrected so sent messages are echoed immediately and the
  input line redraws cleanly without wrapping corruption.
- P2P tests reached 7/7 PASS.
- Manual QEMU input was identified as contamination of an early nxadb shell
  interaction result; subsequent runtime gates were therefore required to run
  without human input unless explicitly coordinated.

## #15–#23 — TTY rendering, counter output, and test coverage

- The owner reported counter/debug output overlapping the graphical shell and
  duplicated graphical characters while UART output remained correct.
- The agent confirmed that the duplication was a graphical terminal/rendering
  problem, not duplicated UART input.
- The owner requested the complete userland test suite, direct execution of
  `sys/bin` and `bin` programs, registry inspection, and an audit of
  printf/header portability.
- The automatic boot test suite and debug visibility were reviewed as part of
  the acceptance criteria.
- The owner established that verified work must be staged only after complete
  checks.

## #24–#37 — Worktree reconciliation and duplicate-echo fix

- Kernel/system/USERKIT staged and unstaged state was explicitly inventoried
  before further changes.
- The counter stdout-routing issue was separated from the glyph duplication
  issue.
- Runtime evidence showed UART advancing while the shell framebuffer remained
  unchanged, proving that self-rendered process output no longer polluted the
  controlling TTY.
- The duplicate graphical characters were traced to `nxttyd` being started
  without `--shell`: both `nxttyd` local echo and NXShell line editing were
  echoing the same input.
- Main and demo manifests were corrected to start `nxttyd --shell <shell>`.
- The owner reiterated dual-architecture acceptance, boot-time userland tests,
  and restoration of the timing probe.

## #38–#48 — Checkpoint discipline and next hardening phase

- The owner requested that kernel/HAL compiler warnings become blocking in the
  next phase, while Power12 strict promotion remained deferred.
- AArch64 clean runtime passed the shell/history/timer/counter gate with
  zero kernel-test failures.
- The userland test runner gained the timing probe; Coreutils remained at its
  known 42/50 baseline and was explicitly treated as pre-existing debt rather
  than hidden.
- The staging rule was formalized: work remains unstaged during a phase; when
  the entire phase is verified, the complete phase is staged together as a
  buildable, committable checkpoint.

## #49–#66 — Execution model, TTY hosting, and job control

- The owner clarified the required execution semantics:
  - one common execution model for shell, launcher, and file manager;
  - Unix-style job control for CLI-to-CLI execution;
  - TTY/CLI hosting is a transport layer, not a second execution model;
  - GUI applications launched from a shell inherit the parent TTY for debug;
  - GUI applications launched outside a TTY retain a headless debug TTY;
  - CLI programs launched outside a TTY open a visible terminal and wait for
    Enter before closing.
- A previously mentioned fixed capability number was explicitly identified as
  a typo and was not introduced.
- The owner required the launcher and dock to use the same icon-classification
  model.
- A clean AArch64 gate passed before the model was frozen.

## #67–#77 — Session 1 freeze and Session 2 handoff

- Counter logging was reduced to a more useful low-frequency heartbeat.
- A coordinated checkpoint was staged across kernel, system, USERKIT, and the
  umbrella repository.
- The next phase was defined as compile-time warning hardening for kernel/HAL;
  runtime behavior must never panic or block merely because a compiler-warning
  policy is enabled.
- The owner required dual-architecture runtime qualification before staging the
  warning-hardening phase and explicitly offered manual visual testing when
  UART or screenshots were insufficient.
- The owner declared the next work to be Session 2, required the Session 1
  transcript to be preserved, and requested periodic review of legacy
  documentation.

## Session 1 outcome

Session 1 established the operating workflow used by later work:

1. recover from P2P/Desktop Commander/Git before editing after disconnects;
2. keep active work unstaged;
3. qualify both architectures and real runtime behavior;
4. stage an entire coherent phase only when it is commit-ready;
5. use the owner as an interactive visual/runtime tester when necessary;
6. keep execution, TTY hosting, job control, and GUI debug semantics distinct
   but coordinated.
