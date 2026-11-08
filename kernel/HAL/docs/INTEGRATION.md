# NX-HAL NXEC integration

HAL/nxec is a temporary reference checkout supplied for migration. Production
NX-HAL does not include it. The complete requested NXEC preboot source set is
integrated into common, capability, tests/unit, docs/nxec-reference and tools.

NXEC contributes the validated description plane: bounded hardware snapshots,
HCD v1/v2, MMU capability normalization, boot-memory normalization, ownership
rules and deny-by-default discovery policy.

Existing NexsOS HAL contributes the proven execution plane: boot assembly,
exception/syscall entry, context switch, page-table manipulation, TLB/cache,
IRQ controllers, timers, UART, SMP and device transport.

They are not competing implementations. Provider code captures raw state and
feeds the description plane; NX-CORE consumes only validated HAL protocol data
and outbound HAL operations. Architecture identity is data inside a validated
descriptor, never a generic-core compile-time branch.

Every change must keep host tests, x86_64/AArch64 freestanding cross-link,
ASan/UBSan, clang-tidy/cppcheck, standalone HAL builds, standalone NX-CORE
builds and nxadb boot/runtime tests on both architectures.
