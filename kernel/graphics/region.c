/*
 * kernel/graphics/region.c
 * Region Manager — List-of-Rectangles
 *
 * Role:
 *   Implements a dynamic list of non-overlapping (but not guaranteed disjoint
 *   after region_add_rect absorption) axis-aligned rectangles.  Used by
 *   compositor.c for occlusion culling (the "occluded" and per-window
 *   "visible" regions computed in compositor_render_internal).
 *
 * API:
 *   region_create / region_destroy  — allocate / free the list.
 *   region_add_rect                 — add a rect, absorbing any rects it fully
 *                                     covers and skipping if fully covered.
 *   region_subtract                 — boolean difference: removes a rect's
 *                                     area from all existing rects, splitting
 *                                     each intersecting rect into up to 4
 *                                     non-overlapping pieces.
 *   region_intersect_rect           — clip the region to a bounding rect.
 *   region_clear                    — reset count to 0 (keeps allocation).
 *
 * Invariants:
 *   - region_destroy(NULL) is a safe no-op.
 *   - region_add_rect rejects zero/negative-dimension rects at entry.
 *   - The rectangle list grows up to MAX_RECTS_PER_REGION (256); additions
 *     beyond that limit are silently dropped.
 *   - region_subtract and region_intersect_rect build a new_reg, then steal
 *     its rects array and free the container shell (not the array).
 *   - region_clear sets count=0 but does NOT null-check reg; caller must not
 *     pass NULL (unlike region_destroy which guards on NULL).
 *
 * Locking: none — all functions are called under compositor_lock in practice.
 *
 * Known issues:
 *   None tracked in docs/review/analysis/06-graphics.md.
 */
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/region.h>
#include <kernel/types.h>

/* Simple Region Implementation (List of Rectangles) */

/*
 * region_create - allocate a new empty region.
 *
 * Allocates a struct region via kcalloc (zeroed) and an initial rect array of
 * 8 slots via kmalloc.  Returns NULL if either allocation fails; in that case
 * the partially-allocated struct is freed before returning.
 *
 * Locking: inherits kmalloc_lock via kmalloc/kcalloc; safe to call with
 *          compositor_lock held.
 * Returns: pointer to new region, or NULL on allocation failure.
 */
struct region *region_create(void) {
  struct region *reg = (struct region *)kcalloc(1, sizeof(struct region));
  if (!reg)
    return NULL;
  reg->rects = (struct rect *)kmalloc(sizeof(struct rect) * 8);
  if (!reg->rects) {
    kfree(reg);
    return NULL;
  }
  reg->count = 0;
  reg->capacity = 8;
  return reg;
}

/*
 * region_destroy - free a region and its rect array.
 *
 * NULL-safe: silently returns if reg is NULL.  Frees reg->rects first (if
 * non-NULL) then frees reg itself.  Callers that have stolen reg->rects
 * (as region_subtract and region_intersect_rect do) must null the pointer
 * before calling region_destroy to avoid a double-free.
 *
 * Locking: none required beyond what kmalloc_lock provides internally.
 */
void region_destroy(struct region *reg) {
  if (reg) {
    if (reg->rects)
      kfree(reg->rects);
    kfree(reg);
  }
}

/* MAX_RECTS_PER_REGION: hard cap on rect count per region.  Additions that
 * would push count past this limit are silently dropped.  Chosen to limit
 * worst-case memory and O(n) scan cost per compositor frame. */
#define MAX_RECTS_PER_REGION 256

/* GFX-PERF-01 (#131 / GFX-COMP-07): region_subtract / region_intersect_rect used
 * to region_create() a scratch region on EVERY call — i.e. a kmalloc/kfree pair
 * per occluder per window per frame, all from the timer-IRQ compositor tick.
 * Instead they now build into a single static scratch (fixed array, never
 * reallocated — region_add_rect's MAX cap prevents growth) and copy the result
 * back, so the steady state does no per-frame allocation.  Safe because both run
 * serially under compositor_lock. */
static struct rect g_scratch_rects[MAX_RECTS_PER_REGION];
static struct region g_scratch = {.rects = g_scratch_rects,
                                  .count = 0,
                                  .capacity = MAX_RECTS_PER_REGION};

static struct region *region_scratch_reset(void) {
  g_scratch.count = 0;
  return &g_scratch;
}

