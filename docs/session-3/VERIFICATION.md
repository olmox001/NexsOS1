# Session 3 — verification ledger (2026-11-19)

Real QEMU boot logs, native `nxadb` commands, QMP screenshots and host
test output were inspected. Files in `build/` and `.git/` are local
evidence and are not tracked release artifacts. These runs reused the
verified GRUB cache; they are **not** proof of a fully clean build.

| Gate | AMD64 GRUB BIOS | AArch64 GRUB UEFI |
| --- | --- | --- |
| Kernel boot and KTEST | PASS 36/36 | PASS 37/37 |
| Libc guest test | PASS 31/31 | PASS 31/31 |
| Coreutils | FAIL 48/50 | FAIL 48/50 |
| gfxcontroltest | PASS 12/12 | PASS 12/12 |
| Full guest userland suite | 19/20 | 19/20 |
| Host `make -o power12 check` | PASS | PASS |
| Host NXPAX stress | 1M + 2M events PASS | 1M + 2M events PASS |

Guest logs: `build/ui-layout-verified-amd64-uart.log` and
`build/ui-layout-verified-aarch64-uart.log`; related build logs are
`.git/ui-layout-verified-{amd64,aarch64}-run.log`.
Host test logs: `.git/ui-layout-host-{amd64,aarch64}-recheck.log`.
The 12 real graphics tests cover plugin anchors and reservations,
workspace reflow, overlay Z, normal oversize window workarea bounds,
and foreign-PID denial. Both arches passed.

Native `nxadb` on AArch64 verified 12 GPU provider modes, 1280x800
physical/Auto logical at zoom 100%; zoom 300% = logical 426x266;
2048x1152 physical/Auto at zoom 100%; zoom 150% = logical 1365x768.
QMP screenshots show `nxbar` and `nxui` at their edges and no
window collision at these sizes. Restored 1280x800 physical,
logical Auto, zoom 100%. Screenshots:
`build/runs/aarch64/iso-uefi/ui-layout-zoom-300.png`,
`ui-layout-highres-100.png`, `ui-layout-highres-150.png`,
`ui-layout-restored.png`. At zoom 300% terminal text quality
remains poor despite correct surface anchoring.

Generated ABI checks, both contract snapshots and 13 cross-repo wire
pair checks passed. Incremental Power12: 67 scoped files, 0 blocking,
47 report-only findings (`.git/ui-layout-verification-final-p12.log`).
Full-project audit: 3 policy, 386 ID/limit, 5,171 AST findings.
These guests are functional diagnostic runs and are **not** globally
Power12-qualified; `make power12-strict` has not passed.

The single failed boot-suite group on each architecture is
Coreutils: `nice` and `nohup`. Timerprobe passed the final guest runs
but failed intermittently in earlier diagnostic runs.
