/*
 * NX notification transport acceptance. Real kernel-stamped PORT sender,
 * real user-mode broker, real bounded registry log (not a fake PASS).
 * The delivery test also verifies 3 distinct severities reach the broker.
 */
#include <os1.h>
#include <stdio.h>
#include <string.h>

enum {
  NX_NOTIFY_RING = 16,
  NX_NOTIFY_POLLS = 180,
  NX_NOTIFY_SLEEP_MS = 10
};

static int notify_seen(int self, const char *token, int severity) {
  char key[32], text[128], prefix[28];
  if (snprintf(prefix, sizeof(prefix), "%d|%d|", self, severity) < 0)
    return 0;
  for (unsigned slot = 0U; slot < NX_NOTIFY_RING; ++slot) {
    int k = snprintf(key, sizeof(key), "sys.ntfy.log.%u", slot);
    if (k <= 0 || k >= (int)sizeof(key))
      return 0;
    if (OS1_registry_get(key, text, sizeof(text)) != 0)
      continue;
    if (strncmp(text, prefix, strlen(prefix)) == 0 &&
        strstr(text, token) != NULL)
      return 1;
  }
  return 0;
}

static int notify_delivered(const char *token, int severity) {
  int self = get_pid();
  for (unsigned poll = 0U; poll < NX_NOTIFY_POLLS; ++poll) {
    if (notify_seen(self, token, severity))
      return 1;
    OS1_sleep(NX_NOTIFY_SLEEP_MS);
  }
  return 0;
}

/* Every real operation contributes to the fail count and to the mdebug UART
 * transcript. A successful API return alone is not delivery evidence. */
static int notify_check(const char *name, int condition) {
  printf_win(0, "[NOTIFYTEST][%s] %s\n",
             condition ? "PASS" : "FAIL", name);
  return condition ? 0 : 1;
}

int main(void) {
  int fails = 0;
  long receiver = OS1low_handle_create(
      OS1_NS_PORT, OS1NX_PORT_NOTIFY, OS1_RIGHT_READ, OBJ_TYPE_PORT);
  fails += notify_check("USER cannot claim service receiver",
                        receiver == -EPERM);
  if (receiver >= 0)
    fails += notify_check("unexpected receiver handle closes",
                          OS1low_handle_close((int)receiver) == 0);

  long send = OS1_port_open(OS1NX_PORT_NOTIFY);
  fails += notify_check("public service SEND capability acquired", send >= 0);
  if (send >= 0) {
    long rights = OS1low_cap_query((int)send);
    fails += notify_check("SEND type is PORT",
                          rights >= 0 && OS1_CAPQ_TYPE(rights) == OBJ_TYPE_PORT);
    fails += notify_check("SEND has WRITE only, no delegation rights",
                          rights >= 0 && OS1_CAPQ_RIGHTS(rights) == OS1_RIGHT_WRITE);
    fails += notify_check("SEND handle released",
                          OS1low_handle_close((int)send) == 0);
  }

  int info = OS1_notify_post("notifytest", "NXNOTIFY-INFO-E2E");
  int warn = OS1_notify_warn("notifytest", "NXNOTIFY-WARN-E2E");
  int error = OS1_notify_error("notifytest", "NXNOTIFY-ERROR-E2E");
  fails += notify_check("INFO accepted by actual PORT", info == 0);
  fails += notify_check("WARN accepted by actual PORT", warn == 0);
  fails += notify_check("ERROR accepted by actual PORT", error == 0);
  fails += notify_check("INFO stored by actual broker in registry",
                        notify_delivered("NXNOTIFY-INFO-E2E", 0));
  fails += notify_check("WARN stored by actual broker in registry",
                        notify_delivered("NXNOTIFY-WARN-E2E", 1));
  fails += notify_check("ERROR stored by actual broker in registry",
                        notify_delivered("NXNOTIFY-ERROR-E2E", 2));
  printf_win(0, "[notifytest] done: %d failure(s)\n", fails);
  if (printf("[notifytest] done: %d failure(s)\n", fails) < 0)
    return 1;
  return fails == 0 ? 0 : 1;
}
