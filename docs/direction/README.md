# NEXS — Architecture Direction (captured notes)

These documents capture the architectural direction for NEXS as a set of focused,
actionable notes — one theme per file, each mappable to a tracked issue. They are
the durable record of the design review held alongside the TIMER-UAF-01 fault fix
(see `docs/report/TIMER-UAF-01-CERTIFIED-FIX.md`).

Guiding thesis (from the review): **NEXS gets stronger the less it tries to look
like UNIX.** Not "become more POSIX" but the opposite — a system centred on the
*compositor* and the *application/window* as the primary object, where
*everything is a service and everything communicates by events*. POSIX/libc is a
compatibility layer built ON TOP of the proprietary OS1 base API, never the core.

| Doc | Theme | Tracking issue |
|-----|-------|----------------|
| [DIR-01](DIR-01-naming-and-objects.md) | OS1_/POSIX naming split + object handles | #137 |
| [DIR-02](DIR-02-compositor-decoupling.md) | Compositor ↔ process/scheduler decoupling; compositor in the HAL | #83 / #69 / #67 |
| [DIR-03](DIR-03-unified-events.md) | One `event_wait()` model (key/mouse/IPC/timer/window/process) | #138 |
| [DIR-04](DIR-04-capabilities-and-services.md) | Capabilities not privileges; service-family syscalls; no `fork()` | #79 / #95 / #120 |
| [DIR-05](DIR-05-fault-recovery-and-debugger.md) | C-line backtrace, kernel recovery mode, on-screen panic | #139 |
| [DIR-06](DIR-06-hal-conformance.md) | ALL kernel code through the HAL; kill arch divergence (HAL-ARCH-01) | #140 |

The fault that triggered this review is tracked + resolved in #136
(`docs/report/TIMER-UAF-01-CERTIFIED-FIX.md`).

## Sequencing

1. **Now / done:** TIMER-UAF-01 fault fix; notification popup fix; pilot rename
   `sleep → OS1_sleep` (DIR-01 pilot).
2. **Next task (explicit):** complete the OS1_ rename across the base API (DIR-01).
3. **Then:** DIR-06 HAL conformance audit (the arch-divergence class that produced
   TIMER-UAF-01's two different symptoms), then DIR-02 compositor decoupling.
4. DIR-03/04/05 are larger and staged after the ABI naming and HAL conformance
   settle, so they are expressed once against a stable surface.

These are direction, not committed scope; each becomes a real issue when picked up.
