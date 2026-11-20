# Session 3 — graphics, boot handoff and Power12

Opened 2026-11-19 from umbrella `f8b6a1f` on `SPERIMENTAL-LINUX`, with
NX-CORE `5959b5a`, NX-SDK `7f324d2` and NX-USERKIT `64dcb4e`.
All four repositories already had staged work. The umbrella gitlinks identify
commits and cannot capture the contents of a subrepository index.

## Source review

| Boundary | Production path | Outstanding work and acceptance evidence |
| --- | --- | --- |
| Bootloader to HAL | AMD64 GRUB BIOS and UEFI supply Multiboot2. The module getter in `kernel/HAL/host/arch/amd64/hal.c` walks tags independently of the existing bounded iterator in `HAL/capability/multiboot2_info.c`. AArch64 GRUB chainloads the EFI loader, and `HAL/host/arch/aarch64/kinit/start.S` captures its record. | Consolidate parsing behind the existing HAL contract. Keep core consumers independent of the firmware format. Verify module selection, reservations and both firmware paths before replacing a reader. Preserve the direct-loader diagnostic lane. |
| HAL to kernel | `kernel/main.c` still branches on the boot ABI, validates AArch64 EFI fields and assigns AMD64 boot globals. The AMD64 boot-memory provider currently returns unavailable. | Migrate provider-specific validation together with its failure semantics and tests. Keep fault reporting on the existing diagnostic path. |
| Physical output to scene | `kernel/resources/display.c` accepts a mode generation/index, calls the GPU and notifies surface consumers. `nxcompositor` opens surface events, renders a logical scene and calls `OS1_display_present`. | Test old and new physical modes, stable window contents, resize, focus and theme in the actual framebuffer. Record output dimensions independently of logical workspace bounds. |
| Window compatibility | `kernel/resources/window_compat.c` still calls kernel compositor snapshot/move functions. Kernel graphics and the emergency terminal still have consumers. | Inventory the callers of each function before removal. Move scene policy to USERKIT with tests. Preserve capability checks, lifetime, damage notification and fault-time output. |
| Terminal input | `user/USERKIT/services/nxttyd/src/main.c` takes the spawned child as controller and forwards key/resize events. Project tests include source-string checks. | Exercise two real shells, change focus, type a marker and compare both windows. A source check alone cannot detect two visible echoes. |
| Power12 | The umbrella build runs the full audit in maintenance mode and a separate blocking delta audit. USERKIT runs its own service audit. | Record full and delta results separately. Qualify HAL, kernel and services against production-linked tests and real guests. Check the index of each owner before staging. |

The [legacy boot inventory](../legacy/architecture/BOOT-PROTOCOL-INVENTORY.md)
and [graphics migration decision](../legacy/architecture/GRAPHICS-USERLAND-MIGRATION.md)
describe the intended boundaries. The [Session 2 handoff](../session-2/P2P-TRANSCRIPT.md)
records the owner's observations through message 640.

## Verification

Run from an empty project-local build root, with only the validated GRUB tool
cache reused. Preserve build output, real guest suite totals, interactive
commands and QMP framebuffer captures separately for each architecture.

## Session 3 current evidence and handoff

- [Verified state and repository ownership](STATE.md)
- [QEMU, nxadb, host, contract and Power12 verification](VERIFICATION.md)
- [Open issues and remaining qualification debt](ISSUES.md)
- [Generic surface layout plugins and protocol](SURFACE-LAYOUT-PLUGINS.md)

These notes describe an experimental checkout, not the Session 2 frozen
release record. They do not supersede Session 1 governance. Full-project
Power12 qualification is **not complete**. Commit component repositories
before updating umbrella gitlinks. Do not push or switch branches as
part of the local checkpoint.
