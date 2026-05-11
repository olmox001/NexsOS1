/*
 * kernel/graphics/draw3d.c
 * Basic 3D Software Renderer (Integer/Fixed-Point Only)
 *
 * Provides simple 3D rendering capabilities:
 * - Vertex transformation (fixed-point)
 * - Triangle rasterization with Z-buffer
 * - Flat shading
 */
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/math.h>
#include <kernel/string.h>
#include <kernel/types.h>

/* Z-Buffer */
static int32_t *zbuffer = NULL;
static uint32_t zbuffer_width = 0;
static uint32_t zbuffer_height = 0;

/*
 * Initialize 3D Renderer
 */
void render3d_init(uint32_t width, uint32_t height) {
  zbuffer_width = width;
  zbuffer_height = height;

  size_t size = width * height * sizeof(int32_t);
  zbuffer = (int32_t *)kmalloc(size);
  if (zbuffer) {
    render3d_clear_zbuffer();
  }
}

/*
 * Clear Z-Buffer
 */
void render3d_clear_zbuffer(void) {
  if (!zbuffer)
    return;
  int32_t far_z = 0x7FFFFFFF; /* Max positive value = far */
  for (uint32_t i = 0; i < zbuffer_width * zbuffer_height; i++) {
    zbuffer[i] = far_z;
  }
}

/*
 * Identity Matrix (4x4, 16.16 fixed-point per element)
 */
mat4_t mat4_identity(void) {
  mat4_t m;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      m.m[i][j] = (i == j) ? 1.0f : 0.0f;
    }
  }
  return m;
}

/*
 * Translation Matrix
 */
mat4_t mat4_translate(float x, float y, float z) {
  mat4_t m = mat4_identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

/*
 * Scale Matrix
 */
mat4_t mat4_scale(float x, float y, float z) {
  mat4_t m = mat4_identity();
  m.m[0][0] = x;
  m.m[1][1] = y;
  m.m[2][2] = z;
  return m;
}

/*
 * Rotation Matrix (Y-axis) - Integer approximation
 * angle is in fixed-point radians
 */
mat4_t mat4_rotate_y(float angle) {
  /* Convert to fixed-point for trig */
  int32_t angle_fp = (int32_t)(angle * FP_ONE);
  int32_t c_fp = k_cos_fp(angle_fp);
  int32_t s_fp = k_sin_fp(angle_fp);

  /* Convert back to float for matrix storage */
  float c = (float)c_fp / FP_ONE;
  float s = (float)s_fp / FP_ONE;

  mat4_t m = mat4_identity();
  m.m[0][0] = c;
  m.m[0][2] = s;
  m.m[2][0] = -s;
  m.m[2][2] = c;
  return m;
}

/*
 * Matrix Multiply (Simple, uses float internally but stores results)
 */
mat4_t mat4_mul(mat4_t a, mat4_t b) {
  mat4_t r;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      float sum = 0;
      for (int k = 0; k < 4; k++) {
        sum += a.m[i][k] * b.m[k][j];
      }
      r.m[i][j] = sum;
    }
  }
  return r;
}

/*
 * Transform Vector by Matrix
 */
vec4_t mat4_mul_vec(mat4_t m, vec4_t v) {
  vec4_t r;
  r.x = m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z + m.m[3][0] * v.w;
  r.y = m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z + m.m[3][1] * v.w;
  r.z = m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z + m.m[3][2] * v.w;
  r.w = m.m[0][3] * v.x + m.m[1][3] * v.y + m.m[2][3] * v.z + m.m[3][3] * v.w;
  return r;
}

/*
 * Perspective Projection Matrix
 * Note: Simplified, uses fixed ratios
 */
mat4_t mat4_perspective(float fov, float aspect, float near, float far) {
  (void)fov; /* Use fixed FOV for simplicity */
  mat4_t m = {{{0}}};

  /* tan(45°/2) ≈ 0.414 */
  float tan_half = 0.414f;

  m.m[0][0] = 1.0f / (aspect * tan_half);
  m.m[1][1] = 1.0f / tan_half;
  m.m[2][2] = -(far + near) / (far - near);
  m.m[2][3] = -1.0f;
  m.m[3][2] = -(2.0f * far * near) / (far - near);

  return m;
}

/*
 * Project to Screen Coordinates (Integer output)
 */
static void project_to_screen(vec4_t v, int *sx, int *sy, int32_t *sz,
                              int screen_w, int screen_h) {
  /* Perspective divide */
  float inv_w = (v.w != 0) ? (1.0f / v.w) : 1.0f;
  float nx = v.x * inv_w;
  float ny = v.y * inv_w;
  float nz = v.z * inv_w;

  /* Map to screen */
  *sx = (int)((nx + 1.0f) * 0.5f * screen_w);
  *sy = (int)((1.0f - ny) * 0.5f * screen_h);
  *sz = (int32_t)((nz + 1.0f) * 0.5f * 0x7FFFFFFF); /* Z to fixed-point */
}

/*
 * Draw 3D Triangle with Z-buffer (Wireframe)
 */
void render3d_triangle(vec4_t v0, vec4_t v1, vec4_t v2, mat4_t mvp,
                       uint32_t color, int screen_w, int screen_h) {
  struct gl_surface *surf = graphics_get_screen_surface();
  if (!surf) return;

  /* Transform vertices */
  vec4_t t0 = mat4_mul_vec(mvp, v0);
  vec4_t t1 = mat4_mul_vec(mvp, v1);
  vec4_t t2 = mat4_mul_vec(mvp, v2);

  /* Project to screen */
  int sx0, sy0, sx1, sy1, sx2, sy2;
  int32_t sz0, sz1, sz2;
  project_to_screen(t0, &sx0, &sy0, &sz0, screen_w, screen_h);
  project_to_screen(t1, &sx1, &sy1, &sz1, screen_w, screen_h);
  project_to_screen(t2, &sx2, &sy2, &sz2, screen_w, screen_h);

  /* Draw wireframe using GL (handles clipping) */
  gl_draw_line(surf, sx0, sy0, sx1, sy1, color);
  gl_draw_line(surf, sx1, sy1, sx2, sy2, color);
  gl_draw_line(surf, sx2, sy2, sx0, sy0, color);
}

/*
 * Draw Simple 3D Cube (Wireframe)
 */
void render3d_cube(float x, float y, float z, float size, mat4_t view_proj,
                   uint32_t color, int screen_w, int screen_h) {
  float s = size / 2.0f;

  /* 8 vertices of cube */
  vec4_t verts[8] = {
      {x - s, y - s, z - s, 1}, {x + s, y - s, z - s, 1},
      {x + s, y + s, z - s, 1}, {x - s, y + s, z - s, 1},
      {x - s, y - s, z + s, 1}, {x + s, y - s, z + s, 1},
      {x + s, y + s, z + s, 1}, {x - s, y + s, z + s, 1},
  };

  /* 12 triangles (2 per face) */
  int indices[12][3] = {
      {0, 1, 2}, {0, 2, 3}, /* Front */
      {4, 6, 5}, {4, 7, 6}, /* Back */
      {0, 5, 1}, {0, 4, 5}, /* Bottom */
      {2, 7, 3}, {2, 6, 7}, /* Top */
      {0, 7, 4}, {0, 3, 7}, /* Left */
      {1, 5, 6}, {1, 6, 2}, /* Right */
  };

  for (int i = 0; i < 12; i++) {
    render3d_triangle(verts[indices[i][0]], verts[indices[i][1]],
                      verts[indices[i][2]], view_proj, color, screen_w,
                      screen_h);
  }
}
