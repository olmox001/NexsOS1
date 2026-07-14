#include <image.h>
#include <input.h>
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD 18
#define TOOLBAR_H 44
#define BTN_SIZE 30
#define BTN_GAP 10
#define MIN_W 280
#define MIN_H 200
#define TERM_W 80
#define TERM_H 40
#define ZOOM_MIN 50
#define ZOOM_MAX 8000

static uint32_t *g_fb;
static int g_ww;
static int g_wh;
static os1_image_t *g_scaled;
static int g_scaled_zoom;
static int g_zoom = 1000;
static int g_screen_w = 800;
static int g_screen_h = 600;

static int streq(const char *a, const char *b) {
  return a && b && strncmp(a, b, 64) == 0;
}

static void update_screen_size(void) {
  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  g_screen_w = sw > 0 ? sw : 800;
  g_screen_h = sh > 0 ? sh : 600;
}

static void clear_scaled(void) {
  os1_image_free(g_scaled);
  g_scaled = NULL;
  g_scaled_zoom = 0;
}

static int scaled_size(const os1_image_t *img, int *out_w, int *out_h) {
  if (!img || !out_w || !out_h)
    return -1;
  int64_t tw = ((int64_t)img->w * g_zoom + 500) / 1000;
  int64_t th = ((int64_t)img->h * g_zoom + 500) / 1000;
  if (tw < 1)
    tw = 1;
  if (th < 1)
    th = 1;
  if (tw > OS1_IMAGE_MAX_DIMENSION)
    tw = OS1_IMAGE_MAX_DIMENSION;
  if (th > OS1_IMAGE_MAX_DIMENSION)
    th = OS1_IMAGE_MAX_DIMENSION;
  if ((uint64_t)tw * (uint64_t)th > OS1_IMAGE_MAX_PIXELS)
    return -1;
  *out_w = (int)tw;
  *out_h = (int)th;
  return 0;
}

static int desired_window_size(const os1_image_t *img, int *ww, int *wh) {
  int iw = 0;
  int ih = 0;
  if (scaled_size(img, &iw, &ih) < 0)
    return -1;

  int max_w = (g_screen_w * 9) / 10;
  int max_h = (g_screen_h * 8) / 10;
  *ww = iw + PAD * 2;
  *wh = ih + PAD * 2 + TOOLBAR_H;
  if (*ww < MIN_W)
    *ww = MIN_W;
  if (*wh < MIN_H)
    *wh = MIN_H;
  if (*ww > max_w)
    *ww = max_w;
  if (*wh > max_h)
    *wh = max_h;
  return 0;
}

static int resize_fb(int ww, int wh) {
  if (ww <= 0 || wh <= 0)
    return -1;
  if ((uint64_t)ww * (uint64_t)wh > OS1_IMAGE_MAX_PIXELS)
    return -1;
  uint32_t *next = (uint32_t *)malloc((size_t)ww * (size_t)wh * 4u);
  if (!next)
    return -1;
  free(g_fb);
  g_fb = next;
  g_ww = ww;
  g_wh = wh;
  return 0;
}

static int ensure_scaled(const os1_image_t *img) {
  int sw = 0;
  int sh = 0;
  if (scaled_size(img, &sw, &sh) < 0)
    return -1;
  if (g_scaled && g_scaled_zoom == g_zoom && g_scaled->w == sw &&
      g_scaled->h == sh)
    return 0;

  os1_image_t *next = os1_image_resample(img, sw, sh);
  if (!next)
    return -1;
  clear_scaled();
  g_scaled = next;
  g_scaled_zoom = g_zoom;
  return 0;
}

static void fb_rect(int x, int y, int w, int h, uint32_t color) {
  if (!g_fb || w <= 0 || h <= 0)
    return;
  int x1 = x < 0 ? 0 : x;
  int y1 = y < 0 ? 0 : y;
  int x2 = x + w;
  int y2 = y + h;
  if (x2 > g_ww)
    x2 = g_ww;
  if (y2 > g_wh)
    y2 = g_wh;
  for (int py = y1; py < y2; py++)
    for (int px = x1; px < x2; px++)
      g_fb[py * g_ww + px] = color;
}

static void fb_line_h(int x, int y, int w, uint32_t color) {
  fb_rect(x, y, w, 2, color);
}

static void fb_line_v(int x, int y, int h, uint32_t color) {
  fb_rect(x, y, 2, h, color);
}

