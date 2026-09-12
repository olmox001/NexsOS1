/*
 * user/bin/demo3d.c
 * Solid 3D Cube Demo — fixed-point software rasterizer
 *
 * Improved renderer:
 *   - Deterministic 60 Hz simulation step with OS1 sleep pacing.
 *   - Fixed-time-step animation, independent of render workload.
 *   - Private color/depth buffers, uploaded through the native graphics API.
 *   - Per-pixel Z-buffer for correct hidden-surface ordering.
 *   - Edge-function/barycentric triangle rasterizer (crack-free).
 *   - Real perspective projection with configurable near plane.
 *   - Backface culling from the transformed 3D normal.
 *   - Advanced flat lighting: ambient + Lambert diffuse + Blinn-style specular.
 *   - Multicolor metallic test shader (iridescent / anodized metal look).
 *   - Keyboard controls through the OS1 input API.
 *   - Resize events consumed safely.
 *
 * Controls:
 *   ESC / Q     : quit
 *   SPACE       : pause/resume animation
 *   R           : reset rotation
 *   LEFT/RIGHT  : change Y rotation speed
 *   UP/DOWN     : change X rotation speed
 */

#include "graphics.h"
#include <input.h>
#include <os1.h>
#include <stddef.h>
#include <stdint.h>

#ifndef FP_SHIFT
#define FP_SHIFT 16
#endif
#ifndef FP_ONE
#define FP_ONE (1 << FP_SHIFT)
#endif

#define WIN_W 320
#define WIN_H 280
#define BUFFER_SIZE (WIN_W * WIN_H)
#define TARGET_FPS 60
#define FRAME_US (1000000ULL / TARGET_FPS)
#define CAMERA_Z (3 * FP_ONE)
#define NEAR_Z (FP_ONE / 4)
#define FAR_Z (10 * FP_ONE)
#define ROT_X_DEFAULT 55
#define ROT_Y_DEFAULT 110
#define ROT_MIN -900
#define ROT_MAX 900
#define BG_COLOR 0x00000000u /* transparent background */
#define Z_CLEAR 0x7FFFFFFFu

/* Metallic material parameters */
#define METAL_AMBIENT 55
#define METAL_DIFFUSE_SCALE 130
#define METAL_SPECULAR_POWER 3
#define METAL_SPECULAR_INTENSITY 90

static uint32_t color_buffer[BUFFER_SIZE];
static int32_t depth_buffer[BUFFER_SIZE];

typedef struct {
  int x, y, z;
} vec3_t;

typedef struct {
  int x, y;
  int z;
} screen_vertex_t;

#define NUM_VERTS 8
static vec3_t verts[NUM_VERTS];

static const int faces[6][4] = {
    {0, 3, 2, 1}, /* front  */
    {4, 5, 6, 7}, /* back   */
    {4, 7, 3, 0}, /* left   */
    {1, 2, 6, 5}, /* right  */
    {3, 7, 6, 2}, /* top    */
    {4, 0, 1, 5}  /* bottom */
};

/* Base albedo for each face (used as metal tint) */
static const uint32_t face_colors[6] = {
    0xFFE53935u, /* red    */
    0xFF43A047u, /* green  */
    0xFF1E88E5u, /* blue   */
    0xFFFFC107u, /* amber  */
    0xFF00ACC1u, /* cyan   */
    0xFF8E24AAu  /* purple */
};

/* ---------- fixed-point helpers (assumed provided by OS math) ---------- */
/* DEG_TO_FP_RAD, cos_fp, sin_fp, fixmul already exist in the environment */

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

