/* Generated from kernel/contracts/surface-v1.json. Do not edit. */
#ifndef NX_SURFACE_ABI_H
#define NX_SURFACE_ABI_H
#include <stdint.h>
#include <stddef.h>
#include <os1_input.h>
#define OS1_SURFACE_ABI_VERSION 1U
#define OS1_SURFACE_TIMING_COMPLETION 1U
#define OS1_RESOURCE_SURFACE_EVENTS "surface/events"
#define OS1_RESOURCE_DISPLAY "display/0"
#define OS1_RESOURCE_KEYBOARD "input/keyboard"
#define OS1_RESOURCE_POINTER "input/pointer"
#define OS1_RESOURCE_WINDOW_STORAGE "surface/window-v1"
struct os1_surface_state_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t generation;
  uint64_t monotonic_ns;
  uint32_t width;
  uint32_t height;
  uint32_t timing;
  uint32_t reserved;
};
#if defined(__cplusplus)
#define NX_SURFACE_ASSERT static_assert
#else
#define NX_SURFACE_ASSERT _Static_assert
#endif
NX_SURFACE_ASSERT(sizeof(struct os1_surface_state_v1) == 40, "surface ABI size");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, abi_version) == 0, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, struct_size) == 4, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, generation) == 8, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, monotonic_ns) == 16, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, width) == 24, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, height) == 28, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, timing) == 32, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_state_v1, reserved) == 36, "surface ABI offset");
#undef NX_SURFACE_ASSERT
struct os1_input_request_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t mode;
  uint32_t reserved;
  struct os1_raw_input_event_v1 event;
};
#if defined(__cplusplus)
#define NX_SURFACE_ASSERT static_assert
#else
#define NX_SURFACE_ASSERT _Static_assert
#endif
NX_SURFACE_ASSERT(sizeof(struct os1_input_request_v1) == 48, "surface ABI size");
NX_SURFACE_ASSERT(offsetof(struct os1_input_request_v1, abi_version) == 0, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_input_request_v1, struct_size) == 4, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_input_request_v1, mode) == 8, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_input_request_v1, reserved) == 12, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_input_request_v1, event) == 16, "surface ABI offset");
#undef NX_SURFACE_ASSERT
struct os1_surface_move_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  int32_t id;
  int32_t x;
  int32_t y;
};
#if defined(__cplusplus)
#define NX_SURFACE_ASSERT static_assert
#else
#define NX_SURFACE_ASSERT _Static_assert
#endif
NX_SURFACE_ASSERT(sizeof(struct os1_surface_move_v1) == 20, "surface ABI size");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_move_v1, abi_version) == 0, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_move_v1, struct_size) == 4, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_move_v1, id) == 8, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_move_v1, x) == 12, "surface ABI offset");
NX_SURFACE_ASSERT(offsetof(struct os1_surface_move_v1, y) == 16, "surface ABI offset");
#undef NX_SURFACE_ASSERT
#endif
