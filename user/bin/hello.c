/*
 * blackhole_os1.c
 * Schwarzschild null geodesics (RK4) + realistic accretion disk + C²
 * embedding W(r) — complete CPU port for NexsOS / OS1 software video.
 *
 * Build (link against libm and the OS1 libc / os1_gl_platform):
 *   clang -O2 -ffast-math -o blackhole_os1 blackhole_os1.c \
 *         os1_gl_platform.c -lm -los1
 *
 * Controls (keyboard):
 *   1          mode Raytrace
 *   2          mode Embedding
 *   A / Z      increase / decrease mass
 *   D / C      increase / decrease disk inclination
 *   Q / W      increase / decrease r0_emb
 *   S / X      increase / decrease a4_emb
 *   R          reset camera & physics
 *   Arrows     rotate camera (yaw / pitch)
 *   = / -      zoom in / out
 * Mouse:
 *   Left drag  rotate camera
 *
 * The renderer is CPU-bound; at 960x540 it is faithful to the original.
 * Reduce WIDTH/HEIGHT for faster frame rates on slower cores.
 */
#include "../sys/lib/portability/opengl/os1_gl_platform.h"
#include <input.h>
#include <math.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */
#define WIDTH 960
#define HEIGHT 540

enum Mode { MODE_RAYTRACE = 0, MODE_EMBEDDING = 1 };
static int mode = MODE_RAYTRACE;

static float camYaw = 0.0f;
static float camPitch = 0.28f;
static float camDist = 13.5f;
static float mass = 1.0f;
static float diskIncl = 1.18f;

/* C² embedding parameters */
static float r0_emb = 1.85f;
static float a0_emb = -2.9f;
static float a4_emb = 0.12f;
static float b0_emb = 0.0f;
static float a2_emb = 0.0f, b1_emb = 0.0f, b2_emb = 0.0f;

static int mouseDown = 0;
static int lastMX = 0, lastMY = 0;

/* ------------------------------------------------------------------ */
/* Vector math (float3 replacement)                                   */
/* ------------------------------------------------------------------ */
typedef struct {
  float x, y, z;
} vec3;

static inline vec3 make_vec3(float x, float y, float z) {
  return (vec3){x, y, z};
}

