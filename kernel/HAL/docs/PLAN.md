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