static void button_rects(int *minus_x, int *reset_x, int *plus_x, int *by) {
  int total = BTN_SIZE * 3 + BTN_GAP * 2;
  int x = (g_ww - total) / 2;
  if (x < PAD)
    x = PAD;
  *minus_x = x;
  *reset_x = x + BTN_SIZE + BTN_GAP;
  *plus_x = x + (BTN_SIZE + BTN_GAP) * 2;
  *by = (TOOLBAR_H - BTN_SIZE) / 2;
}

static void draw_button(int x, int y, int kind) {
  fb_rect(x, y, BTN_SIZE, BTN_SIZE, 0xEEFFFFFFu);
  fb_rect(x, y, BTN_SIZE, 1, 0xFF8A8F98u);
  fb_rect(x, y + BTN_SIZE - 1, BTN_SIZE, 1, 0xFF8A8F98u);
  fb_rect(x, y, 1, BTN_SIZE, 0xFF8A8F98u);
  fb_rect(x + BTN_SIZE - 1, y, 1, BTN_SIZE, 0xFF8A8F98u);

  int cx = x + BTN_SIZE / 2;
  int cy = y + BTN_SIZE / 2;
  fb_line_h(cx - 7, cy, 14, 0xFF111827u);
  if (kind == 1)
    fb_line_v(cx, cy - 7, 14, 0xFF111827u);
  else if (kind == 2) {
    fb_rect(cx - 6, cy - 6, 12, 2, 0xFF111827u);
    fb_rect(cx - 6, cy + 4, 12, 2, 0xFF111827u);
    fb_rect(cx - 6, cy - 6, 2, 12, 0xFF111827u);
    fb_rect(cx + 4, cy - 6, 2, 12, 0xFF111827u);
  }
}

static void draw_view(const os1_image_t *img) {
  if (!g_fb || !img)
    return;

  for (int i = 0; i < g_ww * g_wh; i++)
    g_fb[i] = 0x00000000u;

  fb_rect(0, 0, g_ww, TOOLBAR_H, 0xEAF8F9FBu);
  fb_rect(0, TOOLBAR_H - 1, g_ww, 1, 0xFFB8BEC8u);

  int minus_x, reset_x, plus_x, by;
  button_rects(&minus_x, &reset_x, &plus_x, &by);
  draw_button(minus_x, by, 0);
  draw_button(reset_x, by, 2);
  draw_button(plus_x, by, 1);

  if (ensure_scaled(img) < 0 || !g_scaled)
    return;

  int content_h = g_wh - TOOLBAR_H;
  int ox = (g_ww - g_scaled->w) / 2;
  int oy = TOOLBAR_H + (content_h - g_scaled->h) / 2;
  if (oy < TOOLBAR_H + PAD)
    oy = TOOLBAR_H + PAD;

  for (int y = 0; y < g_scaled->h; y++) {
    int dy = oy + y;
    if (dy < TOOLBAR_H || dy >= g_wh)
      continue;
    for (int x = 0; x < g_scaled->w; x++) {
      int dx = ox + x;
      if (dx < 0 || dx >= g_ww)
        continue;
      g_fb[dy * g_ww + dx] = g_scaled->pixels[y * g_scaled->w + x];
    }
  }
}

static int apply_zoom_window(int win, const os1_image_t *img) {
  int ww = 0;
  int wh = 0;
  if (desired_window_size(img, &ww, &wh) < 0)
    return -1;
  OS1_window_resize(win, ww, wh);
  resize_fb(ww, wh);
  return 0;
}

static int handle_toolbar_click(int win, const os1_image_t *img, int mx,
                                int my) {
  int minus_x, reset_x, plus_x, by;
  button_rects(&minus_x, &reset_x, &plus_x, &by);
  if (my < by || my >= by + BTN_SIZE)
    return 0;

  int next_zoom = g_zoom;
  if (mx >= minus_x && mx < minus_x + BTN_SIZE)
    next_zoom = (g_zoom * 4) / 5;
  else if (mx >= reset_x && mx < reset_x + BTN_SIZE)
    next_zoom = 1000;
  else if (mx >= plus_x && mx < plus_x + BTN_SIZE)
    next_zoom = (g_zoom * 5) / 4;
  else
    return 0;

  if (next_zoom < ZOOM_MIN)
    next_zoom = ZOOM_MIN;
  if (next_zoom > ZOOM_MAX)
    next_zoom = ZOOM_MAX;
  if (next_zoom != g_zoom) {
    g_zoom = next_zoom;
    clear_scaled();
    apply_zoom_window(win, img);
  }
  return 1;
}

