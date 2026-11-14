/*
 * user/sys/bin/nxnotify.c
 * Notification CLI client (the textual front-end to nxntfy_srv).
 *
 * Usage:
 *   nxnotify <message...>       send a notification
 *   nxnotify send <message...>  same, explicit verb
 *   nxnotify list               list the recent notifications (registry log)
 *
 * Thin CLI over OS1_notify_post (IPC to nxntfy_srv) and the sys.ntfy.log.*
 * registry ring that nxntfy_srv maintains — no policy of its own, so its reach
 * is exactly the caller's (the kernel gates IPC + the registry per caller).
 * Replaces the old 'notify' shell builtin, like nxproc/nxwins/nxres.
 */
#include <os1.h>

/* cmd_send - join argv[from..] into one message and post it. */
static int cmd_send(int argc, char **argv, int from, int sev) {
  char msg[128];
  int n = 0;
  for (int i = from; i < argc && n < (int)sizeof(msg) - 1; i++) {
    for (const char *p = argv[i]; *p && n < (int)sizeof(msg) - 1; p++)
      msg[n++] = *p;
    if (i + 1 < argc && n < (int)sizeof(msg) - 1)
      msg[n++] = ' ';
  }
  msg[n] = '\0';
  if (n == 0) {
    printf("usage: nxnotify [send|warn] <message>\n");
    return 1;
  }
  int r = sev ? OS1_notify_warn("nxnotify", msg) : OS1_notify_post("nxnotify", msg);
  if (r == 0) {
    printf("nxnotify: sent\n");
    return 0;
  }
  printf("nxnotify: failed (%d) - is nxntfy_srv up?\n", r);
  return 1;
}

/* cmd_list - print the values of the sys.ntfy.log.* registry ring. */
/* one parsed record from the sys.ntfy.log.* ring: "<from>|<sev>|<state>|<text>" */
struct nrec {
  int from;
  int sev;
  char state; /* 'U' unread / received, 'R' read */
  char text[56];
};

static int cmd_list(void) {
  char keys[640];
  int n = OS1_registry_enum_under("sys.ntfy.log.", keys, sizeof(keys));
  if (n <= 0) {
    printf("nxnotify: no notifications\n");
    return 0;
  }
  struct nrec recs[16];
  int nrec = 0;
  char key[64];
  int k = 0;
  for (const char *p = keys; nrec < 16; p++) {
    if (*p == '\n' || *p == '\0') {
      if (k > 0) {
        key[k] = '\0';
        char val[80];
        if (OS1_registry_get(key, val, sizeof(val)) == 0) {
          struct nrec *r = &recs[nrec];
          const char *q = val;
          r->from = atoi(q);
          while (*q && *q != '|') q++;
          if (*q) q++;
          r->sev = atoi(q);
          while (*q && *q != '|') q++;
          if (*q) q++;
          r->state = *q ? *q : 'U';
          while (*q && *q != '|') q++;
          if (*q) q++;
          int t = 0;
          for (; *q && t < 55; q++) r->text[t++] = *q;
          r->text[t] = '\0';
          nrec++;
        }
      }
      k = 0;
      if (*p == '\0') break;
    } else if (k < (int)sizeof(key) - 1) {
      key[k++] = *p;
    }
  }
  if (nrec == 0) {
    printf("nxnotify: no notifications\n");
    return 0;
  }
  /* Group by sender PID (the per-process distinction). */
  printf("\033[1;33mNotifications (grouped by process):\033[0m\n");
  char done[16] = {0};
  for (int i = 0; i < nrec; i++) {
    if (done[i]) continue;
    printf("\033[1mPID %d:\033[0m\n", recs[i].from);
    for (int j = i; j < nrec; j++) {
      if (recs[j].from != recs[i].from) continue;
      done[j] = 1;
      const char *sv = recs[j].sev == 2 ? "ERR " : recs[j].sev == 1 ? "WARN" : "info";
      const char *st = recs[j].state == 'R' ? "read  " : "unread";
      printf("  [%s][%s] %s\n", sv, st, recs[j].text);
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: nxnotify <msg> | nxnotify send|warn <msg> | nxnotify list\n");
    return 1;
  }
  if (strncmp(argv[1], "list", 5) == 0)
    return cmd_list();
  if (strncmp(argv[1], "send", 5) == 0)
    return cmd_send(argc, argv, 2, 0);
  if (strncmp(argv[1], "warn", 5) == 0)
    return cmd_send(argc, argv, 2, 1); /* yellow warning */
  return cmd_send(argc, argv, 1, 0); /* nxnotify <msg...> */
}
