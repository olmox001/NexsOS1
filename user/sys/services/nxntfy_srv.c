/*
 * nxntfy_srv: authenticated named-port notification receiver, legacy kernel
 * fault mailbox consumer, GUI popup renderer and registry read-receipt ring.
 * All mutable state belongs to one supervised userland service generation.
 *
 * WARN notifications are deduplicated per sender and panel-only. ERROR/INFO
 * show a passive topmost popup unless the notification panel is visible.
 * This receiver must never notify itself when its own delivery path fails:
 * self-diagnostics persist to the registry with a bounded failure record.
 */
#include <font_lib.h>
#include <input.h>
#include <os1.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../bin/nxres.h"

#define NOTIFY_WIDTH 280
#define NOTIFY_HEIGHT 56
#define NOTIFY_RADIUS 10
#define NOTIFY_PIXELS (NOTIFY_WIDTH * NOTIFY_HEIGHT)
#define NOTIFY_POLL_MS 300
#define NOTIFY_TOP_GAP (7 + 26 + 4)
#define WARN_DEDUP_MAX 16
#define COL_ERROR_BG 0xD0FF3B30u
#define COL_WARN_BG 0xD0FF9500u
#define COL_SEV_TEXT 0xFFFFFFFFu

struct notify_state {
  uint32_t col_bg, col_text, col_title;
  uint32_t *pixels;
  struct font_ctx *font;
  int window, screen_w, screen_h;
  int warn_pid[WARN_DEDUP_MAX];
  char warn_text[WARN_DEDUP_MAX][64];
  int warn_count, warn_evict;
  int visible, ring_head, pending_slot, pending_from, pending_severity;
  long visible_since;
  char pending_text[56];
};

static int notif_fail(const char *phase, int code) {
  char record[80];
  int len = snprintf(record, sizeof(record), "%s|%d", phase, code);
  /* A dead notification UI cannot be its own only diagnostic sink.
   * Standard user stdout is mirrored to UART by the existing kernel ABI. */
  int logged = printf("[NXNOTIFY][ERROR] %s: status=%d\n", phase, code);
  if (len > 0 && (size_t)len < sizeof(record)) {
    long published = OS1_registry_set("sys.ntfy.fault", record);
    if (published != 0 || logged < 0) return 1;
  }
  return 1;
}

static void notif_palette(struct notify_state *s, int light) {
  if (light) {
    s->col_bg = 0xE8F5F5F7u;
    s->col_text = 0xFF1C1C1Eu;
    s->col_title = 0xFF000000u;
  } else {
    s->col_bg = 0xE81C1C24u;
    s->col_text = 0xFFF0F0F5u;
    s->col_title = 0xFFFFFFFFu;
  }
}

static void notif_rect(struct notify_state *s, int radius, uint32_t color) {
  for (int y = 0; y < NOTIFY_HEIGHT; ++y) {
    for (int x = 0; x < NOTIFY_WIDTH; ++x) {
      int cx = -1, cy = 0;
      if (x < radius && y < radius) { cx = radius; cy = radius; }
      else if (x >= NOTIFY_WIDTH - radius && y < radius)
        { cx = NOTIFY_WIDTH - radius - 1; cy = radius; }
      else if (x < radius && y >= NOTIFY_HEIGHT - radius)
        { cx = radius; cy = NOTIFY_HEIGHT - radius - 1; }
      else if (x >= NOTIFY_WIDTH - radius && y >= NOTIFY_HEIGHT - radius)
        { cx = NOTIFY_WIDTH - radius - 1; cy = NOTIFY_HEIGHT - radius - 1; }
      if (cx >= 0) {
        int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy > radius * radius) continue;
      }
      s->pixels[y * NOTIFY_WIDTH + x] = color;
    }
  }
}

static void notif_glyph(struct notify_state *s, int x, int y,
                        uint32_t cp, uint32_t color) {
  if (!s->font) return;
  int index = (int)cp - s->font->header.first_char;
  if (index < 0 || index >= s->font->header.num_chars) return;
  struct font_glyph_info *glyph = &s->font->glyphs[index];
  uint8_t *bitmap = s->font->bitmap + glyph->data_offset;
  int sx = x + glyph->x0;
  int sy = y + s->font->header.ascent + glyph->y0;
  for (int gy = 0; gy < NOTIFY_HEIGHT; ++gy) {
    if (gy >= glyph->height) break;
    for (int gx = 0; gx < NOTIFY_WIDTH; ++gx) {
      if (gx >= glyph->width) break;
      int px = sx + gx, py = sy + gy;
      if (px >= 0 && px < NOTIFY_WIDTH && py >= 0 &&
          py < NOTIFY_HEIGHT && bitmap[gy * glyph->width + gx] > 64)
        s->pixels[py * NOTIFY_WIDTH + px] = color;
    }
  }
}

