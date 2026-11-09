# NX-HAL migration plan

1. Prove integrated NXEC-equivalent host/cross/sanitize/static gates.
2. Bind existing provider assembly/C state capture to ISA snapshot/HCD
   production; do not duplicate register access paths.
3. Introduce versioned inbound HAL-to-Core event/service protocols and replace
   direct scheduler/syscall/IRQ/timer/fault/log/memory/device calls.
4. Move provider-owned CPU/topology/device state out of kernel globals or pass it
   through opaque handles/protocol services.
5. Enforce zero unapproved HAL-to-kernel link edges and zero generic ISA includes.
6. Build NX-HAL and NX-CORE independently for amd64/aarch64.
7. Boot both with nxadb, inspect real filesystem/service layout (reg, bin,
   sys/bin) on the first runtime validation, and fix contract mismatches.
8. Only after behavioral/test equivalence, delete temporary HAL/nxec.
9. Keep time as one versioned clocksource/clockevent ABI: remove raw timer reads
   from NX-CORE policy, keep early entropy on its separate HAL entropy ABI, and
   make scheduler/IPC/device deadlines monotonic-nanosecond contracts.
10. Merge NXOLMO hardware/resource semantics into the existing object authority
    model; replace legacy MMIO/IRQ/DMA shortcuts rather than adding parallel
    capability engines.
11. Normalize boot handoff into a common BootInfo. AMD64 without FDT is a valid
    PCI/ACPI path; AArch64 FDT and EFI metadata are validated inputs, not generic
    kernel architecture branches. Add and qualify AMD64 UEFI while retaining
    BIOS and AArch64 UEFI regression lanes.
12. Reconstruct provider assembly module-by-module behind stable C/ABI headers
    (entry, trap, syscall, context, SMP, MMU/TLB), preserving one implementation
    per primitive and enforcing Power-of-Ten/Power12 bounds at every C boundary.
13. Publish complete namespace/ABI ownership maps before renaming legacy APIs:
    NXHAL/NX-CORE/kernel, architecture-provider, OS1 native user API, OS1LOW
    syscall/runtime layer, driver/service API and library/POSIX compatibility.
14. Runtime acceptance is five independent lanes: amd64 GRUB-BIOS image,
    amd64 GRUB-UEFI image, amd64 direct kernel, AArch64 GRUB-UEFI image and
    AArch64 direct kernel. Every lane must reach an interactive NXShell and pass
    timerdiag, short heartbeat, post-heartbeat shell interaction, KTEST and
    zero-fault scans. Evidence from one firmware lane never qualifies another.
15. Keep the object/capability layer device-class neutral. Migrate raw input to
    one modular driver core (`OS1driver_*`) with a dedicated bounded `OS1low_*`
    user ABI; remove duplicate provider queues and dead HAL notification seams.
16. Replace the remaining polling-only USB HID fallback and mouse/shell service
    cadence with real readiness/wait-many semantics over the existing monotonic
    deadline model; never encode device readiness as object-type policy.
17. After each driver/service migration, measure wakeups/context switches and
    repeat all five runtime lanes before claiming a performance improvement.
