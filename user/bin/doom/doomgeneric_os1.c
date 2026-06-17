#include "doomgeneric.h"
#include <os1.h>
#include <unistd.h>   /* usleep() — real microsecond blocking sleep */
#include <input.h>
#include <graphics.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int s_window = -1;

void DG_Init() {
    printf("DG_Init: Creating window...\n");
    /* Doom resolution is typically 640x400 or 320x200 */
    s_window = create_window(50, 50, DOOMGENERIC_RESX, DOOMGENERIC_RESY, "DoomGeneric OS1");
    if (s_window < 0) {
        printf("DG_Init: FAILED to create window!\n");
        exit(1);
    }
    int my_pid = get_pid();
    printf("DG_Init: Window created, id=%d, my_pid=%d\n", s_window, my_pid);
    
    /* Standardized focus handling */
    set_focus(my_pid);
    printf("DG_Init: Focus set to PID %d\n", my_pid);
}

void DG_DrawFrame() {
    if (s_window >= 0 && DG_ScreenBuffer) {
        /* Use new graphics library blit */
        graphics_blit(s_window, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY, (const uint32_t*)DG_ScreenBuffer);
    }
}

void DG_SleepMs(uint32_t ms) {
    /* sleep()/usleep() now block in real wall-clock time (SYS_NANOSLEEP).
     * Wait exactly `ms` milliseconds; usleep() gives finer-than-tick
     * granularity for the short sub-frame waits doom requests. A zero
     * request just yields the rest of our slice. */
    if (ms) usleep(ms * 1000);
    else yield();
}

uint32_t DG_GetTicksMs() {
    /* get_time() in OS1 returns milliseconds since boot */
    return (uint32_t)get_time();
}

int DG_GetKey(int* pressed, unsigned char* key) {
    input_event_t event;
    
    /* Use new standardized input API */
    while (input_poll_event(&event)) {
        if (event.type == INPUT_TYPE_KEYBOARD) {
            *pressed = (event.keyboard.state != KEY_RELEASED);
            unsigned char c = event.keyboard.key;
            
            /* Enhanced Key Mapping for Doom */
            if (c == '\n' || c == '\r') {
                *key = 0x0D; /* KEY_ENTER */
            } else if (c == 27) {
                *key = 0x1B; /* KEY_ESCAPE */
            } else if (c == '\b' || c == 127) {
                *key = 0x08; /* KEY_BACKSPACE */
            } else if (c == ' ') {
                *key = ' ';  /* KEY_USE */
            } else if (c == 'w' || c == 'W') {
                *key = 0xad; /* KEY_UPARROW */
            } else if (c == 's' || c == 'S') {
                *key = 0xaf; /* KEY_DOWNARROW */
            } else if (c == 'a' || c == 'A') {
                *key = 0xac; /* KEY_LEFTARROW */
            } else if (c == 'd' || c == 'D') {
                *key = 0xae; /* KEY_RIGHTARROW */
            } else {
                *key = c;
            }
            return 1;
        } else if (event.type == INPUT_TYPE_MOUSE) {
            /* We can handle mouse buttons here if needed */
            if (event.mouse.button == 1) { // Left click
                *pressed = event.mouse.state;
                *key = 0x9d; // KEY_FIRE (mapped to Ctrl or similar in some engines, but let's use a placeholder)
                // DoomGeneric usually expects keyboard keys for fire. 
                // We'll map left click to 'fire' if it's a press.
                return 1;
            }
        }
    }
    
    return 0;
}

void DG_SetWindowTitle(const char * title) {
    /* Window titles are static in OS1 for now */
    (void)title;
}

int main(int argc, char **argv) {
    printf("Doom OS1 starting (argc=%d)...\n", argc);
    
    /* Initialize DoomGeneric engine */
    doomgeneric_Create(argc, argv);
    
    printf("Doom engine initialized, starting tick loop...\n");
    while (1) {
        /* Engine main loop step */
        doomgeneric_Tick();
        
        /* Yield to other processes to ensure system responsiveness */
        yield();
    }
    
    return 0;
}
