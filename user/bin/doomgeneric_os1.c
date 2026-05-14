#include "doomgeneric-master/doomgeneric/doomgeneric.h"
#include <os1.h>
#include <stddef.h>
#include <stdio.h>

static int s_window = -1;

void DG_Init() {
    printf("DG_Init: Creating window...\n");
    /* Doom resolution is typically 640x400 or 320x200 */
    s_window = create_window(50, 50, DOOMGENERIC_RESX, DOOMGENERIC_RESY, "DoomGeneric OS1");
    if (s_window < 0) {
        printf("DG_Init: FAILED to create window!\n");
        exit(1);
    }
    printf("DG_Init: Window created, id=%d\n", s_window);
    set_focus(get_pid());
}

void DG_DrawFrame() {
    if (s_window >= 0 && DG_ScreenBuffer) {
        /* Blit the entire screen buffer to our window */
        window_blit(s_window, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY, (const unsigned int*)DG_ScreenBuffer);
    }
}

void DG_SleepMs(uint32_t ms) {
    /* OS1 ticks are roughly 10ms. Sleep(1) is 10ms. */
    if (ms < 10) yield();
    else sleep(ms / 10);
}

uint32_t DG_GetTicksMs() {
    /* get_time() in OS1 returns milliseconds since boot */
    return (uint32_t)get_time();
}

int DG_GetKey(int* pressed, unsigned char* key) {
    struct ipc_message msg;
    /* Try to receive a key event from the kernel/compositor */
    if (try_recv(-1, &msg) == 0) {
        if (msg.type == 0x10) { /* IPC_TYPE_INPUT = 0x10 */
            *pressed = (int)msg.data2;
            char c = (char)msg.data1;
            
            /* DoomGeneric expects some specific keycodes. 
             * We map basic ASCII for now. */
            if (c == '\n' || c == '\r') {
                *key = 0x0D; /* KEY_ENTER */
            } else if (c == 27) {
                *key = 0x1B; /* KEY_ESCAPE */
            } else if (c == '\b' || c == 127) {
                *key = 0x08; /* KEY_BACKSPACE */
            } else {
                *key = (unsigned char)c;
            }
            return 1;
        }
    }
    return 0;
}

void DG_SetWindowTitle(const char * title) {
    /* Window titles are static in OS1 for now */
    (void)title;
}

int main(int argc, char **argv) {
    printf("Doom starting (argc=%d)...\n", argc);
    
    /* Initialize DoomGeneric engine */
    doomgeneric_Create(argc, argv);
    
    printf("Doom engine initialized, starting tick loop...\n");
    static int frame_count = 0;
    while (1) {
        /* Engine main loop step */
        doomgeneric_Tick();
        
        /* Yield to other processes to ensure system responsiveness */
        yield();

        if ((++frame_count % 350) == 0) {
            printf("Doom: tick count %d\n", frame_count);
        }
    }
    
    return 0;
}