static void notif_text(struct notify_state *s, int x, int y,
                       const char *text, uint32_t color) {
  if (!s->font || !text) return;
  size_t remaining = strnlen(text, 64U);
  const char *position = text;
  int cursor = x;
  for (size_t step = 0U; step < 64U; ++step) {
    if (remaining == 0U) break;
    uint32_t codepoint = 0U;
    int consumed = utf8_decode(position, remaining, &codepoint);
    if (consumed <= 0) consumed = 1;
    if ((size_t)consumed > remaining) break;
    notif_glyph(s, cursor, y, codepoint, color);
    int index = (int)codepoint - s->font->header.first_char;
    if (index >= 0 && index < s->font->header.num_chars)
      cursor += s->font->glyphs[index].advance;
    position += consumed;
    remaining -= (size_t)consumed;
  }
}

static int notif_warn_duplicate(const struct notify_state *s, int pid,
                                const char *text) {
  for (int i = 0; i < WARN_DEDUP_MAX; ++i) {
    if (i >= s->warn_count) break;
    if (s->warn_pid[i] == pid)
      return strncmp(s->warn_text[i], text, sizeof(s->warn_text[i])) == 0;
  }
  return 0;
}

static void notif_copy_text(char destination[64], const char *text) {
  size_t index = 0U;
  for (; index < 63U && text[index]; ++index)
    destination[index] = text[index];
  destination[index] = '\0';
}

static void notif_warn_remember(struct notify_state *s, int pid,
                                const char *text) {
  int slot = s->warn_count;
  for (int i = 0; i < WARN_DEDUP_MAX; ++i) {
    if (i >= s->warn_count) break;
    if (s->warn_pid[i] == pid) { slot = i; break; }
  }
  if (slot >= s->warn_count) {
    if (s->warn_count < WARN_DEDUP_MAX) ++s->warn_count;
    else {
      slot = s->warn_evict;
      s->warn_evict = (s->warn_evict + 1) % WARN_DEDUP_MAX;
    }
  }
  s->warn_pid[slot] = pid;
  notif_copy_text(s->warn_text[slot], text);
}

static int notif_window_resize(struct notify_state *s, int sw, int sh) {
  /* Fixed BSS-backed notification surface is process-owned. */
  if (s->pixels == NULL) return -EFAULT;
  int x = sw - NOTIFY_WIDTH - 7;
  int y = NOTIFY_TOP_GAP;
  int replacement = create_window(x, y, NOTIFY_WIDTH, NOTIFY_HEIGHT,
                                  "nxntfy_srv");
  if (replacement < 0) return -EIO;
  if (s->window >= 0) destroy_window(s->window);
  s->window = replacement;
  s->screen_w = sw;
  s->screen_h = sh;
  set_window_flags(s->window, 1 | 4 | 8);
  return 0;
}

static int notif_receive(int port, struct ipc_message *message) {
  long result = OS1_port_try_recv(port, message);
  if (result == (long)sizeof(*message)) return 1;
  if (result != -EAGAIN) return result < 0 ? (int)result : -EIO;
  /* Keep kernel fault notifications on the original process mailbox. */
  long legacy = OS1low_ipc_try_recv(-1, message);
  if (legacy == 0) return 1;
  if (legacy == -EAGAIN) return 0;
  return legacy < 0 ? (int)legacy : -EIO;
}

static int notif_publish(int slot, int pid, int severity, char state,
                         const char *text) {
  char key[24], record[80];
  int a = snprintf(key, sizeof(key), "sys.ntfy.log.%d", slot);
  int b = snprintf(record, sizeof(record), "%d|%d|%c|%.48s",
                   pid, severity, state, text);
  if (a <= 0 || (size_t)a >= sizeof(key) ||
      b <= 0 || (size_t)b >= sizeof(record))
    return -EOVERFLOW;
  long written = OS1_registry_set(key, record);
  return written == 0 ? 0 : written < 0 ? (int)written : -EIO;
}

static int notif_show(struct notify_state *s,
                      const struct ipc_message *message) {
  char panel[2] = {0};
  int open = OS1_registry_get("sys.ntfy.panel_open", panel,
                              sizeof(panel)) == 0 && panel[0] == '1';
  if (open) return 0;
  uint32_t bg = s->col_bg, text = s->col_text, title = s->col_title;
  const char *heading = "Notification";
  if (message->data1 == 2U) {
    bg = COL_ERROR_BG; text = COL_SEV_TEXT;
    title = COL_SEV_TEXT; heading = "ERROR";
  }
  if (!s->pixels || !s->font) return -ENOMEM;
  notif_rect(s, NOTIFY_RADIUS, bg);
  notif_text(s, 12, 10, heading, title);
  notif_text(s, 12, 30, message->payload, text);
  window_blit(s->window, 0, 0, NOTIFY_WIDTH, NOTIFY_HEIGHT, s->pixels);
  set_window_flags(s->window, 1 | 2 | 8);
  s->visible = 1;
  s->visible_since = get_time();
  return 1;
}

