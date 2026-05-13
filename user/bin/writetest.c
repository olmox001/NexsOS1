#include <os1.h>

#define WIN_W 300
#define WIN_H 250
#ifndef BUFFER_SIZE
#define BUFFER_SIZE (WIN_W * WIN_H)
#endif /* BUFFER_SIZE */

int main(void) {
  int win_id = create_window(100, 100, 400, 300, "Write Test");
  if (win_id < 0) {
    return 1;
  }

  printf_win(win_id, "Testing File Write...\n");
  printf("Testing File Write...\n");

  const char *path = "init.cfg";
  const char *data = "\n# Written by writetest\nshell.theme=dark\n";

  printf_win(win_id, "Writing to %s...\n", path);
  printf("Writing to %s...\n", path);
  printf_win(win_id, "Data len: %d\n", strlen(data));

  int bytes = file_write(path, data, strlen(data), 0);

  if (bytes < 0) {
    printf_win(win_id, "Write FAILED: %d\n", bytes);
    printf("Write FAILED: %d\n", bytes);
    return 1;
  } else {
    printf_win(win_id, "Write SUCCESS: %d bytes\n", bytes);
    printf_win(win_id, "Please reboot and check content.\n");
    printf("Write SUCCESS: %d bytes\n", bytes);
  }

  return 0;
}