static inline vec3 vadd(vec3 a, vec3 b) {
  return make_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline vec3 vsub(vec3 a, vec3 b) {
  return make_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline vec3 vscale(vec3 a, float s) {
  return make_vec3(a.x * s, a.y * s, a.z * s);
}
static inline vec3 vmul(vec3 a, vec3 b) {
  return make_vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}
static inline vec3 vdiv(vec3 a, vec3 b) {
  return make_vec3(a.x / b.x, a.y / b.y, a.z / b.z);
}
static inline float vdot(vec3 a, vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline vec3 vcross(vec3 a, vec3 b) {
  return make_vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                   a.x * b.y - a.y * b.x);
}
static inline float vlength(vec3 a) {
  return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}
static inline vec3 vnormalize(vec3 a) {
  float len = vlength(a);
  if (len < 1e-12f)
    return make_vec3(0.0f, 0.0f, 0.0f);
  return vscale(a, 1.0f / len);
}
static inline vec3 vmix(vec3 a, vec3 b, float t) {
  return vadd(vscale(a, 1.0f - t), vscale(b, t));
}

static inline float clampf(float x, float mn, float mx) {
  if (x < mn)
    return mn;
  if (x > mx)
    return mx;
  return x;
}

static inline float smoothstep(float edge0, float edge1, float x) {
  float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline vec3 rotateY(vec3 p, float a) {
  float c = cosf(a), s = sinf(a);
  return make_vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
}
static inline vec3 rotateX(vec3 p, float a) {
  float c = cosf(a), s = sinf(a);
  return make_vec3(p.x, c * p.y - s * p.z, s * p.y + c * p.z);
}

/* ------------------------------------------------------------------ */
/* C² embedding coefficient solver                                    */
/* ------------------------------------------------------------------ */
static float det3_c(float m[3][3]) {
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
         m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

static void solve3x3(float A[3][3], float B[3], float X[3]) {
  float detA = det3_c(A);
  if (fabsf(detA) < 1e-12f) {
    X[0] = X[1] = X[2] = 0.0f;
    return;
  }
  float M[3][3];
  for (int col = 0; col < 3; ++col) {
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        M[i][j] = (j == col) ? B[i] : A[i][j];
    X[col] = det3_c(M) / detA;
  }
}

static void computeC2Coefficients(void) {
  float r = r0_emb, r2 = r * r, r3 = r2 * r, r4 = r2 * r2;
  float Mtx[3][3] = {{r2, -1.0f / r, -1.0f / r2},
                     {2.0f * r, 1.0f / r2, 2.0f / r3},
                     {2.0f, -2.0f / r3, -6.0f / r4}};
  float B[3] = {b0_emb - a0_emb - a4_emb * r4, -4.0f * a4_emb * r3,
                -12.0f * a4_emb * r2};
  float X[3];
  solve3x3(Mtx, B, X);
  a2_emb = X[0];
  b1_emb = X[1];
  b2_emb = X[2];
}

static float W_embed(float r) {
  if (r <= r0_emb) {
    float r2 = r * r;
    return a0_emb + a2_emb * r2 + a4_emb * r2 * r2;
  }
  return b0_emb + b1_emb / r + b2_emb / (r * r);
}

/* ------------------------------------------------------------------ */
/* Shading & physics helpers                                          */
/* ------------------------------------------------------------------ */
static vec3 stars(vec3 dir) {
  vec3 col = make_vec3(0.0f, 0.0f, 0.0f);
  for (int i = 0; i < 48; i++) {
    float fi = (float)i;
    float h1 = sinf(fi * 127.1f) * 43758.5453f;
    float h2 = sinf(fi * 269.5f) * 43758.5453f;
    float h3 = sinf(fi * 419.2f) * 43758.5453f;
    vec3 p = vnormalize(make_vec3((h1 - floorf(h1)) * 2.0f - 1.0f,
                                  (h2 - floorf(h2)) * 2.0f - 1.0f,
                                  (h3 - floorf(h3)) * 2.0f - 1.0f));
    float d = vdot(dir, p);
    if (d > 0.0f) {
      float intensity = powf(d, 420.0f) * 2.8f;
      col.x += 1.00f * intensity;
      col.y += 0.94f * intensity;
      col.z += 0.88f * intensity;
    }
  }
  col.x += 0.016f * (0.3f + 0.7f * dir.y);
  col.y += 0.020f * (0.3f + 0.7f * dir.y);
  col.z += 0.040f * (0.3f + 0.7f * dir.y);
  return col;
}

static vec3 blackbody(float T) {
  T = clampf(T, 800.0f, 40000.0f);
  vec3 c;
  if (T < 1500.0f) {
    c = make_vec3(1.0f, 0.15f + 0.25f * (T - 800.0f) / 700.0f, 0.02f);
  } else if (T < 4000.0f) {
    float f = (T - 1500.0f) / 2500.0f;
    c = vmix(make_vec3(1.0f, 0.4f, 0.05f), make_vec3(1.0f, 0.85f, 0.55f), f);
  } else if (T < 8000.0f) {
    float f = (T - 4000.0f) / 4000.0f;
    c = vmix(make_vec3(1.0f, 0.85f, 0.55f), make_vec3(0.9f, 0.92f, 1.0f), f);
  } else {
    float f = clampf((T - 8000.0f) / 20000.0f, 0.0f, 1.0f);
    c = vmix(make_vec3(0.9f, 0.92f, 1.0f), make_vec3(0.7f, 0.8f, 1.0f), f);
  }
  float scale = 0.4f + 1.6f * logf(T / 1000.0f) / logf(40.0f);
  return vscale(c, scale);
}

static vec3 diskColor(float r, float M) {
  float r_isco = 6.0f * M;
  float x = fmaxf(r / r_isco, 0.2f);
  float T = 18000.0f / powf(x, 0.75f);
  float dens = expf(-powf((r - 8.5f * M) / (7.0f * M), 2.0f));
  return vscale(blackbody(T), dens * 1.35f);
}

/* ------------------------------------------------------------------ */
/* Geodesic RK4 (u = 1/r)                                             */
/* ------------------------------------------------------------------ */
typedef struct {
  float u;
  float du;
} OrbitState;

static inline float geodesic_f(float u, float M) {
  return -u + 3.0f * M * u * u;
}

static OrbitState rk4_step(OrbitState s, float h, float M) {
  float k1_du = geodesic_f(s.u, M);
  float k1_u = s.du;

  float u2 = s.u + 0.5f * h * k1_u;
  float du2 = s.du + 0.5f * h * k1_du;
  float k2_du = geodesic_f(u2, M);
  float k2_u = du2;

  float u3 = s.u + 0.5f * h * k2_u;
  float du3 = s.du + 0.5f * h * k2_du;
  float k3_du = geodesic_f(u3, M);
  float k3_u = du3;

  float u4 = s.u + h * k3_u;
  float du4 = s.du + h * k3_du;
  float k4_du = geodesic_f(u4, M);
  float k4_u = du4;

  OrbitState out;
  out.u = s.u + (h / 6.0f) * (k1_u + 2.0f * k2_u + 2.0f * k3_u + k4_u);
  out.du = s.du + (h / 6.0f) * (k1_du + 2.0f * k2_du + 2.0f * k3_du + k4_du);
  return out;
}

/* ------------------------------------------------------------------ */
/* Per-pixel trace: Embedding mode                                    */
/* ------------------------------------------------------------------ */
static vec3 trace_embedding(vec3 ro, vec3 rd, float qx, float qy) {
  vec3 col = make_vec3(0.02f, 0.02f, 0.05f);
  float t = 0.0f;
  int hit = 0;
  vec3 hitPos = make_vec3(0.0f, 0.0f, 0.0f);

  for (int i = 0; i < 80; i++) {
    vec3 p = vadd(ro, vscale(rd, t));
    float r = sqrtf(p.x * p.x + p.y * p.y);
    float h = W_embed(r);
    float d = p.z - h;
    if (fabsf(d) < 0.02f) {
      hit = 1;
      hitPos = p;
      break;
    }
    t += fmaxf(0.03f, fabsf(d) * 0.6f);
    if (t > 40.0f)
      break;
  }

  if (hit) {
    float r = sqrtf(hitPos.x * hitPos.x + hitPos.y * hitPos.y);
    float eps = 0.01f;
    float hx = W_embed(sqrtf((hitPos.x + eps) * (hitPos.x + eps) +
                             hitPos.y * hitPos.y)) -
               W_embed(r);
    float hy = W_embed(sqrtf(hitPos.x * hitPos.x +
                             (hitPos.y + eps) * (hitPos.y + eps))) -
               W_embed(r);
    vec3 hitN = vnormalize(make_vec3(-hx / eps, -hy / eps, 1.0f));

    vec3 L = vnormalize(make_vec3(0.4f, 0.8f, 0.3f));
    float diff = fmaxf(vdot(hitN, L), 0.12f);
    vec3 base = vmix(make_vec3(0.1f, 0.35f, 0.9f), make_vec3(0.3f, 0.7f, 1.0f),
                     smoothstep(0.0f, 5.0f, r));
    col = vscale(base, diff);
    float edge = fabsf(hitPos.z - W_embed(r));
    col = vscale(col, 0.5f + 0.5f * smoothstep(0.0f, 0.15f, edge));
  } else {
    col = stars(rd);
  }

  float vig =
      0.65f + 0.35f * powf(16.0f * qx * qy * (1.0f - qx) * (1.0f - qy), 0.35f);
  col = vscale(col, vig);
  col = vdiv(col, vadd(make_vec3(1.0f, 1.0f, 1.0f), col));
  col = make_vec3(powf(col.x, 0.85f), powf(col.y, 0.85f), powf(col.z, 0.85f));
  return col;
}

/* ------------------------------------------------------------------ */
/* Per-pixel trace: Raytrace mode (full RK4 geodesics)                */
/* ------------------------------------------------------------------ */
static vec3 trace_raytrace(vec3 ro, vec3 rd, float qx, float qy, float M,
                           float rs, float bcrit, float ci, float si) {
  /* FIX 2: base ortonormale con e1 allineato a ro */
  vec3 n = vnormalize(vcross(ro, rd));
  vec3 e1 = vnormalize(ro);
  vec3 e2 = vnormalize(vcross(n, e1));

  float b = vlength(vcross(ro, rd));
  float r0 = vlength(ro);
  float u0 = 1.0f / r0;
  vec3 ro_n = vscale(ro, 1.0f / r0);
  float ur = -vdot(rd, ro_n);

  /* FIX 3: du0 = ur / b (senza il fattore r0 errato) */
  float du0 = ur / fmaxf(b, 1e-5f);

  int captured = (b < bcrit * 0.995f) ? 1 : 0;
  vec3 col = make_vec3(0.0f, 0.0f, 0.0f);
  vec3 diskAcc = make_vec3(0.0f, 0.0f, 0.0f);
  float diskA = 0.0f;
  int crossedDisk = 0;

  if (!captured) {
    OrbitState s;
    s.u = u0;
    s.du = du0;

    float phi = 0.0f;
    float hstep = 0.035f;
    const int MAX_STEPS = 220;

    for (int i = 0; i < MAX_STEPS; i++) {
      float adapt = clampf(0.6f / fmaxf(s.u, 0.05f), 0.4f, 2.2f);
      float step = hstep * adapt;

      s = rk4_step(s, step, M);
      phi += step;

      if (s.u <= 1e-5f)
        break;                          /* escaped to infinity */
      if (s.u > 1.0f / (rs * 1.005f)) { /* captured by horizon */
        captured = 1;
        break;
      }

      float r = 1.0f / s.u;
      vec3 pos = vscale(vadd(vscale(e1, cosf(phi)), vscale(e2, sinf(phi))), r);

      /* Disk intersection */
      float py = pos.y * ci - pos.z * si;
      if (fabsf(py) < 0.055f && r > 2.9f * M && r < 20.0f * M) {
        crossedDisk = 1;
        float dens = expf(-powf((r - 8.5f * M) / (7.5f * M), 2.0f));
        vec3 dc = diskColor(r, M);
        float alpha = dens * 0.68f;

        diskAcc = vadd(diskAcc, vscale(dc, (1.0f - diskA) * alpha));
        diskA += (1.0f - diskA) * alpha;
        if (diskA > 0.94f)
          break;
      }
    }

    if (captured) {
      /* FIX 4: if we crossed the disk before falling in, disk is in front */
      col = crossedDisk ? diskAcc : make_vec3(0.0f, 0.0f, 0.0f);
    } else {
      /* FIX 5: final direction from orbit derivative */
      vec3 radial_dir = vadd(vscale(e1, cosf(phi)), vscale(e2, sinf(phi)));
      vec3 tangent_dir = vadd(vscale(e1, -sinf(phi)), vscale(e2, cosf(phi)));
      vec3 finalDir =
          vnormalize(vadd(vscale(radial_dir, -s.du / s.u), tangent_dir));
      col = stars(finalDir);
      col = vadd(vscale(col, 1.0f - diskA), diskAcc);
    }
  }

  /* Tone mapping & vignette (raytrace) */
  float vig =
      0.58f + 0.42f * powf(16.0f * qx * qy * (1.0f - qx) * (1.0f - qy), 0.38f);
  col = vscale(col, vig);
  col = vdiv(col, vadd(make_vec3(1.0f, 1.0f, 1.0f), vscale(col, 0.95f)));
  col = make_vec3(powf(col.x, 0.78f), powf(col.y, 0.78f), powf(col.z, 0.78f));
  return col;
}

/* ------------------------------------------------------------------ */
/* Main render loop                                                   */
/* ------------------------------------------------------------------ */
int main(void) {
  struct os1_gl_surface_params params = {.x = 50,
                                         .y = 50,
                                         .width = WIDTH,
                                         .height = HEIGHT,
                                         .title = "Black Hole OS1"};
  struct os1_gl_surface *surf = NULL;
  if (os1_gl_surface_create(&params, &surf) != 0) {
    printf("BlackHole: failed to create GL surface\n");
    return 1;
  }

  computeC2Coefficients();

  while (1) {
    /* ---- Input ---- */
    input_event_t ev;
    while (input_poll_event(&ev)) {
      if (ev.type == INPUT_TYPE_RESIZE) {
        os1_gl_surface_resize(surf, ev.resize.w, ev.resize.h);
      }
      if (ev.type == INPUT_TYPE_KEYBOARD && ev.keyboard.state == KEY_PRESSED) {
        unsigned char k = ev.keyboard.key;
        int sc = ev.keyboard.scancode;
        int rebuild = 0;

        if (k == '1')
          mode = MODE_RAYTRACE;
        else if (k == '2')
          mode = MODE_EMBEDDING;
        else if (k == 'a')
          mass = fminf(3.0f, mass + 0.05f);
        else if (k == 'z')
          mass = fmaxf(0.3f, mass - 0.05f);
        else if (k == 'd')
          diskIncl += 0.05f;
        else if (k == 'c')
          diskIncl -= 0.05f;
        else if (k == 'q') {
          r0_emb += 0.08f;
          rebuild = 1;
        } else if (k == 'w') {
          r0_emb = fmaxf(0.5f, r0_emb - 0.08f);
          rebuild = 1;
        } else if (k == 's') {
          a4_emb += 0.02f;
          rebuild = 1;
        } else if (k == 'x') {
          a4_emb -= 0.02f;
          rebuild = 1;
        } else if (k == 'r') {
          camYaw = 0.0f;
          camPitch = 0.28f;
          camDist = 13.5f;
          mass = 1.0f;
          diskIncl = 1.18f;
          r0_emb = 1.85f;
          a0_emb = -2.9f;
          a4_emb = 0.12f;
          rebuild = 1;
        } else if (sc == INPUT_KEY_LEFT)
          camYaw -= 0.09f;
        else if (sc == INPUT_KEY_RIGHT)
          camYaw += 0.09f;
        else if (sc == INPUT_KEY_UP)
          camPitch -= 0.07f;
        else if (sc == INPUT_KEY_DOWN)
          camPitch += 0.07f;
        else if (k == '=' || k == '+')
          camDist = fmaxf(6.0f, camDist - 0.5f);
        else if (k == '-')
          camDist = fminf(40.0f, camDist + 0.5f);

        if (rebuild)
          computeC2Coefficients();
      }
      if (ev.type == INPUT_TYPE_MOUSE) {
        int btn = ev.mouse.button;
        int st = ev.mouse.state;
        int mx = ev.mouse.x;
        int my = ev.mouse.y;

        if (btn == MOUSE_BTN_LEFT) {
          if (st == KEY_PRESSED) {
            mouseDown = 1;
            lastMX = mx;
            lastMY = my;
          } else if (st == KEY_RELEASED) {
            mouseDown = 0;
          }
        }
        if (mouseDown) {
          camYaw += (mx - lastMX) * 0.008f;
          camPitch += (my - lastMY) * 0.008f;
          camPitch = clampf(camPitch, -1.4f, 1.4f);
          lastMX = mx;
          lastMY = my;
        }
      }
    }

    /* ---- Render ---- */
    int w, h;
    size_t pitch_bytes;
    uint32_t *pixels = os1_gl_surface_buffer(surf, &w, &h, &pitch_bytes);
    int pitch_px = (int)(pitch_bytes / sizeof(uint32_t));

    /* Per-frame constants */
    vec3 ro = make_vec3(0.0f, 0.0f, -camDist);
    ro = rotateX(ro, camPitch);
    ro = rotateY(ro, camYaw);
    vec3 ww = vnormalize(vscale(ro, -1.0f));
    vec3 uu = vnormalize(vcross(ww, make_vec3(0.0f, 1.0f, 0.0f)));
    vec3 vv = vcross(uu, ww);
    float M = fmaxf(mass, 0.25f);
    float rs = 2.0f * M;
    float bcrit = 3.0f * sqrtf(3.0f) * M;
    float ci = cosf(diskIncl), si = sinf(diskIncl);
    float aspect = (float)w / (float)h;

    for (int y = 0; y < h; y++) {
      float qy = 1.0f - y / (float)h; /* 1 = top, 0 = bottom (shader UV) */
      float v = (y / (float)h) * 2.0f - 1.0f;
      for (int x = 0; x < w; x++) {
        float qx = x / (float)w;
        float u = (x / (float)w) * 2.0f - 1.0f;
        u *= aspect;

        vec3 rd = vnormalize(
            vadd(vadd(vscale(uu, u), vscale(vv, v)), vscale(ww, 1.7f)));

        vec3 col = (mode == MODE_RAYTRACE)
                       ? trace_raytrace(ro, rd, qx, qy, M, rs, bcrit, ci, si)
                       : trace_embedding(ro, rd, qx, qy);

        int r = (int)(clampf(col.x, 0.0f, 1.0f) * 255.0f + 0.5f);
        int g = (int)(clampf(col.y, 0.0f, 1.0f) * 255.0f + 0.5f);
        int b = (int)(clampf(col.z, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixels[y * pitch_px + x] = 0xFF000000u | ((uint32_t)r << 16) |
                                   ((uint32_t)g << 8) | (uint32_t)b;
      }
    }

    os1_gl_surface_swap(surf);
    OS1_sleep(16); /* ~60 FPS pacing via real kernel timer */
  }

  os1_gl_surface_destroy(surf);
  return 0;
}