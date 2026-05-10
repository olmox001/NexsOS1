#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/region.h>
#include <kernel/types.h>

/* Simple Region Implementation (List of Rectangles) */

struct region *region_create(void) {
  struct region *reg = (struct region *)kmalloc(sizeof(struct region));
  if (!reg)
    return NULL;
  reg->count = 0;
  reg->capacity = 8;
  reg->rects = (struct rect *)kmalloc(sizeof(struct rect) * reg->capacity);
  if (!reg->rects) {
    kfree(reg);
    return NULL;
  }
  return reg;
}

void region_destroy(struct region *reg) {
  if (reg) {
    if (reg->rects)
      kfree(reg->rects);
    kfree(reg);
  }
}

void region_add_rect(struct region *reg, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;

  if (reg->count >= reg->capacity) {
    reg->capacity *= 2;
    /* Simple realloc equivalent since we don't have krealloc yet */
    struct rect *new_rects =
        (struct rect *)kmalloc(sizeof(struct rect) * reg->capacity);
    if (new_rects) {
      /* memcpy */
      for (int i = 0; i < reg->count; i++)
        new_rects[i] = reg->rects[i];
      kfree(reg->rects);
      reg->rects = new_rects;
    } else {
      return; /* Out of memory, drop rect */
    }
  }
  reg->rects[reg->count].x = x;
  reg->rects[reg->count].y = y;
  reg->rects[reg->count].w = w;
  reg->rects[reg->count].h = h;
  reg->count++;

  /* TODO: Merge adjacent rectangles to optimize? */
}

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

  struct region *new_reg = region_create();
  if (!new_reg)
    return;

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

  /* Swap rects */
  kfree(reg->rects);
  reg->rects = new_reg->rects;
  reg->count = new_reg->count;
  reg->capacity = new_reg->capacity;
  /* Manually free container of new_reg but not its rects which we took */
  kfree(new_reg);
}

void region_intersect_rect(struct region *reg, int x, int y, int w, int h) {
  struct rect clip = {x, y, w, h};
  struct region *new_reg = region_create();
  if (!new_reg)
    return;

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

  kfree(reg->rects);
  reg->rects = new_reg->rects;
  reg->count = new_reg->count;
  reg->capacity = new_reg->capacity;
  kfree(new_reg);
}

void region_clear(struct region *reg) { reg->count = 0; }
