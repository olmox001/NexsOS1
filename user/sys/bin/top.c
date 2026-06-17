/*
 * user/bin/proce_top.c
 * Realtime Process List Utility - Full 32-Process Atomic Edition
 */
#include "proce.h"
#include <os1.h>

// Funzione helper per convertire i numeri in stringa senza usare printf esterne
// e rischiare overflow
static int mini_itoa(char *buf, int num) {
  int i = 0;
  if (num == 0) {
    buf[i++] = '0';
    buf[i] = '\0';
    return i;
  }
  char temp[10];
  int t_idx = 0;
  while (num > 0) {
    temp[t_idx++] = (num % 10) + '0';
    num /= 10;
  }
  for (int j = 0; j < t_idx; j++) {
    buf[i++] = temp[t_idx - 1 - j];
  }
  buf[i] = '\0';
  return i;
}

// Helper per copiare stringhe con lunghezza fissa (padding/clipping manuale)
static int copy_str_fixed(char *dest, const char *src, int width) {
  int i = 0;
  while (src[i] != '\0' && i < width) {
    dest[i] = src[i];
    i++;
  }
  while (i < width) {
    dest[i] = ' '; // Padding di spazi
    i++;
  }
  return width;
}

int main(void) {
  struct ps_info procs[32];

  // Creazione finestra (leggermente più alta per ospitare fino a 32 righe, es.
  // 600px)
  int my_win =
      _sys_create_window(100, 100, 520, 600, "NEXS Realtime Process List");
  if (my_win < 0)
    return 1;

  // Buffer locale allargato a 4096 byte per contenere fino a 32 processi con i
  // relativi codici colore
  char screen_buffer[4096];

  while (1) {
    int count = _sys_get_procs(procs, 32);
    if (count < 0)
      count = 0;
    if (count > 32)
      count = 32; // Sicurezza per non sforare l'array

    int buf_idx = 0;

    // 1. Comandi ANSI di pulizia all'inizio del buffer unico
    screen_buffer[buf_idx++] = '\033';
    screen_buffer[buf_idx++] = '[';
    screen_buffer[buf_idx++] = 'H';
    screen_buffer[buf_idx++] = '\033';
    screen_buffer[buf_idx++] = '[';
    screen_buffer[buf_idx++] = 'J';

    // 2. Intestazione Gialla
    char col_yellow[] = "\033[1;33m";
    for (int k = 0; col_yellow[k]; k++)
      screen_buffer[buf_idx++] = col_yellow[k];

    char header[] = "PID  NAME         STATE    PRIO CPU\n";
    for (int k = 0; header[k]; k++)
      screen_buffer[buf_idx++] = header[k];

    char reset_and_sep[] = "\033[0m------------------------------------\n";
    for (int k = 0; reset_and_sep[k]; k++)
      screen_buffer[buf_idx++] = reset_and_sep[k];

    // 3. Costruzione di TUTTE le righe attive (fino a un massimo di 32)
    for (int i = 0; i < count; i++) {

      if (procs[i].state == 2) { // RUNNING (Verde)
        char c[] = "\033[92m";
        for (int k = 0; c[k]; k++)
          screen_buffer[buf_idx++] = c[k];
      } else if (procs[i].state == 3) { // SLEEPING (Grigio)
        char c[] = "\033[90m";
        for (int k = 0; c[k]; k++)
          screen_buffer[buf_idx++] = c[k];
      }

      // PID
      char num_buf[16];
      mini_itoa(num_buf, procs[i].pid);
      buf_idx += copy_str_fixed(&screen_buffer[buf_idx], num_buf, 4);
      screen_buffer[buf_idx++] = ' ';

      // NAME
      buf_idx += copy_str_fixed(&screen_buffer[buf_idx], procs[i].name, 12);
      screen_buffer[buf_idx++] = ' ';

      // STATE
      const char *state_str = "UNKNOWN";
      switch (procs[i].state) {
      case 1:
        state_str = "CREATED";
        break;
      case 2:
        state_str = "RUNNING";
        break;
      case 3:
        state_str = "SLEEPING";
        break;
      case 4:
        state_str = "ZOMBIE";
        break;
      case 5:
        state_str = "DEAD";
        break;
      case 6:
        state_str = "READY";
        break;
      }
      buf_idx += copy_str_fixed(&screen_buffer[buf_idx], state_str, 8);
      screen_buffer[buf_idx++] = ' ';

      // PRIO
      mini_itoa(num_buf, procs[i].priority);
      buf_idx += copy_str_fixed(&screen_buffer[buf_idx], num_buf, 4);
      screen_buffer[buf_idx++] = ' ';

      // CPU
      mini_itoa(num_buf, procs[i].on_cpu);
      buf_idx += copy_str_fixed(&screen_buffer[buf_idx], num_buf, 3);

      // Reset colore a fine riga ed andiamo a capo
      char rst[] = "\033[0m\n";
      for (int k = 0; rst[k]; k++)
        screen_buffer[buf_idx++] = rst[k];
    }

    /* 4. UNICA EMISSIONE ATOMICA DI TUTTA LA LISTA */
    _sys_window_write(my_win, screen_buffer, buf_idx);

    /* 5. REFRESH RATE (1Hz): block for one second via the real kernel timer
     * instead of busy-spinning yield(), so top no longer burns a core idling. */
    sleep(1000);
  }

  return 0;
}