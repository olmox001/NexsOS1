/*
 * user/bin/capreg.c
 * Registry-as-object test (ASTRA §6.6: every registry node is a capability,
 * every op is a message).  Uses the OS1 native object ABI.
 *
 *   1. read a default key (system.hostname) through a READ regkey handle;
 *   2. create a R/W handle to our own key, write then read it back;
 *   3. rights enforced: write through a READ-only handle -> -EPERM;
 *   4. cap_query reports OBJ_TYPE_REGKEY;
 *   5. registry_enum lists keys (LIB-REG-04) and includes ours.
 * Tagged "[capreg]" on the serial console.
 */
#include <os1.h>
#include <string.h>

static int failures = 0;

static void check(int win, const char *name, int ok) {
  printf_win(win, "%s: %s\n", name, ok ? "PASS" : "FAIL");
  printf("[capreg] %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

/* substring search using only the available libc subset (strlen/strncmp) */
static int contains(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  for (const char *p = hay; *p; p++)
    if (strncmp(p, needle, nl) == 0)
      return 1;
  return 0;
}

int main(void) {
  int win = create_window(150, 150, 460, 320, "Cap Registry");
  char buf[256];
  int ok;
  printf("[capreg] start (registry as capability objects)\n");

  /* 1. read a kernel-seeded default key through a READ capability */
  int hr = (int)OS1low_handle_create(OS1_NS_REG, "system.hostname",
                                     OS1_RIGHT_READ, OBJ_TYPE_REGKEY);
  memset(buf, 0, sizeof(buf));
  ok = hr >= 0 && OS1_object_read(hr, buf, sizeof(buf)) > 0 &&
       strncmp(buf, "NeXs", 4) == 0;
  check(win, "read-default-key", ok);

  /* 2. create a R/W capability to our own key, write then read back */
  int hw = (int)OS1low_handle_create(OS1_NS_REG, "capreg.test",
                                     OS1_RIGHT_READ | OS1_RIGHT_WRITE,
                                     OBJ_TYPE_REGKEY);
  ok = hw >= 0 && OS1_object_write(hw, "hello42", 8) > 0;
  check(win, "write-key", ok);

  memset(buf, 0, sizeof(buf));
  ok = hw >= 0 && OS1_object_read(hw, buf, sizeof(buf)) > 0 &&
       strncmp(buf, "hello42", 7) == 0;
  check(win, "read-back", ok);

  /* 3. rights enforced: a READ-only handle cannot write */
  ok = OS1_object_write(hr, "x", 2) == -EPERM;
  check(win, "deny-write-readonly", ok);

  /* 4. cap_query reports the object type */
  long q = OS1low_cap_query(hw);
  ok = q >= 0 && OS1_CAPQ_TYPE(q) == OBJ_TYPE_REGKEY;
  check(win, "cap_query-regkey", ok);

  /* 5. enumerate keys (LIB-REG-04): our key must appear */
  memset(buf, 0, sizeof(buf));
  int en = registry_enum(buf, sizeof(buf));
  ok = en > 0 && contains(buf, "capreg.test") && contains(buf, "system.hostname");
  check(win, "registry-enum", ok);

  if (hr >= 0)
    OS1low_handle_close(hr);
  if (hw >= 0)
    OS1low_handle_close(hw);

  printf("[capreg] done: %d failure(s)\n", failures);
  printf_win(win, "done: %d failure(s)\n", failures);
  for (int i = 0; i < 150; i++)
    yield();
  return failures ? 1 : 0;
}
