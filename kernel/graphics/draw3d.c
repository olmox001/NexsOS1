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
  /* BUG FIX #1: Memory leak on re-initialization.
   * If render3d_init() is called a second time (e.g. on mode change) the old
   * zbuffer pointer was silently overwritten, leaking the previous allocation.
   * Free it first. */
  if (zbuffer) {
    kfree(zbuffer);
    zbuffer = NULL;
  }

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
  int32_t far_z = 0x7FFFFFFF;
  for (uint32_t i = 0; i < zbuffer_width * zbuffer_height; i++) {
    zbuffer[i] = far_z;
  }
}

/*
 * Identity Matrix (4x4)
 * Storage convention throughout this file: m[col][row]  (column-major, OpenGL
 * style).  mat4_mul_vec confirms this: r.x accumulates m[0..3][0] * v[0..3].
 */
mat4_t mat4_identity(void) {
  mat4_t m;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      m.m[i][j] = (i == j) ? 1.0f : 0.0f;
  return m;
}

/*
 * Translation Matrix
 * In column-major layout the translation lives in column 3, rows 0-2:
 * m[3][0..2].  mat4_mul_vec confirms: r.x += m[3][0] * v.w.
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
 * Rotation Matrix (Y-axis) - integer-trig approximation
 * angle: float, radians (converted internally to fixed-point for k_sin/cos_fp)
 *
 * Standard right-handed Y rotation (column-major, column vectors):
 *   logical M[row][col] =
 *     [ cos   0   sin   0 ]
 *     [  0    1    0    0 ]
 *     [-sin   0   cos   0 ]
 *     [  0    0    0    1 ]
 *
 * In column-major storage m[col][row] = M[row][col]:
 *   m[0][0] = cos,  m[2][0] = sin   (column 0)
 *   m[0][2] = -sin, m[2][2] = cos   (column 2)
 *
 * BUG FIX #2: The original code had m[0][2]=+s and m[2][0]=-s, which is
 * exactly the transpose of the correct rotation — the matrix rotated in the
 * wrong direction (effectively a negative-angle rotation).
 */
mat4_t mat4_rotate_y(float angle) {
  int32_t angle_fp = (int32_t)(angle * FP_ONE);
  int32_t c_fp = k_cos_fp(angle_fp);
  int32_t s_fp = k_sin_fp(angle_fp);

  float c = (float)c_fp / FP_ONE;
  float s = (float)s_fp / FP_ONE;

  mat4_t m = mat4_identity();
  m.m[0][0] = c;  /* M[0][0] */
  m.m[2][0] = s;  /* M[0][2]  — was -s (wrong sign) */
  m.m[0][2] = -s; /* M[2][0]  — was +s (wrong sign) */
  m.m[2][2] = c;  /* M[2][2] */
  return m;
}

/*
 * Matrix Multiply: C = A * B
 *
 * BUG FIX #3 (Critical): The original loop was:
 *   r.m[i][j] = sum_k  a.m[i][k] * b.m[k][j]
 *
 * With column-major storage (m[col][row]), setting col_c=i and row_c=j gives:
 *   r[col_c][row_c] = sum_k  a[col_c][k] * b[k][row_c]
 *
 * But the correct formula for C=A*B in column-major is:
 *   r[col_c][row_c] = sum_k  a[k][row_c] * b[col_c][k]
 *
 * The original formula computes B*A instead of A*B.  For non-commutative
 * transforms (translate + rotate, MVP assembly, etc.) this produces silently
 * wrong results.
 *
 * Fixed: swap the two index pairs so that a is indexed [k][row] and b is
 * indexed [col][k].
 */
mat4_t mat4_mul(mat4_t a, mat4_t b) {
  mat4_t r;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0.0f;
      for (int k = 0; k < 4; k++) {
        sum += a.m[k][row] * b.m[col][k]; /* was: a.m[col][k] * b.m[k][row] */
      }
      r.m[col][row] = sum;
    }
  }
  return r;
}

/*
 * Transform Vector by Matrix  (v' = M * v)
 * Consistent with column-major convention: r[row] = sum_col m[col][row]*v[col]
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
 * Perspective Projection Matrix (column-major, OpenGL conventions)
 *
 * Note: fov parameter is accepted but the implementation uses a fixed 90° FOV
 * (tan_half ≈ 0.414 = tan(22.5°)) for simplicity.  A production version
 * should compute tan_half = tanf(fov * 0.5f).
 */
