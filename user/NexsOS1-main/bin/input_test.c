#include <os1.h>
#include <stdio.h>
#include <input.h>
#include <graphics.h>
#include <string.h>

int main(void) {
    printf("Graphical Input Test Starting...\n");

    int width = 640;
    int height = 480;
    int win = create_window(100, 100, width, height, "Modern Input Diagnostic");
    set_focus(get_pid());

    char last_key[64] = "None";
    int mouse_x = 0, mouse_y = 0;
    int mouse_btn = 0;

    input_event_t ev;
    while (1) {
        /* Clear background with a dark gray */
        graphics_draw_rect(win, 0, 0, width, height, 0xFF111111);

        /* Draw UI info */
        graphics_draw_text(win, 20, 20, "Modern OS1 Input System", 0xFFFFFFFF);
        graphics_draw_text(win, 20, 50, "Keyboard (UTF-8):", 0xFFAAAAAA);
        graphics_draw_text(win, 160, 50, last_key, 0xFF00FF00);
        
        char mouse_info[64];
        snprintf(mouse_info, sizeof(mouse_info), "Mouse: X=%d, Y=%d (Btn=%d)", mouse_x, mouse_y, mouse_btn);
        graphics_draw_text(win, 20, 80, mouse_info, 0xFF00FFFF);

        /* Draw a small cursor (simulated by rects) */
        graphics_draw_rect(win, mouse_x - 10, mouse_y, 20, 2, 0xFFFF0000);
        graphics_draw_rect(win, mouse_x, mouse_y - 10, 2, 20, 0xFFFF0000);

        while (input_poll_event(&ev)) {
            if (ev.type == INPUT_TYPE_KEYBOARD) {
                if (ev.keyboard.state != 0) { // Pressed or Repeat
                    if (ev.keyboard.utf8[0] != '\0') {
                        snprintf(last_key, sizeof(last_key), "'%s' (Code %d)", ev.keyboard.utf8, ev.keyboard.scancode);
                    } else {
                        snprintf(last_key, sizeof(last_key), "SC %d", ev.keyboard.scancode);
                    }
                }
                if (ev.keyboard.key == 27) goto cleanup; // ESC
            } else if (ev.type == INPUT_TYPE_MOUSE) {
                mouse_x = ev.mouse.x;
                mouse_y = ev.mouse.y;
                mouse_btn = ev.mouse.button;
            }
        }
        yield();
    }

cleanup:
    destroy_window(win);
    return 0;
}
