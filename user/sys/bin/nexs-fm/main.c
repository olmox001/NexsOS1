/*
 * NeXs File Manager - Professional File Manager for OS1
 * Main entry point and initialization
 */
#include "nexs-fm.h"

void fm_init(void) {
    fm_state_init();

    fm_state.window_id = create_window(0, 0, FM_WIN_W, FM_WIN_H, "NeXs File Manager");
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
    uint32_t last_draw = 0;
    
    while (fm_state.running) {
        uint32_t now = get_time();
        
        /* Rate limiting: disegna massimo ~60 FPS */
        if (now - last_draw >= 16) {  /* ~60 FPS */
            fm_draw_full_ui();
            last_draw = now;
        }
        
        /* Poll input */
        if (input_poll_event(&event) == 1) {
            if (event.type == INPUT_TYPE_KEYBOARD) {
                fm_handle_keyboard(&event);
            } else if (event.type == INPUT_TYPE_MOUSE) {
                fm_handle_mouse(&event);
            }
        }
        
        yield();
        sleep(1);  /* Piccola pausa per dare respiro al sistema */
    }
}

int main(void) {
    fm_init();
    fm_main_loop();
    fm_cleanup();
    return 0;
}
