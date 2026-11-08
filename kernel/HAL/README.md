# NX-HAL

HAL/ owns every architecture implementation used by NX-CORE. The kernel is
being migrated to consume only architecture-neutral HAL contracts; architecture
names, trap layouts, ISA instructions, page-table encodings, boot handoffs and
platform discovery belong here.

Current verified migration boundary:

- HAL/arch/{amd64,aarch64} owns the former kernel/arch trees.
- Architecture-only UART/IRQ/timer providers live beside their providers.
- sources.mk is the single HAL source manifest.
- make -C HAL ARCH=amd64|aarch64 builds HAL/build/<arch>/libnxhal.a.
- The kernel build consumes the same HAL source set through one archive rather
  than duplicating the source manifest.

This is the first mechanical boundary, not the final semantic boundary. The
legacy providers still import kernel facilities. ARCHITECTURE.md records those
reverse dependencies and the protocol conversion required to reach the hard
rule: HAL never includes or calls kernel internals, and kernel source never
selects or names an architecture.