static int clamp_int(int v, int lo, int hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

/* Approximate integer square root (good enough for lighting) */
static int isqrt(long long v) {
  if (v <= 0)
    return 0;
  long long x = v;
  long long y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return (int)x;
}

/*
 * Smooth multicolor metallic gradient.
 * Color shifts continuously with the reflection angle (no per-face jumps).
 * Base stays a muted metal, specular carries a smooth rainbow gradient.
 */
static uint32_t metal_shade(uint32_t base, int diffuse, int specular,
                            int angle) {
  /* Soft metal base (desaturated + lifted) */
  int br = ((base >> 16) & 0xFF);
  int bg = ((base >> 8) & 0xFF);
  int bb = (base & 0xFF);
  br = (br * 70 + 90) / 100;
  bg = (bg * 70 + 90) / 100;
  bb = (bb * 70 + 90) / 100;

  int dr = br * diffuse / 255;
  int dg = bg * diffuse / 255;
  int db = bb * diffuse / 255;

  /* Smooth rainbow from angle (0..255 → full cycle) */
  int h = (angle * 3) & 255; /* 0-255 */
  int sr, sg, sb;
  if (h < 85) {
    sr = 255;
    sg = h * 3;
    sb = 0;
  } else if (h < 170) {
    sr = 255 - (h - 85) * 3;
    sg = 255;
    sb = (h - 85) * 3;
  } else {
    sr = 0;
    sg = 255 - (h - 170) * 3;
    sb = 255;
  }

  /* Soft specular: colored gradient that fades, no full white faces */
  int rim = (specular * 70) / 255; /* max +70, never dominates */

  int r = clamp_int(dr + (sr * rim) / 255, 0, 240);
  int g = clamp_int(dg + (sg * rim) / 255, 0, 240);
  int b = clamp_int(db + (sb * rim) / 255, 0, 240);

  return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void clear_buffers(void) {
  for (int i = 0; i < BUFFER_SIZE; ++i) {
    color_buffer[i] = BG_COLOR;
    depth_buffer[i] = Z_CLEAR;
  }
}

/*
 * Perspective projection.
 * Cube stays in front of the near plane → no artificial z-clamp needed.
 */
static int project(vec3_t p, screen_vertex_t *out) {
  int z = p.z + CAMERA_Z;
  if (!out || z <= NEAR_Z || z >= FAR_Z)
    return 0;

  int focal =
      (WIN_W < WIN_H ? WIN_W : WIN_H) * 14 / 10; /* slightly stronger FOV */
  long long px = ((long long)p.x * focal) / z;
  long long py = ((long long)p.y * focal) / z;

  out->x = WIN_W / 2 + (int)px;
  out->y = WIN_H / 2 - (int)py;
  out->z = z;
  return 1;
}

static long long edge_function(int ax, int ay, int bx, int by, int px, int py) {
  return (long long)(px - ax) * (by - ay) - (long long)(py - ay) * (bx - ax);
}

/*
 * Barycentric software rasterizer with depth testing.
 * Inclusive edges so the two triangles of a quad never leave cracks.
 */
static void raster_triangle(screen_vertex_t a, screen_vertex_t b,
                            screen_vertex_t c, uint32_t color) {
  long long area = edge_function(a.x, a.y, b.x, b.y, c.x, c.y);
  if (area == 0)
    return;

  if (area < 0) {
    screen_vertex_t t = b;
    b = c;
    c = t;
    area = -area;
  }

  int min_x = a.x, max_x = a.x, min_y = a.y, max_y = a.y;
  if (b.x < min_x)
    min_x = b.x;
  if (c.x < min_x)
    min_x = c.x;
  if (b.x > max_x)
    max_x = b.x;
  if (c.x > max_x)
    max_x = c.x;
  if (b.y < min_y)
    min_y = b.y;
  if (c.y < min_y)
    min_y = c.y;
  if (b.y > max_y)
    max_y = b.y;
  if (c.y > max_y)
    max_y = c.y;

  min_x = clamp_int(min_x, 0, WIN_W - 1);
  max_x = clamp_int(max_x, 0, WIN_W - 1);
  min_y = clamp_int(min_y, 0, WIN_H - 1);
  max_y = clamp_int(max_y, 0, WIN_H - 1);

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      long long w0 = edge_function(b.x, b.y, c.x, c.y, x, y);
      long long w1 = edge_function(c.x, c.y, a.x, a.y, x, y);
      long long w2 = edge_function(a.x, a.y, b.x, b.y, x, y);

      if (w0 < 0 || w1 < 0 || w2 < 0)
        continue;

      long long z = ((w0 * a.z) + (w1 * b.z) + (w2 * c.z)) / area;
      if (z < NEAR_Z || z > FAR_Z)
        continue;

      int index = y * WIN_W + x;
      if ((int32_t)z < depth_buffer[index]) {
        depth_buffer[index] = (int32_t)z;
        color_buffer[index] = color;
      }
    }
  }
}

/*
 * Multicolor metallic lighting (flat per face).
 * Returns the final shaded color and sets *visible.
 */