static int run_window(const char *path, os1_image_t *img) {
  update_screen_size();

  int fit_w = 0;
  int fit_h = 0;
  int max_w = (g_screen_w * 9) / 10 - PAD * 2;
  int max_h = (g_screen_h * 8) / 10 - PAD * 2 - TOOLBAR_H;
  if (os1_image_fit_size(img->w, img->h, max_w, max_h, 0, &fit_w, &fit_h) ==
      OS1_IMAGE_OK) {
    int zw = (fit_w * 1000) / img->w;
    int zh = (fit_h * 1000) / img->h;
    g_zoom = zw < zh ? zw : zh;
    if (g_zoom < ZOOM_MIN)
      g_zoom = ZOOM_MIN;
    if (g_zoom > 1000)
      g_zoom = 1000;
  }

  int ww = 0, wh = 0;
  desired_window_size(img, &ww, &wh);
  int wx = (g_screen_w - ww) / 2;
  int wy = (g_screen_h - wh) / 2;

  if (resize_fb(ww, wh) < 0)
    return 1;

  char title[64];
  snprintf(title, sizeof(title), "nximage: %s", path);
  int win = create_window(wx, wy, ww, wh, title);
  if (win < 0)
    return 1;

  for (;;) {
    draw_view(img);
    window_blit(win, 0, 0, g_ww, g_wh, g_fb);
    compositor_render();

    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_RESIZE && ev.resize.w > 0 && ev.resize.h > 0) {
        resize_fb(ev.resize.w, ev.resize.h);
      } else if (ev.type == INPUT_TYPE_MOUSE &&
                 ev.mouse.button == MOUSE_BTN_LEFT &&
                 ev.mouse.state == KEY_PRESSED) {
        handle_toolbar_click(win, img, ev.mouse.x, ev.mouse.y);
      } else if (ev.type == INPUT_TYPE_KEYBOARD &&
                 ev.keyboard.state == KEY_PRESSED) {
        if (ev.keyboard.key == 'q' || ev.keyboard.key == 'Q' ||
            ev.keyboard.scancode == INPUT_KEY_ESC) {
          destroy_window(win);
          return 0;
        }
      }
    }
    OS1_sleep(50);
  }
}

static int run_terminal(const char *path, os1_image_t *img) {
  os1_image_t *small = os1_image_resample_fit(img, TERM_W, TERM_H, 0);
  if (!small)
    return 1;

  const char *ramp = " .:-=+*#%@";
  int ramp_n = 10;
  printf("nximage: %s (%dx%d)\n", path, img->w, img->h);
  for (int y = 0; y < small->h; y++) {
    for (int x = 0; x < small->w; x++) {
      uint32_t p = small->pixels[y * small->w + x];
      int a = (int)((p >> 24) & 0xFFu);
      int r = (int)((p >> 16) & 0xFFu);
      int g = (int)((p >> 8) & 0xFFu);
      int b = (int)(p & 0xFFu);
      int lum = (r * 30 + g * 59 + b * 11) / 100;
      lum = (lum * a + 255 * (255 - a)) / 255;
      int idx = (lum * (ramp_n - 1)) / 255;
      putchar(ramp[idx]);
      putchar(ramp[idx]);
    }
    putchar('\n');
  }
  os1_image_free(small);
  return 0;
}

int main(int argc, char **argv) {
  int term = 0;
  const char *path = "/home/Pictures//globe.png";

  if (argc >= 2 && streq(argv[1], "--term")) {
    term = 1;
    if (argc >= 3)
      path = argv[2];
  } else if (argc >= 2) {
    path = argv[1];
  }

  os1_image_t *img = os1_image_load(path);
  if (!img) {
    printf("nximage: unable to load '%s'\n", path);
    printf("usage: nximage [--term] [file.png]\n");
    return 1;
  }

  printf("nximage: decoded %s (%dx%d, sanitized ARGB)\n", path, img->w, img->h);
  int r = term ? run_terminal(path, img) : run_window(path, img);
  clear_scaled();
  os1_image_free(img);
  free(g_fb);
  return r;
}