/* Copy src's rects into dst, growing dst->rects once if needed (rare; reaches a
 * steady capacity after warmup, then never allocates). */
static void region_copy_into(struct region *dst, const struct region *src) {
  if (dst->capacity < src->count) {
    int new_cap = src->count;
    struct rect *nr = (struct rect *)kmalloc(sizeof(struct rect) * new_cap);
    if (!nr)
      return; /* keep dst unchanged on OOM */
    if (dst->rects)
      kfree(dst->rects);
    dst->rects = nr;
    dst->capacity = new_cap;
  }
  for (int i = 0; i < src->count; i++)
    dst->rects[i] = src->rects[i];
  dst->count = src->count;
}

/*
 * region_add_rect - add an axis-aligned rectangle to the region.
 *
 * Params: reg — target region (must not be NULL); x, y — top-left corner;
 *         w, h — dimensions (must be > 0 or the call is ignored).
 *
 * Algorithm:
 *   1. Early exit for zero/negative dimensions.
 *   2. Containment check: if any existing rect fully covers the new rect,
 *      skip (the new area is already represented).
 *   3. Absorption pass: remove any existing rects that are fully covered by
 *      the new rect (compact the list in-place with a write pointer j).
 *   4. Capacity guard: drop the addition if count >= MAX_RECTS_PER_REGION.
 *   5. Grow the rects array by doubling capacity (up to MAX_RECTS_PER_REGION)
 *      if needed; failure to realloc silently drops the addition.
 *   6. Append the new rect.
 *
 * The region is NOT guaranteed to be disjoint after absorption — two rects
 * that partially overlap are both retained; subtraction is not performed here.
 *
 * Locking: caller must hold compositor_lock if used from compositor context.
 */
void region_add_rect(struct region *reg, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;

  int x2 = x + w, y2 = y + h;

  /* Skip if already fully covered by an existing rect */
  for (int i = 0; i < reg->count; i++) {
    struct rect *r = &reg->rects[i];
    if (r->x <= x && r->y <= y && r->x + r->w >= x2 && r->y + r->h >= y2)
      return;
  }

  /* Absorb any existing rects fully covered by the new rect */
  int j = 0;
  for (int i = 0; i < reg->count; i++) {
    struct rect *r = &reg->rects[i];
    if (!(x <= r->x && y <= r->y && x2 >= r->x + r->w && y2 >= r->y + r->h))
      reg->rects[j++] = reg->rects[i];
  }
  reg->count = j;

  if (reg->count >= MAX_RECTS_PER_REGION)
    return;

  if (reg->count >= reg->capacity) {
    int new_cap = reg->capacity * 2;
    if (new_cap > MAX_RECTS_PER_REGION)
      new_cap = MAX_RECTS_PER_REGION;

    struct rect *new_rects =
        (struct rect *)kmalloc(sizeof(struct rect) * new_cap);
    if (new_rects) {
      for (int i = 0; i < reg->count; i++)
        new_rects[i] = reg->rects[i];
      kfree(reg->rects);
      reg->rects = new_rects;
      reg->capacity = new_cap;
    } else {
      return;
    }
  }
  reg->rects[reg->count].x = x;
  reg->rects[reg->count].y = y;
  reg->rects[reg->count].w = w;
  reg->rects[reg->count].h = h;
  reg->count++;
}

/*
 * region_subtract - remove a rectangle's area from every rect in the region.
 *
 * Params: reg — region to modify; x, y, w, h — rectangle to subtract (no-op
 *         if w <= 0 or h <= 0).
 *
 * Algorithm (naive rebuild):
 *   Allocates a new_reg scratch region.  For each existing rect r:
 *     - Compute intersection of r and sub (ix, iy, iw, ih).
 *     - If no intersection: add r verbatim to new_reg.
 *     - If intersection: split r into up to 4 axis-aligned pieces (top,
 *       bottom, left, right of the intersection) and add each to new_reg.
 *   Then steal new_reg->rects into reg (updating count/capacity) and free
 *   the new_reg container shell (NOT its rects array which is now owned by
 *   reg).
 *
 * The "split into 4" decomposition is the standard rectangle boolean-
 * difference: top strip (r.y .. iy), bottom strip (iy+ih .. r.y+r.h), left
 * strip (r.x .. ix, height=ih), right strip (ix+iw .. r.x+r.w, height=ih).
 * Strips with zero height or width are skipped by region_add_rect's guard.
 *
 * Locking: caller must hold compositor_lock if used from compositor context.
 * NOTE: O(n) per call; called in an inner loop → O(n²) per render frame.
 */