static uint32_t face_lit_color(vec3_t a, vec3_t b, vec3_t c, uint32_t base,
                               int *visible) {
  /* Face normal via cross product */
  long long ux = (long long)b.x - a.x;
  long long uy = (long long)b.y - a.y;
  long long uz = (long long)b.z - a.z;
  long long vx = (long long)c.x - a.x;
  long long vy = (long long)c.y - a.y;
  long long vz = (long long)c.z - a.z;

  long long nx = uy * vz - uz * vy;
  long long ny = uz * vx - ux * vz;
  long long nz = ux * vy - uy * vx;

  /* Back-face cull (camera looks toward +Z, visible faces have nz < 0) */
  if (nz >= 0) {
    *visible = 0;
    return base;
  }
  *visible = 1;

  /* Light direction (upper-left, slightly front) */
  const long long lx = -(FP_ONE * 2) / 5;
  const long long ly = (FP_ONE * 4) / 5;
  const long long lz = -(FP_ONE * 3) / 4;

  /* View direction (camera looks along +Z → view vector ≈ (0,0,-1)) */
  const long long vx_ = 0;
  const long long vy_ = 0;
  const long long vz_ = -FP_ONE;

  long long n2 = nx * nx + ny * ny + nz * nz;
  if (n2 == 0) {
    *visible = 0;
    return base;
  }

  /* Normalize N approximately */
  int nlen = isqrt(n2);
  if (nlen < 1)
    nlen = 1;
  long long nnx = (nx * FP_ONE) / nlen;
  long long nny = (ny * FP_ONE) / nlen;
  long long nnz = (nz * FP_ONE) / nlen;

  /* Diffuse (Lambert) */
  long long l2 = lx * lx + ly * ly + lz * lz;
  int llen = isqrt(l2);
  if (llen < 1)
    llen = 1;
  long long nlx = (lx * FP_ONE) / llen;
  long long nly = (ly * FP_ONE) / llen;
  long long nlz = (lz * FP_ONE) / llen;

  long long ndotl = (nnx * nlx + nny * nly + nnz * nlz) >> FP_SHIFT;
  if (ndotl < 0)
    ndotl = 0;

  int diffuse = (int)((ndotl * METAL_DIFFUSE_SCALE) >> FP_SHIFT);
  diffuse = clamp_int(diffuse + METAL_AMBIENT, 0, 255);

  /* Blinn-Phong half-vector specular */
  long long hx = nlx + vx_;
  long long hy = nly + vy_;
  long long hz = nlz + vz_;
  long long h2 = hx * hx + hy * hy + hz * hz;
  int hlen = isqrt(h2);
  if (hlen < 1)
    hlen = 1;
  long long nhx = (hx * FP_ONE) / hlen;
  long long nhy = (hy * FP_ONE) / hlen;
  long long nhz = (hz * FP_ONE) / hlen;

  long long ndoth = (nnx * nhx + nny * nhy + nnz * nhz) >> FP_SHIFT;
  if (ndoth < 0)
    ndoth = 0;

  /* Power approximation (cheap integer power) */
  int spec = (int)((ndoth * 255) >> FP_SHIFT);
  for (int i = 1; i < METAL_SPECULAR_POWER; ++i) {
    spec = (spec * (int)((ndoth * 255) >> FP_SHIFT)) / 255;
  }
  spec = clamp_int((spec * METAL_SPECULAR_INTENSITY) / 255, 0, 255);

  /* Continuous angle for smooth gradient (derived from N·H, no face jumps) */
  int angle = (int)((ndoth * 255) >> FP_SHIFT);
  angle = clamp_int(angle, 0, 255);

  return metal_shade(base, diffuse, spec, angle);
}

/*
 * Render the complete cube into the private buffers.
 */
static void render_cube(int angle_x, int angle_y) {
  vec3_t transformed[NUM_VERTS];
  screen_vertex_t projected[NUM_VERTS];
  int projected_ok[NUM_VERTS];

  for (int i = 0; i < NUM_VERTS; ++i) {
    vec3_t v = rotate_x(verts[i], angle_x);
    v = rotate_y(v, angle_y);
    transformed[i] = v;
    projected_ok[i] = project(v, &projected[i]);
  }

  for (int i = 0; i < 6; ++i) {
    int i0 = faces[i][0];
    int i1 = faces[i][1];
    int i2 = faces[i][2];
    int i3 = faces[i][3];

    if (!projected_ok[i0] || !projected_ok[i1] || !projected_ok[i2] ||
        !projected_ok[i3])
      continue;

    int visible = 0;
    uint32_t color = face_lit_color(transformed[i0], transformed[i1],
                                    transformed[i2], face_colors[i], &visible);
    if (!visible)
      continue;

    raster_triangle(projected[i0], projected[i1], projected[i2], color);
    raster_triangle(projected[i0], projected[i2], projected[i3], color);
  }
}

