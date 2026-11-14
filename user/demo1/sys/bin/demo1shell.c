#include <nxexample_echo.h>
#include <os1.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { DEMO_LINE_MAX = 96, DEMO_SERVICE_COUNT = 9 };

static const char *const demo_services[DEMO_SERVICE_COUNT] = {
    "notify", "exec", "timer", "pax", "compositor",
    "keyboard", "pointer", "echo", "shell"};

static void demo_emit(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int printed = vprintf(format, args);
  va_end(args);
  if (printed < 0)
    OS1_report_error("demo1.print", -EIO);
}

static void demo_print_services(void) {
  char key[64];
  char record[64];
  for (unsigned i = 0U; i < DEMO_SERVICE_COUNT; ++i) {
    int n = snprintf(key, sizeof(key), "system.manage.service.%s",
                     demo_services[i]);
    if (n <= 0 || n >= (int)sizeof(key)) {
      demo_emit("%s: invalid key\n", demo_services[i]);
      continue;
    }
    record[0] = '\0';
    int rc = OS1_registry_get(key, record, sizeof(record));
    if (rc == 0)
      demo_emit("%s=%s\n", demo_services[i], record);
    else
      demo_emit("%s=unavailable (%d)\n", demo_services[i], rc);
  }
}

static void demo_command(char line[DEMO_LINE_MAX], int *running) {
  size_t len = strlen(line);
  if (len > 0U && line[len - 1U] == '\n')
    line[len - 1U] = '\0';
  if (strcmp(line, "help") == 0) {
    demo_emit("commands: help project init services echo-test notify-test exit\n");
  } else if (strcmp(line, "project") == 0) {
    char project[32] = {0};
    int rc = OS1_registry_get("system.manage.current", project, sizeof(project));
    demo_emit("project=%s rc=%d\n", rc == 0 ? project : "unknown", rc);
  } else if (strcmp(line, "init") == 0) {
    char init_name[32] = {0};
    int rc = OS1_registry_get("system.init.name", init_name, sizeof(init_name));
    demo_emit("init=%s rc=%d\n", rc == 0 ? init_name : "unknown", rc);
  } else if (strcmp(line, "services") == 0) {
    demo_print_services();
  } else if (strcmp(line, "echo-test") == 0) {
    char reply[32] = {0};
    int rc = NX_example_echo("DEMO1-ECHO", reply, sizeof(reply));
    demo_emit("echo=%s rc=%d\n", rc == 0 ? reply : "failed", rc);
  } else if (strcmp(line, "notify-test") == 0) {
    char last[64] = {0};
    int sent = OS1_notify_post("Demo1", "alternate-notify");
    OS1_sleep(50);
    int read = OS1_registry_get("example.notify.last", last, sizeof(last));
    demo_emit("notify=%s send=%d read=%d\n",
              read == 0 ? last : "unavailable", sent, read);
  } else if (strcmp(line, "exit") == 0) {
    *running = 0;
  } else if (line[0] != '\0') {
    demo_emit("demo1: unknown command: %s\n", line);
  }
}

int main(void) {
  char line[DEMO_LINE_MAX];
  int running = 1;
  demo_emit("Demo1 userland ready\n");
  for (unsigned command = 0U; command < 4096U && running != 0; ++command) {
    demo_emit("NXShell:demo1> ");
    if (fgets(line, sizeof(line), stdin) == NULL)
      return 1;
    demo_command(line, &running);
  }
  return running == 0 ? 0 : 2;
}