/* Subtract rect (sub) from region (reg) */
void region_subtract(struct region *reg, int x, int y, int w, int h) {
  /* Boolean difference: For each rect in reg, subtract (x,y,w,h) */
  /* This can split 1 rect into up to 4 rects */
  /* Since we modify the list in place, we need a temp list or careful iteration
   */
  /* Naive approach: Create new list */

  struct rect sub = {x, y, w, h};
  if (w <= 0 || h <= 0)
    return;

  struct region *new_reg = region_scratch_reset();

  for (int i = 0; i < reg->count; i++) {
    struct rect r = reg->rects[i];

    /* Check intersection */
    int ix = (r.x > sub.x) ? r.x : sub.x;
    int iy = (r.y > sub.y) ? r.y : sub.y;
    int iw = ((r.x + r.w) < (sub.x + sub.w)) ? (r.x + r.w) : (sub.x + sub.w);
    int ih = ((r.y + r.h) < (sub.y + sub.h)) ? (r.y + r.h) : (sub.y + sub.h);
    iw -= ix;
    ih -= iy;

    if (iw <= 0 || ih <= 0) {
      /* No intersection, keep original */
      region_add_rect(new_reg, r.x, r.y, r.w, r.h);
      continue;
    }

    /*
     * Split r into up to 4 pieces:
     * Top, Bottom, Left, Right relative to intersection
     */

    /* Top */
    if (r.y < iy) {
      region_add_rect(new_reg, r.x, r.y, r.w, iy - r.y);
    }
    /* Bottom */
    if (r.y + r.h > iy + ih) {
      region_add_rect(new_reg, r.x, iy + ih, r.w, (r.y + r.h) - (iy + ih));
    }
    /* Left */
    if (r.x < ix) {
      region_add_rect(new_reg, r.x, iy, ix - r.x, ih);
    }
    /* Right */
    if (r.x + r.w > ix + iw) {
      region_add_rect(new_reg, ix + iw, iy, (r.x + r.w) - (ix + iw), ih);
    }
  }

  /* Copy the scratch result back into reg (no per-call allocation). */
  region_copy_into(reg, new_reg);
}

/*
 * region_intersect_rect - clip the region to a bounding rectangle.
 *
 * Params: reg — region to clip in-place; x, y, w, h — clip rectangle.
 *
 * For each rect in reg, computes its intersection with the clip rect and
 * retains only the overlapping portion.  Rects with no intersection are
 * discarded.  Like region_subtract, builds a scratch new_reg and steals its
 * rects array into reg.
 *
 * Used in compositor_render_internal to clip each window's visible region to
 * screen bounds after occlusion subtraction.
 *
 * Locking: caller must hold compositor_lock if used from compositor context.
 */
void region_intersect_rect(struct region *reg, int x, int y, int w, int h) {
  struct rect clip = {x, y, w, h};
  struct region *new_reg = region_scratch_reset();

  for (int i = 0; i < reg->count; i++) {
    struct rect r = reg->rects[i];
    int ix = (r.x > clip.x) ? r.x : clip.x;
    int iy = (r.y > clip.y) ? r.y : clip.y;
    int iw =
        ((r.x + r.w) < (clip.x + clip.w)) ? (r.x + r.w) : (clip.x + clip.w);
    int ih =
        ((r.y + r.h) < (clip.y + clip.h)) ? (r.y + r.h) : (clip.y + clip.h);
    iw -= ix;
    ih -= iy;

    if (iw > 0 && ih > 0) {
      region_add_rect(new_reg, ix, iy, iw, ih);
    }
  }

  region_copy_into(reg, new_reg);
}

/*
 * region_clear - reset the region's rect count to zero without freeing memory.
 *
 * Param: reg — must NOT be NULL (no NULL guard here, unlike region_destroy).
 * Keeps the allocated rects array for reuse; does not zero the array contents.
 * The next region_add_rect call will overwrite slots from index 0.
 *
 * Locking: caller must hold compositor_lock if used from compositor context.
 */
void region_clear(struct region *reg) { reg->count = 0; }
