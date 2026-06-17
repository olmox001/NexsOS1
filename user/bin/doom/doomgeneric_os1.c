#include "doomgeneric.h"
#include <graphics.h>
#include <input.h>
#include <os1.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* usleep() — real microsecond blocking sleep */

static int s_window = -1;
char *savedir = "/bin";

void DG_Init() {
  printf("DG_Init: Creating window...\n");
  /* Doom resolution is typically 640x400 or 320x200 */
  s_window = create_window(50, 50, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                           "DoomGeneric OS1");
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
    graphics_blit(s_window, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                  (const uint32_t *)DG_ScreenBuffer);
  }
}

void DG_SleepMs(uint32_t ms) {
  /* sleep()/usleep() now block in real wall-clock time (SYS_NANOSLEEP).
   * Wait exactly `ms` milliseconds; usleep() gives finer-than-tick
   * granularity for the short sub-frame waits doom requests. A zero
   * request just yields the rest of our slice. */
  if (ms)
    usleep(ms * 1000);
  else
    yield();
}

uint32_t DG_GetTicksMs() {
  /* get_time() in OS1 returns milliseconds since boot */
  return (uint32_t)get_time();
}

int DG_GetKey(int *pressed, unsigned char *key) {
  input_event_t event;

  /* Use new standardized input API */
  while (input_poll_event(&event)) {
    if (event.type == INPUT_TYPE_KEYBOARD) {
      *pressed = (event.keyboard.state != KEY_RELEASED);
      unsigned char c = event.keyboard.key;

      // Forza la conversione in minuscolo
      if (c >= 'A' && c <= 'Z') {
        c = c + ('a' - 'A');
      }

      /* Enhanced Key Mapping for Doom */
      if (c == '\n' || c == '\r') {
        *key = 13; /* KEY_ENTER */
      } else if (c == 27) {
        *key = 27; /* KEY_ESCAPE */
      } else if (c == '\b' || c == 127) {
        *key = 0x7f; /* KEY_BACKSPACE */
      } else if (c == 'l') {
        *key = 0xa2; /* KEY_USE (Apri porte) */
      } else if (c == 'w') {
        *key = 0xad; /* KEY_UPARROW */
      } else if (c == 's') {
        *key = 0xaf; /* KEY_DOWNARROW */
      } else if (c == 'a') {
        *key = 0xac; /* KEY_LEFTARROW */
      } else if (c == 'd') {
        *key = 0xae; /* KEY_RIGHTARROW */
      } else if (c == 'k') {
        *key = 0xa3;         /* KEY_FIRE (Spara) */
      } else if (c == 'm') { // <--- AGGIUNTO: TASTO M PER APRIRE IL MENU
        *key = 27;           /* Forza KEY_ESCAPE */
      } else if (c == 'j') { // <--- AGGIUNTO: TASTO J PER RINASCERE / INVIO
        *key = 13;           /* Forza KEY_ENTER */
      } else {
        *key = c;
      }
      return 1;
    } else if (event.type == INPUT_TYPE_MOUSE) {
      if (event.mouse.button == 1) { // Click Sinistro
        *pressed = (event.mouse.state != 0);
        *key = 0xa3; // KEY_FIRE
        return 1;
      }
    }
  }

  return 0;
}

void DG_SetWindowTitle(const char *title) {
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