static void present_frame(int win_id) {
  graphics_blit(win_id, 0, 0, WIN_W, WIN_H, color_buffer);
}

#define SIM_DT_US FRAME_US

static void timer_sleep_frame(void) {
  OS1_sleep((unsigned int)(FRAME_US / 1000ULL));
}

static int advance_angle(int angle, int degrees_per_second, unsigned int dt_us,
                         unsigned int *remainder_us) {
  unsigned long long value =
      (unsigned long long)*remainder_us +
      (unsigned long long)(degrees_per_second < 0 ? -degrees_per_second
                                                  : degrees_per_second) *
          (unsigned long long)dt_us;
  int delta = (int)(value / 1000000ULL);
  *remainder_us = (unsigned int)(value % 1000000ULL);

  if (degrees_per_second < 0)
    angle -= delta;
  else
    angle += delta;

  while (angle >= 360)
    angle -= 360;
  while (angle < 0)
    angle += 360;
  return angle;
}

static void handle_input(int *running, int *paused, int *speed_x, int *speed_y,
                         int *angle_x, int *angle_y) {
  input_event_t event;
  while (input_poll_event(&event) > 0) {
    if (event.type == INPUT_TYPE_KEYBOARD) {
      if (event.keyboard.state == KEY_PRESSED ||
          event.keyboard.state == KEY_REPEAT) {
        unsigned char key = event.keyboard.key;
        uint16_t sc = event.keyboard.scancode;

        if (key == 'q' || key == 'Q' || sc == INPUT_KEY_ESC) {
          *running = 0;
        } else if (key == ' ') {
          *paused = !*paused;
        } else if (key == 'r' || key == 'R') {
          *angle_x = 0;
          *angle_y = 0;
        } else if (sc == INPUT_KEY_LEFT) {
          *speed_y -= 18;
          if (*speed_y < ROT_MIN)
            *speed_y = ROT_MIN;
        } else if (sc == INPUT_KEY_RIGHT) {
          *speed_y += 18;
          if (*speed_y > ROT_MAX)
            *speed_y = ROT_MAX;
        } else if (sc == INPUT_KEY_UP) {
          *speed_x += 18;
          if (*speed_x > ROT_MAX)
            *speed_x = ROT_MAX;
        } else if (sc == INPUT_KEY_DOWN) {
          *speed_x -= 18;
          if (*speed_x < ROT_MIN)
            *speed_x = ROT_MIN;
        }
      }
    } else if (event.type == INPUT_TYPE_RESIZE) {
      (void)event.resize.w;
      (void)event.resize.h;
    }
  }
}

int main(void) {
  int pid = get_pid();
  char title[64];
  snprintf(title, sizeof(title), "3D Metal Cube - PID %d", pid);

  int win_id = _sys_create_window(40, 40, WIN_W, WIN_H, title);
  if (win_id < 0) {
    print("[Demo3D] Error creating window\n");
    exit(1);
  }

  OS1_notify_post("Demo3D", "Multicolor metal shader ready");

  init_shape(FP_ONE / 2);

  int angle_x = 25;
  int angle_y = 35;
  int speed_x = ROT_X_DEFAULT;
  int speed_y = ROT_Y_DEFAULT;
  int paused = 0;
  int running = 1;
  unsigned int angle_x_remainder_us = 0;
  unsigned int angle_y_remainder_us = 0;

  while (running) {
    handle_input(&running, &paused, &speed_x, &speed_y, &angle_x, &angle_y);
    if (!running)
      break;

    clear_buffers();

    if (!paused)
      render_cube(angle_x, angle_y);

    present_frame(win_id);

    if (!paused) {
      angle_x = advance_angle(angle_x, speed_x, (unsigned int)SIM_DT_US,
                              &angle_x_remainder_us);
      angle_y = advance_angle(angle_y, speed_y, (unsigned int)SIM_DT_US,
                              &angle_y_remainder_us);
    }

    timer_sleep_frame();
  }

  _sys_destroy_window(win_id);
  exit(0);
  return 0;
}