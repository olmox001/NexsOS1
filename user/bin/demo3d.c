/*
 * user/bin/demo3d.c
 * Solid 3D Cube Demo with Software Rasterization
 * Implements Flat Shading and Backface Culling
 */
#include <os1.h>

/* Fixed-point 16.16 */
#ifndef FP_SHIFT
#define FP_SHIFT 16
#endif /* FP_SHIFT */
#ifndef FP_ONE
#define FP_ONE (1 << FP_SHIFT)
#endif /* FP_ONE */

#define WIN_W 300
#define WIN_H 250
#define BUFFER_SIZE (WIN_W * WIN_H)

/* Framebuffer */
static unsigned int framebuffer[BUFFER_SIZE];

/* 3D Point */
typedef struct {
  int x, y, z; /* Fixed-point 16.16 */
} vec3_t;

/* Cube: 8 vertices */
#define NUM_VERTS 8
static vec3_t verts[NUM_VERTS];

/* Initialize cube vertices */
static void init_shape(int s) {
  verts[0] = (vec3_t){-s, -s, -s};
  verts[1] = (vec3_t){s, -s, -s};
  verts[2] = (vec3_t){s, s, -s};
  verts[3] = (vec3_t){-s, s, -s};
  verts[4] = (vec3_t){-s, -s, s};
  verts[5] = (vec3_t){s, -s, s};
  verts[6] = (vec3_t){s, s, s};
  verts[7] = (vec3_t){-s, s, s};
}

/* Face definition (counter-clockwise winding from outside) */
static int faces[6][4] = {
    {0, 3, 2, 1}, /* Front */
    {4, 5, 6, 7}, /* Back */
    {4, 7, 3, 0}, /* Left */
    {1, 2, 6, 5}, /* Right */
    {3, 7, 6, 2}, /* Top */
    {4, 0, 1, 5}  /* Bottom */
};

/* Base colors for the 6 faces */
static unsigned int face_colors[6] = {
    0xFF3333, /* Red */
    0x33FF33, /* Green */
    0x3333FF, /* Blue */
    0xFFFF33, /* Yellow */
    0x33FFFF, /* Cyan */
    0xFF33FF  /* Magenta */
};

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
  int dist = 3 * FP_ONE;
  int z_eff = p.z + dist;
  if (z_eff < 100)
    z_eff = 100;

  long long x_proj = ((long long)p.x * dist) / z_eff;
  long long y_proj = ((long long)p.y * dist) / z_eff;

  int proj_x = (int)(x_proj >> 8);
  int proj_y = (int)(y_proj >> 8);

  /* Scala ridotta al 75% per rientrare nella finestra senza risultare troppo
   * piccola */
  *sx = WIN_W / 2 + (proj_x * 3) / 4;
  *sy = WIN_H / 2 - (proj_y * 3) / 4;
}

/* * Flat-shaded software triangle rasterizer
 * Splits the generic triangle into top and bottom flat sections.
 */
static void fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3,
                          unsigned int color) {
  /* Sort vertices by Y ascending */
  if (y1 > y2) {
    int t = x1;
    x1 = x2;
    x2 = t;
    t = y1;
    y1 = y2;
    y2 = t;
  }
  if (y2 > y3) {
    int t = x2;
    x2 = x3;
    x3 = t;
    t = y2;
    y2 = y3;
    y3 = t;
  }
  if (y1 > y2) {
    int t = x1;
    x1 = x2;
    x2 = t;
    t = y1;
    y1 = y2;
    y2 = t;
  }

  if (y1 == y3)
    return; /* Degenerate */

  long long dx13 = x3 - x1;
  long long dy13 = y3 - y1;
  long long step13 = dy13 ? (dx13 << 16) / dy13 : 0;

  long long curx1 = (long long)x1 << 16;
  long long curx2 = (long long)x1 << 16;

  /* Top half */
  if (y2 > y1) {
    long long dx12 = x2 - x1;
    long long dy12 = y2 - y1;
    long long step12 = dy12 ? (dx12 << 16) / dy12 : 0;
    for (int y = y1; y < y2; y++) {
      int cx1 = curx1 >> 16;
      int cx2 = curx2 >> 16;
      if (cx1 > cx2) {
        int t = cx1;
        cx1 = cx2;
        cx2 = t;
      }

      if (y >= 0 && y < WIN_H) {
        int start = cx1 < 0 ? 0 : cx1;
        int end = cx2 >= WIN_W ? WIN_W - 1 : cx2;
        for (int x = start; x <= end; x++) {
          framebuffer[y * WIN_W + x] = color;
        }
      }
      curx1 += step13;
      curx2 += step12;
    }
  }

  /* Bottom half */
  curx2 = (long long)x2 << 16;
  if (y3 > y2) {
    long long dx23 = x3 - x2;
    long long dy23 = y3 - y2;
    long long step23 = dy23 ? (dx23 << 16) / dy23 : 0;
    for (int y = y2; y <= y3; y++) {
      int cx1 = curx1 >> 16;
      int cx2 = curx2 >> 16;
      if (cx1 > cx2) {
        int t = cx1;
        cx1 = cx2;
        cx2 = t;
      }

      if (y >= 0 && y < WIN_H) {
        int start = cx1 < 0 ? 0 : cx1;
        int end = cx2 >= WIN_W ? WIN_W - 1 : cx2;
        for (int x = start; x <= end; x++) {
          framebuffer[y * WIN_W + x] = color;
        }
      }
      curx1 += step13;
      curx2 += step23;
    }
  }
}

