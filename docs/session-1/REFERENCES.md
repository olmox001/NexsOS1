# Session 1 reference policy

References are study inputs, not build dependencies and not authority over the
NexsOS1 owner directive.

## Approved reference families

### Linux

Use for driver model, VFS, DMA/IRQ/device lifecycle, VirtIO, input/evdev,
graphics/DRM concepts, scheduling and practical hardware coverage. Compare
interfaces and lifecycle invariants before adopting structure.

### seL4

Use for capability semantics, authority transfer, isolation, revocation,
verified-kernel design discipline and minimal trusted-computing-base concepts.
Do not copy seL4 IPC or object layout blindly into the existing NexsOS object
model.

### BSD

Use BSD kernels for networking, VFS/device code, portability and security
design comparison. The exact BSD repository/revision must be selected and
documented before adding an optional submodule; no specific BSD variant is
silently assumed by this document.

### NXOLMO

Project-owned reference/source candidate for registry/VFS, ext4, 9P, checked
algorithms and related contracts already identified by NexsOS1 architecture
documents. Reuse is component-by-component with license, ownership, Power12 and
behavioral verification.

### Existing project references

Existing pinned or local project references such as base-nexs, external
libraries and historical NexsOS source are evidence for algorithms and behavior
only after their current relevance is checked.

## Optional submodule policy

Linux, seL4 and the selected BSD reference are to live under `references/` as
optional Git submodules. They are not initialized by the normal setup script and
must never become required for build, test or release.

Before adding each reference:

1. choose the exact upstream repository and immutable revision;
2. record license and intended study scope;
3. prove normal setup/build works with the reference absent;
4. add an explicit opt-in fetch/setup command;
5. record the submodule in the Session 1 verification log.

## Use rule

When design uncertainty exists, use this order:

1. current NexsOS1 source and tests;
2. current active Session 1 documentation;
3. relevant legacy NexsOS1 design/history;
4. project-owned references such as NXOLMO/base-nexs;
5. Linux/seL4/BSD source;
6. current external research when the above are insufficient.

A reference may suggest a design; only NexsOS1 tests and acceptance gates prove
the implementation.
