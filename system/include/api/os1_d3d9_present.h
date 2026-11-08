/*
 * OS1 D3D9-style presentation chain.
 *
 * The presentation seam for the Direct3D 9 personality: a swapchain with a
 * CPU backbuffer, D3D9 Lock/Unlock/Present/Reset semantics, mapped onto the
 * os1_video_platform portability header.  Device/context dispatch is
 * backend-independent: every entry point forwards through an ops table chosen
 * at creation from the platform's advertised backend, so the software
 * presenter can later be swapped for the ASTRA GPU-context object without
 * touching D3D9-personality callers.
 */
#ifndef OS1_D3D9_PRESENT_H
#define OS1_D3D9_PRESENT_H

#include <stddef.h>
#include <stdint.h>

struct os1_d3d9_present_params {
  int x;
  int y;
  int width;  /* backbuffer width in pixels  */
  int height; /* backbuffer height in pixels */
  const char *title;
};

/* D3DLOCKED_RECT analogue: pitch is in BYTES, pixels are ARGB8888. */
struct os1_d3d9_locked_rect {
  uint32_t *bits;
  size_t pitch;
};

/* Optional Present() dirty rect (D3D9 pSourceRect analogue). */
struct os1_d3d9_dirty_rect {
  int x;
  int y;
  int width;
  int height;
};

struct os1_d3d9_swapchain; /* opaque; owns window + backbuffer */

/* Create a swapchain on the platform's active backend.  Returns 0 and a
 * swapchain, -ENOSYS when only an unsupported backend is advertised, or a
 * negative OS1 error. */
int os1_d3d9_swapchain_create(const struct os1_d3d9_present_params *params,
                              struct os1_d3d9_swapchain **out);
void os1_d3d9_swapchain_destroy(struct os1_d3d9_swapchain *swapchain);

/* Lock grants CPU access to the backbuffer; Present while locked fails with
 * -EINVAL (D3D9 forbids presenting a locked surface). */
int os1_d3d9_swapchain_lock(struct os1_d3d9_swapchain *swapchain,
                            struct os1_d3d9_locked_rect *out);
int os1_d3d9_swapchain_unlock(struct os1_d3d9_swapchain *swapchain);

/* Present the backbuffer (or just `dirty` when non-NULL) and notify the
 * compositor. */
int os1_d3d9_swapchain_present(struct os1_d3d9_swapchain *swapchain,
                               const struct os1_d3d9_dirty_rect *dirty);

/* D3D9 Reset analogue: resize window + backbuffer.  Fails while locked. */
int os1_d3d9_swapchain_reset(struct os1_d3d9_swapchain *swapchain, int width,
                             int height);

/* The enum os1_video_backend this swapchain runs on. */
int os1_d3d9_swapchain_backend(const struct os1_d3d9_swapchain *swapchain);

#endif /* OS1_D3D9_PRESENT_H */
