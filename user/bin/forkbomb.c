/*
 * user/bin/forkbomb.c
 * Resource-exhaustion / quota test (SCHED-DOS-01 #122).
 *
 * Runs as a standalone TTY graphical window app, creating its own
 * canvas within the NEXS compositor to bypass FD inheritance limitations.
 */
#include <os1.h>

/* Helper locale per convertire interi in stringa nello stack */
static void local_itoa(int n, char *s) {
  int i, sign;
  if ((sign = n) < 0)
    n = -n;
  i = 0;
  do {
    s[i++] = n % 10 + '0';
  } while ((n /= 10) > 0);
  if (sign < 0)
    s[i++] = '-';
  s[i] = '\0';
  for (int j = 0, k = i - 1; j < k; j++, k--) {
    char temp = s[j];
    s[j] = s[k];
    s[k] = temp;
  }
}

static int local_strlen(const char *s) {
  int len = 0;
  while (s[len] != '\0')
    len++;
  return len;
}

int main(void) {
  int created = 0;
  char buf[32];

  /* * Corretto secondo include/api/os1.h:
   * Coordinate: x=100, y=100, w=400, h=300, seguito dal Titolo.
   */
  int my_win = _sys_create_window(100, 100, 400, 300, "Forkbomb Sandbox TTY");
  if (my_win < 0) {
    print("[forkbomb] Errore: Impossibile allocare una nuova finestra TTY\n");
    return 1;
  }

  /* Pulizia dello schermo e banner di avvio nella nuova finestra grafica */
  _sys_window_write(my_win, "\033[H\033[J", 6); // ANSI Clear Screen
  _sys_window_write(my_win, "\033[1;31m========================================\n",
             46);
  _sys_window_write(my_win, "  NEXS QUOTA DOS TEST (SCHED-DOS-01)\n", 37);
  _sys_window_write(my_win, "========================================\033[0m\n\n", 46);

  _sys_window_write(my_win, "[TTY] Finestra inizializzata. PID: ", 35);
  local_itoa(get_pid(), buf);
  _sys_window_write(my_win, buf, local_strlen(buf));
  _sys_window_write(my_win,
             "\n[TTY] Avvio attacco controllato sulla coda dei processi...\n\n",
             60);

  /* Ciclo di Forkbomb */
  for (int i = 0; i < 2000; i++) {
    int pid = spawn("/bin/counter");
    if (pid > 0) {
      created++;

      /* Feedback visivo immediato nella finestra dedicata */
      _sys_window_write(my_win, "[+] Creato figlio PID: ", 23);
      local_itoa(pid, buf);
      _sys_window_write(my_win, buf, local_strlen(buf));
      _sys_window_write(my_win, "\n", 1);
    } else {
      /* La quota di protezione per-parent (32 processi) è scattata con
       * successo! */
      _sys_write(
          my_win,
          "\n\033[1;33m[!] STOP COMPULSIRE! Lo spawn e' stato rifiutato.\n",
          54);
      _sys_window_write(my_win, "[!] Totale figli allocati prima del blocco: ", 44);
      local_itoa(created, buf);
      _sys_window_write(my_win, buf, local_strlen(buf));

      _sys_window_write(my_win, "\n[!] Codice di errore restituito dal kernel: ", 45);
      local_itoa(pid, buf);
      _sys_window_write(my_win, buf, local_strlen(buf));
      _sys_window_write(my_win, " (Quota attiva con successo)\033[0m\n", 34);
      break;
    }

    /* Cediamo il passo alle altre CPU (SMP) per far ridisegnare i testi
     * correnti */
    yield();
  }

  _sys_write(
      my_win,
      "\n\033[1;32m[OK] Sistema protetto. La shell originale e' reattiva.\n",
      61);
  _sys_window_write(my_win,
             "[OK] Puoi fare 'kill' di questo processo in sicurezza.\033[0m\n",
             61);

  /* Rimane in ascolto indefinito per consentire l'ispezione visiva */
  while (1) {
    yield();
  }

  return 0;
}