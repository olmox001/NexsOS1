#include <font_lib.h>
#include <image.h>
#include <input.h>
#include <os1.h>
#include <os1vid.h>
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

#define VBAR_MARGIN 10
#define VBAR_TIME_W 92
#define VBAR_H 8

#define HIDE_BTN_SIZE 18
#define HIDE_LINE_H 3

static int g_bar_hidden = 0;

static uint32_t *g_fb;
static int g_ww;
static int g_wh;
static os1_image_t *g_scaled;
static int g_scaled_zoom;
static int g_zoom = 1000;
static int g_screen_w = 800;
static int g_screen_h = 600;

static struct font_ctx *g_font;

static void buf_draw_glyph(int x, int y, uint32_t codepoint, uint32_t color) {
  if (!g_font)
    return;
  int idx = (int)codepoint - g_font->header.first_char;
  if (idx < 0 || idx >= g_font->header.num_chars)
    return;
  struct font_glyph_info *gi = &g_font->glyphs[idx];
  uint8_t *bitmap = g_font->bitmap + gi->data_offset;
  int start_x = x + gi->x0;
  int start_y = y + g_font->header.ascent + gi->y0;
  for (int gy = 0; gy < gi->height; gy++) {
    for (int gx = 0; gx < gi->width; gx++) {
      uint8_t alpha = bitmap[gy * gi->width + gx];
      if (alpha > 64) {
        int px = start_x + gx, py = start_y + gy;
        if (px >= 0 && px < g_ww && py >= 0 && py < g_wh)
          g_fb[py * g_ww + px] = color;
      }
    }
  }
}

static int buf_text_width(const char *s) {
  if (!g_font || !s)
    return 0;
  return font_string_width(g_font, s);
}

static void buf_draw_text(int x, int y, const char *s, uint32_t color) {
  if (!g_font || !s)
    return;
  uint32_t cp;
  int consumed, cursor = x;
  size_t rem = strlen(s);
  const char *p = s;
  while (*p) {
    consumed = utf8_decode(p, rem, &cp);
    if (consumed <= 0) {
      p++;
      rem--;
      continue;
    }
    buf_draw_glyph(cursor, y, cp, color);
    int idx = (int)cp - g_font->header.first_char;
    if (idx >= 0 && idx < g_font->header.num_chars)
      cursor += g_font->glyphs[idx].advance;
    p += consumed;
    rem -= consumed;
  }
}

static int streq(const char *a, const char *b) {
  return a && b && strncmp(a, b, 64) == 0;
}

static int top_bar_h(void) { return g_bar_hidden ? HIDE_LINE_H : TOOLBAR_H; }

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

static int render_size(const os1_image_t *img, int *out_w, int *out_h) {
  if (scaled_size(img, out_w, out_h) < 0)
    return -1;

  if (g_ww > 0 && g_wh > 0) {
    int content_w = g_ww - PAD * 2;
    int content_h = g_wh - PAD * 2 - top_bar_h();
    if (content_w < 1)
      content_w = 1;
    if (content_h < 1)
      content_h = 1;
    if (*out_w > content_w)
      *out_w = content_w;
    if (*out_h > content_h)
      *out_h = content_h;
  }
  if (*out_w < 1)
    *out_w = 1;
  if (*out_h < 1)
    *out_h = 1;
  return 0;
}

