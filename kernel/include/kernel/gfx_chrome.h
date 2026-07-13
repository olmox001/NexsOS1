/*
 * Kernel-internal window-chrome primitives.
 *
 * Allocation-free geometry and raster helpers for the compositor's window
 * decoration: drop shadows, titlebar tinting, titlebar buttons and borders.
 * Every input is plain data (rects, radii, ARGB colours, style scalars); the
 * style/theme structs stay in the compositor, which maps them to arguments.
 * The raster functions draw on a gfx_surface_t honouring an exclusive clip
 * rect, so they share the surface/clip semantics of the userland SDL/OpenGL
 * presentation model.
 *
 * Locking contract: like gl.c, the painters that touch a shared surface must
 * run with compositor_lock held (compositor_render_internal does).
 */
#ifndef KERNEL_GFX_CHROME_H
#define KERNEL_GFX_CHROME_H

#include <kernel/gfx_surface.h>
#include <stdint.h>

/* Extra footprint a shadow adds around a window frame, per side.  Used by the
 * compositor's damage-skip test so a window whose body is outside the damage
 * box but whose shadow reaches into it is still reprocessed. */
typedef struct gfx_chrome_margins {
  int left;
  int right;
  int top;
  int bottom;
} gfx_chrome_margins_t;

/* Titlebar button geometry: single source of truth shared by the render pass
 * and the click hit-test (they historically mirrored each other by hand). */
typedef struct gfx_button_geometry {
  int size;     /* button square/diameter, px          */
  int top;      /* screen y of the buttons' top edge   */
  int close_cx; /* screen x of the close button centre */
  int bg_cx;    /* screen x of the background button centre */
} gfx_button_geometry_t;

enum gfx_button_hit {
  GFX_BUTTON_NONE = 0,
  GFX_BUTTON_CLOSE = 1,
  GFX_BUTTON_BACKGROUND = 2,
};

/* Rounded-rect membership test: is local pixel (lx,ly) inside a w*h rect with
 * corner radius r?  r<=0 ⇒ always inside (square corners). */
int gfx_rrect_contains(int lx, int ly, int w, int h, int r);

/* Shadow footprint per shadow_type (0 solid, 1 fast, 2 premium); a
 * shadow_size of 0 yields zero margins for every type. */
void gfx_chrome_shadow_margins(int shadow_type, int shadow_size,
                               gfx_chrome_margins_t *out);

/* Type 0: opaque backing rect drawn exactly under the (rounded) frame. */
void gfx_chrome_shadow_solid(gfx_surface_t *surface, const gfx_rect_t *clip,
                             const gfx_rect_t *frame, int radius,
                             uint32_t argb);

/* Type 1: single-pass SDF gradient shadow centred on the frame. */
void gfx_chrome_shadow_fast(gfx_surface_t *surface, const gfx_rect_t *clip,
                            const gfx_rect_t *frame, int radius, int spread);

/* Type 2: contact + diffuse SDF shadows with a top rim-light highlight. */
void gfx_chrome_shadow_premium(gfx_surface_t *surface, const gfx_rect_t *clip,
                               const gfx_rect_t *frame, int radius,
                               int spread);

/* Per-pixel titlebar tint (top highlight / vertical gradient) for the given
 * shadow_type; returns the tinted colour.  y is the row inside the titlebar. */
uint32_t gfx_chrome_titlebar_tint(int shadow_type, int y, int titlebar_h,
                                  uint32_t base);

/* Per-pixel inner shadow / separator under the titlebar for the given
 * shadow_type; y is the row inside the content area. */
uint32_t gfx_chrome_content_separator(int shadow_type, int y, uint32_t base);

/* Compute titlebar button geometry for a frame whose titlebar top-left is
 * (frame_x, titlebar_y) and width frame_w.  shape: 0 circle, 1 square,
 * 2 rounded square.  side: 0 left, 1 right. */
void gfx_chrome_button_geometry(int frame_x, int frame_w, int titlebar_y,
                                int titlebar_h, int shape, int side,
                                gfx_button_geometry_t *out);

/* Rectangular hit-test on the two buttons (matches the click handler). */
int gfx_chrome_button_hit(const gfx_button_geometry_t *geometry, int x, int y);

/* Per-pixel button paint over a titlebar pixel at screen (x,y): returns the
 * button colour, the drop-shadow-accented base (when drop_shadow, used by the
 * premium style), or base unchanged. */
uint32_t gfx_chrome_button_pixel(const gfx_button_geometry_t *geometry,
                                 int shape, int x, int y, uint32_t close_argb,
                                 uint32_t bg_argb, int drop_shadow,
                                 uint32_t base);

/* 1px outline around the (rounded) frame, honouring the clip. */
void gfx_chrome_border(gfx_surface_t *surface, const gfx_rect_t *clip,
                       const gfx_rect_t *frame, int radius, uint32_t argb);

#endif /* KERNEL_GFX_CHROME_H */
