/*
 * NeXs File Manager - Professional File Manager for OS1
 * Main entry point and initialization
 */
#include "nxfilem.h"

void fm_init(void) {
  fm_state_init();

  fm_state.window_id =
      create_window(40, 80, FM_WIN_W, FM_WIN_H, "NeXs File Manager");
  if (fm_state.window_id < 0) {
    printf("ERRORE: Impossibile creare finestra!\n");
    exit(1);
  }

  /* Prima refresh directory per avere contenuto valido */
  fm_refresh_directory();

  /* Poi set focus - ordine critico per evitare crash compositor */
  set_focus(get_pid());

  printf("NeXs FM avviato - Finestra ID: %d\n", fm_state.window_id);
}

void fm_cleanup(void) {
  if (fm_state.window_id >= 0) {
    destroy_window(fm_state.window_id);
  }
}

void fm_main_loop(void) {
  input_event_t event;

  while (fm_state.running) {
    /* Drena tutti gli eventi input disponibili: il driver PS/2 e
     * virtio-input possono accumulare più IPC in coda tra un poll e
     * l'altro, e se ne processiamo uno solo al ciclo il mouse
     * "scatta". */
    while (input_poll_event(&event) == 1) {
      if (event.type == INPUT_TYPE_KEYBOARD) {
        fm_handle_keyboard(&event);
      } else if (event.type == INPUT_TYPE_MOUSE) {
        fm_handle_mouse(&event);
      }
    }

    if (fm_state.needs_redraw) {
      fm_draw_full_ui();
      fm_state.needs_redraw = 0;
    }

    /* Sleep breve e BLOCCANTE via il timer reale del kernel
     * (vedi lib.c::sleep — no busy-wait). 2 ms è abbastanza per
     * non sentire il mouse "lento" ma abbastanza per non saturare
     * una CPU quando la finestra è inattiva. */
    yield();
    OS1_sleep(2);
  }
}

int main(void) {
  fm_init();
  fm_main_loop();
  fm_cleanup();
  return 0;
}
