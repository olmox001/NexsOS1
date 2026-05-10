#ifndef KERNEL_REGION_H
#define KERNEL_REGION_H

struct rect {
  int x, y, w, h;
};

struct region {
  struct rect *rects;
  int count;
  int capacity;
};

/* API */
struct region *region_create(void);
void region_destroy(struct region *reg);
void region_add_rect(struct region *reg, int x, int y, int w, int h);
void region_subtract(struct region *reg, int x, int y, int w, int h);
void region_intersect_rect(struct region *reg, int x, int y, int w, int h);
void region_clear(struct region *reg);

#endif