/* Clear framebuffer */
static void clear_buffer(unsigned int color) {
  if (color == 0) {
    memset(framebuffer, 0, BUFFER_SIZE * sizeof(unsigned int));
  } else {
    unsigned int *ptr = framebuffer;
    int n = BUFFER_SIZE;
    while (n-- > 0)
      *ptr++ = color;
  }
}

int main(void) {
  int pid = get_pid();
  char title[64];
  sprintf(title, "3D Demo (Solid Cube) PID %d", pid);

  int win = create_window(50, 50, WIN_W, WIN_H, title);
  if (win <= 0) {
    print("[Demo3D] Error creating window\n");
    exit(1);
  }

  printf("[Demo3D] Real Solid GL Engine Init. PID %d\n", pid);

  /* Cube size scaled to fit window optimally */
  init_shape(FP_ONE / 3);

  int angle_y = 0;
  int angle_x = 0;

  while (1) {
    clear_buffer(0); /* Dark gray background */

    vec3_t transformed[NUM_VERTS];
    int sx[NUM_VERTS], sy[NUM_VERTS];

    /* Trasformazione e proiezione matematica dei vertici */
    for (int i = 0; i < NUM_VERTS; i++) {
      vec3_t v = verts[i];
      v = rotate_x(v, angle_x);
      v = rotate_y(v, angle_y);
      transformed[i] = v;
      project(v, &sx[i], &sy[i]);
    }

    /* Rasterizzazione delle 6 facce */
    for (int i = 0; i < 6; i++) {
      vec3_t t0 = transformed[faces[i][0]];
      vec3_t t1 = transformed[faces[i][1]];
      vec3_t t2 = transformed[faces[i][2]];

      /* Vettori sullo spazio 3D trasformato per cross product */
      long long v1x = t1.x - t0.x;
      long long v1y = t1.y - t0.y;
      long long v2x = t2.x - t0.x;
      long long v2y = t2.y - t0.y;

      /* Calcolo normale Asse Z (Profondità) ridotto in scala */
      long long nz = (v1x * v2y - v1y * v2x) >> FP_SHIFT;

      /* Backface culling e Shading (nz < 0 = faccia rivolta alla telecamera) */
      if (nz < 0) {
        long long max_nz =
            (4 * FP_ONE) /
            9; /* Massima estensione derivata dalla scala FP_ONE / 3 */
        if (max_nz == 0)
          max_nz = 1;

        int ambient = 40;
        int intensity = ambient + (int)((-nz * (255 - ambient)) / max_nz);
        if (intensity > 255)
          intensity = 255;
        if (intensity < 0)
          intensity = 0;

        /* Applicazione intensità Flat Shading sul colore base */
        unsigned int base = face_colors[i];
        int r = ((base >> 16) & 0xFF) * intensity / 255;
        int g = ((base >> 8) & 0xFF) * intensity / 255;
        int b = (base & 0xFF) * intensity / 255;
        unsigned int shaded = 0xFF000000 | (r << 16) | (g << 8) | b;

        int s0x = sx[faces[i][0]], s0y = sy[faces[i][0]];
        int s1x = sx[faces[i][1]], s1y = sy[faces[i][1]];
        int s2x = sx[faces[i][2]], s2y = sy[faces[i][2]];
        int s3x = sx[faces[i][3]], s3y = sy[faces[i][3]];

        /* Costruzione delle primitive a triangolo */
        fill_triangle(s0x, s0y, s1x, s1y, s2x, s2y, shaded);
        fill_triangle(s0x, s0y, s2x, s2y, s3x, s3y, shaded);
      }
    }

    /* Syscall di upload */
    window_blit(win, 0, 0, WIN_W, WIN_H, framebuffer);
    compositor_render();

    /* Modifica i gradi di rotazione limitandoli a 360 per prevenire overflow
     * numerici */
    angle_x = (angle_x + 1) % 360;
    angle_y = (angle_y + 2) % 360;
  }

  exit(0);
  return 0;
}