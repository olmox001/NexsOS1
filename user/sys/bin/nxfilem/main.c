/*
 * user/sys/bin/nxfilem/main.c
 * Entry point: window/state setup and the ~30Hz event loop.
 *
 * Same loop shape as nxsettings.c/nxui.c: redraw (internally gated by a
 * state-signature so an unchanged frame never blits), drain every queued
 * input event, sleep — never a busy-spin.
 */
#include "nxfilem.h"

static void fm_cleanup(void) {
  if (fm_state.window_id >= 0)
    destroy_window(fm_state.window_id);
}

int main(void) {
  fm_state_init();

  if (fm_ui_init() != 0) {
    printf("nxfilem: failed to create window\n");
    return 1;
  }

  /* First listing before set_focus — the compositor has crashed in the past
   * on a focus request racing an unpopulated window (see fileops history). */
  fm_refresh_directory();
  set_focus(get_pid());
  fm_ui_redraw(1);

  while (fm_state.running) {
    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_KEYBOARD) {
        fm_handle_keyboard(&ev);
      } else if (ev.type == INPUT_TYPE_MOUSE) {
        if (ev.mouse.state == KEY_PRESSED)
          fm_ui_handle_click(ev.mouse.x, ev.mouse.y, ev.mouse.button);
      } else if (ev.type == INPUT_TYPE_RESIZE && ev.resize.w > 0 &&
                ev.resize.h > 0) {
        fm_ui_reinit_window(ev.resize.w, ev.resize.h);
      } else if (ev.type == INPUT_TYPE_LOOK_CHANGED) {
        fm_ui_load_theme(nxres_theme_is_light());
      }
    }

    fm_ui_redraw(0);
    OS1_sleep(33);
  }

  fm_cleanup();
  return 0;
}