static void notif_remember_receipt(struct notify_state *s, int slot,
                                   const struct ipc_message *message) {
  s->pending_slot = slot;
  s->pending_from = (int)message->from;
  s->pending_severity = (int)message->data1;
  size_t index = 0U;
  for (; index < sizeof(s->pending_text) - 1U && message->payload[index];
       ++index)
    s->pending_text[index] = message->payload[index];
  s->pending_text[index] = '\0';
}

static int notif_handle_message(struct notify_state *s,
                                struct ipc_message *message) {
  if (message->type != IPC_TYPE_NOTIFY) return 0;
  if (message->data2 == IPC_LOOK_PING_MAGIC) {
    notif_palette(s, nxres_theme_is_light());
    return 0;
  }
  message->payload[sizeof(message->payload) - 1U] = '\0';
  /* UART must record every received message independently of windows,
   * warning de-duplication and the bounded GUI receipt registry. */
  char uart_text[sizeof(message->payload)];
  for (unsigned i = 0U; i < sizeof(uart_text); ++i) {
    unsigned char ch = (unsigned char)message->payload[i];
    uart_text[i] = ch == 0U ? '\0' :
                   (ch < 32U || ch == 127U) ? '?' : (char)ch;
  }
  const char *severity = message->data1 == 2U ? "ERROR" :
                         message->data1 == 1U ? "WARN" : "INFO";
  if (printf("[NXNOTIFY][%s] pid=%d %s\n",
             severity, (int)message->from, uart_text) < 0)
    return -EIO;
  int warning = message->data1 == 1U;
  if (warning && notif_warn_duplicate(s, (int)message->from,
                                      message->payload)) return 0;
  if (warning) notif_warn_remember(s, (int)message->from, message->payload);
  int shown = warning ? 0 : notif_show(s, message);
  if (shown < 0) return shown;
  int slot = s->ring_head & 0x0F;
  int result = notif_publish(slot, (int)message->from,
                             (int)message->data1, 'U', message->payload);
  if (result != 0) return result;
  ++s->ring_head;
  if (shown > 0) notif_remember_receipt(s, slot, message);
  return 0;
}

static int notif_hide_if_elapsed(struct notify_state *s) {
  if (!s->visible || get_time() - s->visible_since < 2000) return 0;
  set_window_flags(s->window, 1 | 4 | 8);
  s->visible = 0;
  if (s->pending_slot >= 0) {
    int saved = notif_publish(s->pending_slot, s->pending_from,
                              s->pending_severity, 'R', s->pending_text);
    if (saved != 0) return saved;
    s->pending_slot = -1;
  }
  return 0;
}

static int notif_iteration(struct notify_state *s, int port) {
  long packed = OS1_display_info();
  int sw = (int)((packed >> 16) & 0xffff);
  int sh = (int)(packed & 0xffff);
  if (sw > 0 && sh > 0 &&
      (sw != s->screen_w || sh != s->screen_h)) {
    int resized = notif_window_resize(s, sw, sh);
    if (resized != 0) return resized;
  }
  struct ipc_message message = {0};
  int received = notif_receive(port, &message);
  if (received < 0) return received;
  if (received > 0) {
    int handled = notif_handle_message(s, &message);
    if (handled != 0) return handled;
  }
  int hidden = notif_hide_if_elapsed(s);
  if (hidden != 0) return hidden;
  OS1_sleep(s->visible ? 100 : NOTIFY_POLL_MS);
  return 0;
}

int main(void) {
  /* Bounded process-local surface avoids heap allocation during service
   * restart, resizing or the resident notification event loop. */
  static uint32_t frame[NOTIFY_PIXELS];
  struct notify_state state = {
      .pixels = frame, .window = -1, .pending_slot = -1};
  state.font = font_load("/fonts/Rewir-Light.off");
  if (state.font == NULL) return notif_fail("font_load", -ENOENT);
  notif_palette(&state, nxres_theme_is_light());
  long info = OS1_display_info();
  int sw = (int)((info >> 16) & 0xffff);
  int sh = (int)(info & 0xffff);
  if (sw <= 0) sw = 800;
  if (sh <= 0) sh = 600;
  int result = notif_window_resize(&state, sw, sh);
  if (result == 0) {
    int port = OS1_port_create(OS1NX_PORT_NOTIFY);
    if (port >= 0) {
      for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
        result = notif_iteration(&state, port);
        if (result != 0) break;
      }
      long closed = OS1low_handle_close(port);
      if (closed < 0 && result == 0) result = (int)closed;
    } else result = port;
  }
  if (state.window >= 0) destroy_window(state.window);
  font_free(state.font);
  return notif_fail("notification_service", result ? result : -EIO);
}
