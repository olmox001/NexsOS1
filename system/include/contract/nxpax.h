#ifndef OS1_NXPAX_ABI_H
#define OS1_NXPAX_ABI_H

#include <stdint.h>
#include <stddef.h>

/* Stable object-resource ABI, unrelated to kernel object implementation. */
#define OS1_NXPAX_ABI_VERSION UINT32_C(1)
#define OS1_NXPAX_RESOURCE "diagnostics/nxpax"
#define OS1_NXPAX_OP_READ_RECORD UINT32_C(0x00220001)

/* Bounded retention: two ring slots per supported CPU, shared by fault,
 * Core disposition, and diagnostic severity records; not lossless. */
#define OS1_NXPAX_CAPTURE_CPU_LIMIT UINT32_C(64)
#define OS1_NXPAX_MAX_WINDOW_RECORDS (2U * OS1_NXPAX_CAPTURE_CPU_LIMIT)
#define OS1_NXPAX_STATUS_EMPTY UINT32_C(0)
#define OS1_NXPAX_STATUS_EVENT UINT32_C(1)
#define OS1_NXPAX_STATUS_OVERRUN UINT32_C(2)
#define OS1_NXPAX_KIND_HARDWARE UINT32_C(1)
#define OS1_NXPAX_KIND_USER_TERMINATED UINT32_C(2)
#define OS1_NXPAX_KIND_TERMINATION_FAILED UINT32_C(3)
#define OS1_NXPAX_KIND_KERNEL_FATAL UINT32_C(4)
#define OS1_NXPAX_KIND_DIAGNOSTIC_WARNING UINT32_C(5)
#define OS1_NXPAX_KIND_DIAGNOSTIC_ERROR UINT32_C(6)

/* Cursor = last consumed sequence. work_units counts attempted ring-slot
 * reads (0 or 1), not cycles or elapsed execution time. Addresses are only
 * returned by the privileged resource, never by the public NXPAX broker. */
struct os1_nxpax_query {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t cursor;
  uint64_t newest;
  uint64_t dropped;
  uint64_t fault_pc;
  uint64_t fault_address;
  uint64_t syndrome;
  int32_t pid;
  int32_t cpu_id;
  uint32_t kind;
  uint32_t flags;
  uint32_t status;
  uint32_t operation_id;
  uint64_t work_units;
};

_Static_assert(sizeof(struct os1_nxpax_query) == 88U,
               "NXPAX object resource ABI size");

#endif
