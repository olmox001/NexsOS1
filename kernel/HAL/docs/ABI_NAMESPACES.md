# ABI and symbol namespace ownership

This file is the naming source of truth for the NX-HAL/NX-CORE migration.
Older documents that used `OS1arch_*` for the HAL seam are superseded by the
versioned `nxhal_*` ABI already implemented in this tree.

| Prefix | Owner | Stability | Examples |
| --- | --- | --- | --- |
| `NXHAL_*` / `nxhal_*` | public architecture-neutral HAL ABI | versioned/stable | `nxhal_time_counter_read`, `nxhal_resource_validate` |
| `nxhal_provider_*` | selected architecture provider -> HAL core contract | private to HAL build | `nxhal_provider_clocksource_read` |
| `nxhal_arch_*` | provider-private architecture helper | private; never visible to NX-CORE | new assembly/C helpers replacing mixed `arch_*` / `arch_impl_*` |
| `OS1libkernel_*` | NX-CORE internal policy/library surface | kernel-internal | `OS1libkernel_time_monotonic_ns` |
| `OS1driver_*` | driver/provider -> kernel service contract | kernel-internal, modular | `OS1driver_input_publish` |
| `OS1low_*` | stable low-level user<->kernel ABI | near-immutable | handles, IPC, VM, driver-service primitives |
| `OS1_*` | versionable native user API | libos1/services | windows, input, registry, process APIs |
| bare POSIX/C names | compatibility personality | standards-defined | `poll`, `read`, `clock_gettime` |

## Rules

1. One implementation per primitive. Compatibility aliases are temporary only
   while callers are migrated and are deleted in the same family pass.
2. NX-CORE never includes provider-private architecture headers.
3. Drivers do not embed user-service policy; they publish typed events/resources
   through `OS1driver_*` contracts.
4. The kernel object/capability layer remains type-generic. Device-class policy
   such as keyboard/mouse routing does not live in `object.c`.
5. Userland service policy lives in supervised services. Kernel entry points
   authenticate/authorize the service and transfer bounded data only.
6. HAL inbound events use versioned `nxhal_*_protocol_vN` contracts only when
   a real Core consumer exists. Dead notification protocols are removed, not
   retained as no-op shims.
7. New and reconstructed assembly exports only the provider-private namespace;
   C-visible offsets/layouts are specified by stable headers and static asserts.
8. Every rename family must pass amd64+aarch64 build, HAL boundary gates and the
   four nxadb runtime lanes before the previous spelling is removed.