static int desired_window_size(const os1_image_t *img, int *ww, int *wh) {
  int iw = 0, ih = 0;
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
  int sw = 0, sh = 0;
  if (render_size(img, &sw, &sh) < 0)
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

static void hide_btn_rect(int *hx, int *hy) {
  *hx = g_ww - PAD - HIDE_BTN_SIZE;
  *hy = (TOOLBAR_H - HIDE_BTN_SIZE) / 2;
}

static void draw_hide_button(int x, int y) {
  fb_rect(x, y, HIDE_BTN_SIZE, HIDE_BTN_SIZE, 0xEEFFFFFFu);
  fb_rect(x, y, HIDE_BTN_SIZE, 1, 0xFF8A8F98u);
  fb_rect(x, y + HIDE_BTN_SIZE - 1, HIDE_BTN_SIZE, 1, 0xFF8A8F98u);
  fb_rect(x, y, 1, HIDE_BTN_SIZE, 0xFF8A8F98u);
  fb_rect(x + HIDE_BTN_SIZE - 1, y, 1, HIDE_BTN_SIZE, 0xFF8A8F98u);

  int cx = x + HIDE_BTN_SIZE / 2;
  int cy = y + HIDE_BTN_SIZE / 2;
  for (int i = 0; i < 5; i++) {
    fb_rect(cx - 5 + i, cy + 2 - i, 2, 2, 0xFF111827u);
    fb_rect(cx + 3 - i, cy + 2 - i, 2, 2, 0xFF111827u);
  }
}

static void draw_button(int x, int y, int kind) {
  fb_rect(x, y, BTN_SIZE, BTN_SIZE, 0xEEFFFFFFu);
  fb_rect(x, y, BTN_SIZE, 1, 0xFF8A8F98u);
  fb_rect(x, y + BTN_SIZE - 1, BTN_SIZE, 1, 0xFF8A8F98u);
  fb_rect(x, y, 1, BTN_SIZE, 0xFF8A8F98u);
  fb_rect(x + BTN_SIZE - 1, y, 1, BTN_SIZE, 0xFF8A8F98u);

  int cx = x + BTN_SIZE / 2;
  int cy = y + BTN_SIZE / 2;
  if (kind == 0) {
    fb_line_h(cx - 7, cy, 14, 0xFF111827u);
  } else if (kind == 1) {
    fb_line_h(cx - 7, cy, 14, 0xFF111827u);
    fb_line_v(cx, cy - 7, 14, 0xFF111827u);
  } else if (kind == 2) {
    fb_rect(cx - 6, cy - 6, 12, 2, 0xFF111827u);
    fb_rect(cx - 6, cy + 4, 12, 2, 0xFF111827u);
    fb_rect(cx - 6, cy - 6, 2, 12, 0xFF111827u);
    fb_rect(cx + 4, cy - 6, 2, 12, 0xFF111827u);
  } else if (kind == 3) {
    for (int i = 0; i < 10; i++)
      fb_line_v(cx - 5 + i, cy - 9 + i, 18 - i * 2, 0xFF111827u);
  } else if (kind == 4) {
    fb_rect(cx - 6, cy - 8, 4, 16, 0xFF111827u);
    fb_rect(cx + 2, cy - 8, 4, 16, 0xFF111827u);
  }
}

static void draw_view(const os1_image_t *img) {
  if (!g_fb || !img)
    return;

  for (int i = 0; i < g_ww * g_wh; i++)
    g_fb[i] = 0x00000000u;

  if (!g_bar_hidden) {
    fb_rect(0, 0, g_ww, TOOLBAR_H, 0xEAF8F9FBu);
    fb_rect(0, TOOLBAR_H - 1, g_ww, 1, 0xFFB8BEC8u);

    int minus_x, reset_x, plus_x, by;
    button_rects(&minus_x, &reset_x, &plus_x, &by);
    draw_button(minus_x, by, 0);
    draw_button(reset_x, by, 2);
    draw_button(plus_x, by, 1);

    int hx, hy;
    hide_btn_rect(&hx, &hy);
    draw_hide_button(hx, hy);
  } else {
    fb_rect(0, 0, g_ww, HIDE_LINE_H, 0xFFFFFFFFu);
  }

  if (ensure_scaled(img) < 0 || !g_scaled)
    return;

  int bar_h = top_bar_h();
  int content_h = g_wh - bar_h;
  int ox = (g_ww - g_scaled->w) / 2;
  int oy = bar_h + (content_h - g_scaled->h) / 2;
  if (oy < bar_h + PAD)
    oy = bar_h + PAD;
  if (ox < PAD)
    ox = PAD;

  for (int y = 0; y < g_scaled->h; y++) {
    int dy = oy + y;
    if (dy < bar_h || dy >= g_wh)
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
  int ww = 0, wh = 0;
  if (desired_window_size(img, &ww, &wh) < 0)
    return -1;
  OS1_window_resize(win, ww, wh);
  resize_fb(ww, wh);
  return 0;
}

static int handle_toolbar_click(int win, const os1_image_t *img, int mx,
                                int my) {
  if (g_bar_hidden) {
    if (my < HIDE_LINE_H + 6) {
      g_bar_hidden = 0;
      clear_scaled();
      return 1;
    }
    return 0;
  }

  int hx, hy;
  hide_btn_rect(&hx, &hy);
  if (mx >= hx && mx < hx + HIDE_BTN_SIZE && my >= hy &&
      my < hy + HIDE_BTN_SIZE) {
    g_bar_hidden = 1;
    clear_scaled();
    return 1;
  }

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

  int fit_w = 0, fit_h = 0;
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

static void video_bar_rects(int *play_x, int *play_y, int *bar_x, int *bar_y,
                            int *bar_w, int *time_x) {
  *play_y = (TOOLBAR_H - BTN_SIZE) / 2;
  *play_x = PAD;
  *bar_x = *play_x + BTN_SIZE + VBAR_MARGIN;
  *bar_y = (TOOLBAR_H - VBAR_H) / 2;
  *time_x = g_ww - PAD - HIDE_BTN_SIZE - VBAR_MARGIN - VBAR_TIME_W;
  *bar_w = *time_x - VBAR_MARGIN - *bar_x;
  if (*bar_w < 10)
    *bar_w = 10;
}

static void format_time(double seconds, char *out, size_t out_size) {
  if (seconds < 0)
    seconds = 0;
  int total = (int)(seconds + 0.5);
  int m = total / 60;
  int s = total % 60;
  snprintf(out, out_size, "%02d:%02d", m, s);
}

static void draw_video_toolbar(const os1vid_t *v) {
  if (g_bar_hidden) {
    fb_rect(0, 0, g_ww, HIDE_LINE_H, 0xFFFFFFFFu);
    return;
  }

  fb_rect(0, 0, g_ww, TOOLBAR_H, 0xEAF8F9FBu);
  fb_rect(0, TOOLBAR_H - 1, g_ww, 1, 0xFFB8BEC8u);

  int play_x, play_y, bar_x, bar_y, bar_w, time_x;
  video_bar_rects(&play_x, &play_y, &bar_x, &bar_y, &bar_w, &time_x);

  draw_button(play_x, play_y, os1vid_is_paused(v) ? 3 : 4);

  int hx, hy;
  hide_btn_rect(&hx, &hy);
  draw_hide_button(hx, hy);

  fb_rect(bar_x, bar_y, bar_w, VBAR_H, 0xFFD8DCE3u);
  double dur = os1vid_get_duration(v);
  double cur = os1vid_get_time(v);
  int fill_w = 0;
  if (dur > 0.0) {
    fill_w = (int)((cur / dur) * bar_w + 0.5);
    if (fill_w < 0)
      fill_w = 0;
    if (fill_w > bar_w)
      fill_w = bar_w;
  }
  if (fill_w > 0)
    fb_rect(bar_x, bar_y, fill_w, VBAR_H, 0xFF3B82F6u);

  int handle_x = bar_x + fill_w - 2;
  if (handle_x < bar_x)
    handle_x = bar_x;
  fb_rect(handle_x, bar_y - 3, 4, VBAR_H + 6, 0xFF1D4ED8u);

  if (g_font) {
    char cur_s[16], dur_s[16], label[40];
    format_time(cur, cur_s, sizeof(cur_s));
    format_time(dur, dur_s, sizeof(dur_s));
    snprintf(label, sizeof(label), "%s / %s", cur_s, dur_s);
    int lw = buf_text_width(label);
    int lx = time_x + (VBAR_TIME_W - lw) / 2;
    if (lx < time_x)
      lx = time_x;
    int ly = (TOOLBAR_H - 16) / 2;
    buf_draw_text(lx, ly, label, 0xFF111827u);
  }
}

static int handle_video_toolbar_click(os1vid_t *v, int mx, int my) {
  if (g_bar_hidden) {
    if (my < HIDE_LINE_H + 6) {
      g_bar_hidden = 0;
      return 2;
    }
    return 0;
  }

  int hx, hy;
  hide_btn_rect(&hx, &hy);
  if (mx >= hx && mx < hx + HIDE_BTN_SIZE && my >= hy &&
      my < hy + HIDE_BTN_SIZE) {
    g_bar_hidden = 1;
    return 2;
  }

  int play_x, play_y, bar_x, bar_y, bar_w, time_x;
  video_bar_rects(&play_x, &play_y, &bar_x, &bar_y, &bar_w, &time_x);

  if (my >= play_y && my < play_y + BTN_SIZE && mx >= play_x &&
      mx < play_x + BTN_SIZE) {
    if (os1vid_is_paused(v))
      os1vid_resume(v);
    else
      os1vid_pause(v);
    return 1;
  }

  int bar_hit_y0 = bar_y - 6;
  int bar_hit_y1 = bar_y + VBAR_H + 6;
  if (my >= bar_hit_y0 && my < bar_hit_y1 && mx >= bar_x &&
      mx < bar_x + bar_w) {
    double dur = os1vid_get_duration(v);
    if (dur > 0.0) {
      double frac = (double)(mx - bar_x) / (double)bar_w;
      if (frac < 0.0)
        frac = 0.0;
      if (frac > 1.0)
        frac = 1.0;
      os1vid_seek(v, frac * dur);
    }
    return 1;
  }

  return 0;
}

static void fit_video_content(const os1vid_t *v, int ww, int wh, int *out_vw,
                              int *out_vh) {
  if (ww <= 0 || wh <= 0) {
    *out_vw = 1;
    *out_vh = 1;
    return;
  }

  int avail_w = ww - PAD * 2;
  int avail_h = wh - top_bar_h() - PAD * 2;
  if (avail_w < 1)
    avail_w = 1;
  if (avail_h < 1)
    avail_h = 1;

  int vw = v->width;
  int vh = v->height;
  if (vw > avail_w || vh > avail_h) {
    int fit_w = 0, fit_h = 0;
    if (os1_image_fit_size(v->width, v->height, avail_w, avail_h, 0, &fit_w,
                           &fit_h) == OS1_IMAGE_OK) {
      vw = fit_w;
      vh = fit_h;
    }
  }
  if (vw > avail_w)
    vw = avail_w;
  if (vh > avail_h)
    vh = avail_h;
  if (vw < 1)
    vw = 1;
  if (vh < 1)
    vh = 1;
  *out_vw = vw;
  *out_vh = vh;
}

static int run_video(const char *path) {
  os1vid_t *v = os1vid_open(path);
  if (!v) {
    printf("nximage: unable to decode '%s' as MPEG1 (.mpg/.mpeg)\n", path);
    printf("Convert other formats first, e.g.:\n");
    printf("  ffmpeg -i input.mp4 -c:v mpeg1video -c:a mp2 -format mpeg "
           "output.mpg\n");
    return 1;
  }

  update_screen_size();
  int max_w = (g_screen_w * 9) / 10;
  int max_h = (g_screen_h * 8) / 10;

  int vw = 0, vh = 0;
  fit_video_content(v, max_w, max_h, &vw, &vh);
  if (vw < MIN_W)
    vw = MIN_W;
  if (vh < MIN_H - top_bar_h())
    vh = MIN_H - top_bar_h();

  int ww = vw;
  int wh = vh + top_bar_h();
  int wx = (g_screen_w - ww) / 2;
  int wy = (g_screen_h - wh) / 2;

  if (resize_fb(ww, wh) < 0) {
    os1vid_close(v);
    return 1;
  }

  char title[64];
  snprintf(title, sizeof(title), "nximage: %s", path);
  int win = create_window(wx, wy, ww, wh, title);
  if (win < 0) {
    os1vid_close(v);
    return 1;
  }

  printf("nximage: playing %s (%dx%d, %.2f fps, MPEG1, no audio device)\n",
         path, v->width, v->height, v->framerate);

  int quit = 0;
  int force_redraw = 1;
  while (!quit) {
    int decoded = os1vid_update(v);

    if (decoded || os1vid_is_paused(v) || os1vid_has_ended(v) || force_redraw) {
      force_redraw = 0;
      for (int i = 0; i < g_ww * g_wh; i++)
        g_fb[i] = 0x00000000u;

      int bar_h = top_bar_h();
      int content_h = g_wh - bar_h;
      int ox = (g_ww - vw) / 2;
      int oy = bar_h + (content_h - vh) / 2;
      if (oy < bar_h + PAD)
        oy = bar_h + PAD;
      if (ox < PAD)
        ox = PAD;

      /* Use packed_pixels for safe bounds-checked drawing */
      if (vw == v->width && vh == v->height) {
        for (int y = 0; y < vh; y++) {
          int dy = oy + y;
          if (dy < bar_h || dy >= g_wh)
            continue;
          for (int x = 0; x < vw; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= g_ww)
              continue;
            g_fb[dy * g_ww + dx] = v->packed_pixels[y * v->width + x];
          }
        }
      } else {
        os1_image_t frame_view = {v->width, v->height, v->packed_pixels};
        os1_image_t *scaled = os1_image_resample(&frame_view, vw, vh);
        if (scaled) {
          for (int y = 0; y < scaled->h; y++) {
            int dy = oy + y;
            if (dy < bar_h || dy >= g_wh)
              continue;
            for (int x = 0; x < scaled->w; x++) {
              int dx = ox + x;
              if (dx < 0 || dx >= g_ww)
                continue;
              g_fb[dy * g_ww + dx] = scaled->pixels[y * scaled->w + x];
            }
          }
          free(scaled->pixels);
          free(scaled);
        }
      }

      draw_video_toolbar(v);
      window_blit(win, 0, 0, g_ww, g_wh, g_fb);
      compositor_render();
    }

    if (os1vid_has_ended(v)) {
      os1vid_pause(v);
    }

    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_RESIZE && ev.resize.w > 0 && ev.resize.h > 0) {
        if (resize_fb(ev.resize.w, ev.resize.h) == 0) {
          fit_video_content(v, g_ww, g_wh, &vw, &vh);
          force_redraw = 1;
        }
      } else if (ev.type == INPUT_TYPE_MOUSE &&
                 ev.mouse.button == MOUSE_BTN_LEFT &&
                 ev.mouse.state == KEY_PRESSED) {
        int r = handle_video_toolbar_click(v, ev.mouse.x, ev.mouse.y);
        if (r == 2) {
          fit_video_content(v, g_ww, g_wh, &vw, &vh);
          force_redraw = 1;
        } else if (r == 1) {
          force_redraw = 1;
        }
      } else if (ev.type == INPUT_TYPE_KEYBOARD &&
                 ev.keyboard.state == KEY_PRESSED) {
        if (ev.keyboard.key == 'q' || ev.keyboard.key == 'Q' ||
            ev.keyboard.scancode == INPUT_KEY_ESC) {
          quit = 1;
        } else if (ev.keyboard.key == ' ') {
          if (os1vid_is_paused(v))
            os1vid_resume(v);
          else
            os1vid_pause(v);
          force_redraw = 1;
        }
      }
    }
    OS1_sleep(8);
  }

  destroy_window(win);
  os1vid_close(v);
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

  if (os1vid_path_has_known_ext(path)) {
    if (term) {
      printf("nximage: --term does not support video playback; drop --term to "
             "open %s in a window.\n",
             path);
      return 1;
    }
    g_font = font_load("/fonts/Rewir-Light.off");
    return run_video(path);
  }

  os1_image_t *img = os1_image_load(path);
  if (!img) {
    printf("nximage: unable to load '%s'\n", path);
    printf("usage: nximage [--term] [file.png|file.mpg]\n");
    return 1;
  }

  printf("nximage: decoded %s (%dx%d, sanitized ARGB)\n", path, img->w, img->h);
  int r = term ? run_terminal(path, img) : run_window(path, img);
  clear_scaled();
  os1_image_free(img);
  free(g_fb);
  return r;
}