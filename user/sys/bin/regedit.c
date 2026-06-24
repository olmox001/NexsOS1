/*
 * user/sys/bin/regedit.c
 * Registry Editor (Control Panel)
 *
 * Displays a simple control panel window showing a fixed set of registry keys
 * and their current values.  Intended to allow inspection (and eventually
 * editing) of system configuration stored in the global registry.
 *
 * Current state:
 *   - The display loop draws coloured rectangles per key entry but cannot
 *     render text labels — window_draw only fills rectangles; the commented-out
 *     window_print() calls are stubs.
 *   - registry_read() fetches values correctly, but they are invisible in the
 *     window because the text-draw calls are disabled.
 *
 * Known issues:
 *   USR-REGEDIT-01 (W3 BUG·STUB) recv(0, &msg) at line ~59 is a BLOCKING
 *                  call (SYS_RECV, syscall #231 with no timeout).  The
 *                  comment on that line acknowledges this: "No, recv blocks.
 *                  We need an event loop."  The window will freeze indefinitely
 *                  waiting for an IPC message that never comes, since nothing
 *                  sends IPC to regedit.  The yield() after it is unreachable.
 *   USR-SEC-01     (W3 SECURITY) registry_read/write has no authentication;
 *                  any process can overwrite any key.
 *   USR-BLOAT-01/02 (W2 BAD-IMPL·PERF) ~500KB ELF due to unconditional
 *                  stb_image/stb_easy_font in lib.o and retained DWARF debug.
 */
#include <os1.h>

#define MAX_KEYS 16       /* Maximum number of registry keys the UI can show */
#define KEY_WIDTH 250     /* Width of each key entry row rectangle (px) */
#define KEY_HEIGHT 30     /* Height of each key entry row rectangle (px) */
#define PADDING 10        /* Margin between rows and window edge (px) */

/*
 * main - registry editor entry point; does not return.
 *
 * Creates a 400x300 compositor window.  In each iteration of the main loop:
 *   1. Clears the window background.
 *   2. For each registered key, draws a white rectangle and calls
 *      registry_read() to fetch the value — but the text display is commented
 *      out (no userland text renderer is available via window_draw).
 *   3. Calls recv(0, &msg) — a BLOCKING IPC receive — which hangs the loop
 *      indefinitely since no sender targets regedit.
 *   4. compositor_render() and yield() after recv() are unreachable.
 *
 * NOTE(USR-REGEDIT-01): The recv() call blocks forever; the window appears
 *   as a grey rectangle with no content and no responsiveness to interaction.
 *   Correct fix: replace recv() with try_recv() and add mouse-event handling.
 *
 * Returns 1 if window creation fails, never returns otherwise.
 */
int main(void) {
  int w = 400;
  int h = 300;
  int win_id = create_window(100, 100, w, h, "Control Panel");
  if (win_id < 0)
    return 1;

  /* Hardcoded initial set of three registry keys to display.
   * MAX_KEYS entries allocated but only key_count=3 used. */
  char keys[MAX_KEYS][64] = {"theme.color", "system.hostname",
                             "mouse.sensitivity"};
  int key_count = 3;

  char val_buf[128];  /* Buffer for registry_read() output */

  while (1) {
    /* Clear background */
    window_draw(win_id, 0, 0, w, h, 0xFFE0E0E0);

    /* Draw Title */
    // TODO: Font rendering in window_draw? For now we only have basic rects.
    // We really need a proper UI library or font support in userspace.
    // Using `notify` as a poor-man's persistent display is not great.

    int y = PADDING;
    for (int i = 0; i < key_count; i++) {
      /* Draw white rectangle behind each key entry */
      window_draw(win_id, PADDING, y, w - 2 * PADDING, KEY_HEIGHT, 0xFFFFFFFF);

      /* Fetch Value from global registry.
       * registry_read returns 0 on success, negative on not-found. */
      if (OS1_registry_get(keys[i], val_buf, sizeof(val_buf)) == 0) {
        /* Display Key */
        // window_print(win_id, PADDING + 5, y + 5, keys[i], 0xFF000000);
        /* Display Value */
        // window_print(win_id, PADDING + 150, y + 5, val_buf, 0xFF0000BB);
        /* NOTE: window_print() is not implemented; text is invisible.
         * val_buf contains the correct value but cannot be shown. */
      }

      y += KEY_HEIGHT + PADDING;
    }

    /*
     * CRITICAL LIMITATION: We don't have text rendering in userspace library
     * yet! The `window_draw` syscall only does rectangles. The kernel
     * compositor draws text, but userspace cannot (easily).
     *
     * Workaround: Use `notify()` to show current values when "clicked".
     */

    struct ipc_message msg;
    /* NOTE(USR-REGEDIT-01): recv() is BLOCKING (SYS_RECV #231, no timeout).
     * Nothing sends IPC messages to regedit, so this call hangs the loop
     * indefinitely.  compositor_render() and yield() below are unreachable.
     * Should be replaced with try_recv() and a proper event-driven loop. */
    if (recv(0, &msg)) { /* Non-blocking check? No, recv blocks. */
                         /* We need an event loop. */
    }

    compositor_render();
    yield();
  }

  return 0;
}