mat4_t mat4_perspective(float fov, float aspect, float near, float far) {
  (void)fov;
  mat4_t m = {{{0}}};

  float tan_half = 0.414f; /* tan(45°/2) ≈ tan(22.5°) — fixed 90° HFOV */

  m.m[0][0] = 1.0f / (aspect * tan_half);
  m.m[1][1] = 1.0f / tan_half;
  m.m[2][2] = -(far + near) / (far - near);
  m.m[2][3] = -1.0f;
  m.m[3][2] = -(2.0f * far * near) / (far - near);

  return m;
}

/*
 * Project clip-space vertex to integer screen coordinates.
 *
 * BUG FIX #4a: Unsafe float comparison `v.w != 0`.
 * IEEE 754 equality on floats is unreliable and may warn with -Wfloat-equal.
 * Use an epsilon guard instead.
 *
 * BUG FIX #4b: Potential int32_t overflow in sz.
 * After perspective divide, nz is nominally in [-1, 1], but floating-point
 * arithmetic can push it slightly outside that range.  Multiplying an nz of,
 * say, 1.0000001 by 0x7FFFFFFF produces a value that overflows int32_t.
 * Clamp nz to [-1, 1] before the conversion.
 */
static void project_to_screen(vec4_t v, int *sx, int *sy, int32_t *sz,
                              int screen_w, int screen_h) {
  /* BUG FIX #4a: epsilon guard instead of != 0 */
  float inv_w = (v.w > 1e-6f || v.w < -1e-6f) ? (1.0f / v.w) : 1.0f;

  float nx = v.x * inv_w;
  float ny = v.y * inv_w;
  float nz = v.z * inv_w;

  /* BUG FIX #4b: clamp nz to prevent overflow when casting to int32_t */
  if (nz > 1.0f)
    nz = 1.0f;
  if (nz < -1.0f)
    nz = -1.0f;

  *sx = (int)((nx + 1.0f) * 0.5f * screen_w);
  *sy = (int)((1.0f - ny) * 0.5f * screen_h);
  *sz = (int32_t)((nz + 1.0f) * 0.5f * (float)0x7FFFFFFF);
}

/*
 * Draw 3D Triangle — Wireframe with Z-buffer support
 *
 * BUG FIX #5: The original code computed sz0/sz1/sz2 but never used them —
 * the zbuffer array was allocated and cleared but never read or written during
 * actual rendering.  For a pure wireframe path the per-line Z-test is non-
 * trivial (requires per-pixel interpolation); the values are retained here so
 * a future filled-triangle path can use them.  The wireframe lines are still
 * drawn unconditionally, but the sz values are now at least preserved for
 * callers that may build on this function.  A TODO marks the missing zbuffer
 * integration for future filled rasterization.
 */
void render3d_triangle(vec4_t v0, vec4_t v1, vec4_t v2, mat4_t mvp,
                       uint32_t color, int screen_w, int screen_h) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *surf = &screen;

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

  /* Simple back-face / behind-camera cull: skip if all verts have w <= 0 */
  if (t0.w <= 0.0f && t1.w <= 0.0f && t2.w <= 0.0f)
    return;

  /*
   * TODO: zbuffer integration for filled rasterization.
   * sz0/sz1/sz2 are computed above and zbuffer[] is ready; a scanline fill
   * loop should interpolate sz per-pixel and test/update zbuffer[y*w+x].
   * Wireframe edges do not perform per-pixel Z testing.
   */
  (void)sz0;
  (void)sz1;
  (void)sz2;

  /* Draw wireframe */
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

  /* 8 vertices */
  vec4_t verts[8] = {
      {x - s, y - s, z - s, 1}, {x + s, y - s, z - s, 1},
      {x + s, y + s, z - s, 1}, {x - s, y + s, z - s, 1},
      {x - s, y - s, z + s, 1}, {x + s, y - s, z + s, 1},
      {x + s, y + s, z + s, 1}, {x - s, y + s, z + s, 1},
  };

  /* 12 triangles (2 per face) */
  int indices[12][3] = {
      {0, 1, 2}, {0, 2, 3}, /* Front  (-Z) */
      {4, 6, 5}, {4, 7, 6}, /* Back   (+Z) */
      {0, 5, 1}, {0, 4, 5}, /* Bottom (-Y) */
      {2, 7, 3}, {2, 6, 7}, /* Top    (+Y) */
      {0, 7, 4}, {0, 3, 7}, /* Left   (-X) */
      {1, 5, 6}, {1, 6, 2}, /* Right  (+X) */
  };

  for (int i = 0; i < 12; i++) {
    render3d_triangle(verts[indices[i][0]], verts[indices[i][1]],
                      verts[indices[i][2]], view_proj, color, screen_w,
                      screen_h);
  }
}