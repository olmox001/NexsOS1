/*
 * user/demo3d.c
 * High-Performance 3D Rotating Icosahedron Demo
 * Uses Software Rasterization and Batch Blitting
 */
#include <os1.h>

/* Fixed-point 16.16 */
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)

/* Use long long for 64-bit integers to avoid stdint.h dependency issues */

#define WIN_W 300
#define WIN_H 250
#define BUFFER_SIZE (WIN_W * WIN_H)

/* Framebuffer */
static unsigned int framebuffer[BUFFER_SIZE];

/* 3D Point */
typedef struct {
  int x, y, z; /* Fixed-point 16.16 */
} vec3_t;

/* Icosahedron: 12 vertices */
#define NUM_VERTS 12
static vec3_t verts[NUM_VERTS];

/* Vertices initialization (Golden Ratio based) */
static void init_shape(int size) {
  /* Golden Ratio phi = 1.61803... */
  /* FP_ONE is 65536. phi_fp = 106039 */
  int phi = 106039;
  int s = size;
  int phis = fixmul(size, phi);

  /* (0, ±1, ±phi) */
  verts[0] = (vec3_t){0, -s, -phis};
  verts[1] = (vec3_t){0, -s, phis};
  verts[2] = (vec3_t){0, s, -phis};
  verts[3] = (vec3_t){0, s, phis};

  /* (±1, ±phi, 0) */
  verts[4] = (vec3_t){-s, -phis, 0};
  verts[5] = (vec3_t){-s, phis, 0};
  verts[6] = (vec3_t){s, -phis, 0};
  verts[7] = (vec3_t){s, phis, 0};

  /* (±phi, 0, ±1) */
  verts[8] = (vec3_t){-phis, 0, -s};
  verts[9] = (vec3_t){-phis, 0, s};
  verts[10] = (vec3_t){phis, 0, -s};
  verts[11] = (vec3_t){phis, 0, s};
}

/* Rotate point around Y axis */
static vec3_t rotate_y(vec3_t p, int angle) {
  int rad = DEG_TO_FP_RAD(angle);
  int c = cos_fp(rad);
  int s = sin_fp(rad);
  vec3_t r;
  r.x = fixmul(p.x, c) - fixmul(p.z, s);
  r.y = p.y;
  r.z = fixmul(p.x, s) + fixmul(p.z, c);
  return r;
}

/* Rotate point around X axis */
static vec3_t rotate_x(vec3_t p, int angle) {
  int rad = DEG_TO_FP_RAD(angle);
  int c = cos_fp(rad);
  int s = sin_fp(rad);
  vec3_t r;
  r.x = p.x;
  r.y = fixmul(p.y, c) - fixmul(p.z, s);
  r.z = fixmul(p.y, s) + fixmul(p.z, c);
  return r;
}

/* Project 3D to 2D */
static void project(vec3_t p, int *sx, int *sy) {
  /* Camera offset */
  int dist = 3 * FP_ONE;
  int z_eff = p.z + dist;
  if (z_eff < 100)
    z_eff = 100;

  /* Use 64-bit math to prevent overflow */
  /* Projection formula: x_screen = (x * dist) / z */
  /* We scale by shifting right to fit screen coordinates */
  int64_t x_proj = ((int64_t)p.x * dist) / z_eff;
  int64_t y_proj = ((int64_t)p.y * dist) / z_eff;

  /* Centered on screen, scaling down from fixed point */
  *sx = WIN_W / 2 + (int)(x_proj >> 8);
  *sy = WIN_H / 2 - (int)(y_proj >> 8);
}

/* Fast Line Drawing to framebuffer */
static void draw_line_buf(int x0, int y0, int x1, int y1, unsigned int color) {
  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;

  while (1) {
    if (x0 >= 0 && x0 < WIN_W && y0 >= 0 && y0 < WIN_H) {
      framebuffer[y0 * WIN_W + x0] = color;
    }

    if (x0 == x1 && y0 == y1)
      break;

    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

/* Clear framebuffer */
/* Clear framebuffer */
static void clear_buffer(unsigned int color) {
  /* Use memset for black background (0) for maximum performance */
  if (color == 0) {
    memset(framebuffer, 0, BUFFER_SIZE * sizeof(unsigned int));
  } else {
    /* Fallback to loop for specific color */
    unsigned int *ptr = framebuffer;
    int n = BUFFER_SIZE;
    while (n-- > 0)
      *ptr++ = color;
  }
}

int main(void) {
  int pid = get_pid();
  char title[64];
  sprintf(title, "3D Demo (Icosahedron) PID %d", pid);

  int win = create_window(50, 50, WIN_W, WIN_H, title);
  if (win <= 0) {
    print("[Demo3D] Error creating window\n");
    exit(1);
  }

  printf("[Demo3D] High Performance Mode. PID %d\n", pid);

  init_shape(FP_ONE); /* Size 1.0 */

  int angle_y = 0;
  int angle_x = 0;

  while (1) {
    /* 1. Software Clear */
    /* 1. Software Clear (Black) */
    clear_buffer(0); /* Black to enable memset optimization */

    /* 2. Transform & Project */
    int sx[NUM_VERTS], sy[NUM_VERTS];
    for (int i = 0; i < NUM_VERTS; i++) {
      vec3_t v = verts[i];
      v = rotate_x(v, angle_x);
      v = rotate_y(v, angle_y);
      project(v, &sx[i], &sy[i]);
    }

    /* 3. Draw Points (Vertices) */
    for (int i = 0; i < NUM_VERTS; i++) {
      /* Draw small 3x3 rect for vertex */
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
          draw_line_buf(sx[i] + dx, sy[i] + dy, sx[i] + dx, sy[i] + dy,
                        0xFFFFFF00);
    }

    /* 4. Draw Edges from Connectivity Array (Pseudo-Icosahedron wireframe loop)
     */
    for (int i = 0; i < NUM_VERTS; i++) {
      int next = (i + 1) % NUM_VERTS;
      /* Draw perimeter loop */
      draw_line_buf(sx[i], sy[i], sx[next], sy[next], 0xFF00FF88);

      /* Cross connections for complexity */
      int cross = (i + 5) % NUM_VERTS;
      draw_line_buf(sx[i], sy[i], sx[cross], sy[cross], 0xFF0088FF);

      /* Another cross connection */
      int cross2 = (i + 3) % NUM_VERTS;
      draw_line_buf(sx[i], sy[i], sx[cross2], sy[cross2], 0xFFFF0088);
    }

    /* 5. Blit to Kernel Window (One Syscall!) */
    window_blit(win, 0, 0, WIN_W, WIN_H, framebuffer);

    /* 6. Compositor Render (Flush to Screen) */
    compositor_render();

    /* Rotate */
    angle_x = (angle_x + 1) % 360;
    angle_y = (angle_y + 2) % 360;
  }

  exit(0);
  return 0;
}
