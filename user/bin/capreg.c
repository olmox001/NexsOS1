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
  int en = OS1_registry_enum(buf, sizeof(buf));
  ok = en > 0 && contains(buf, "capreg.test") && contains(buf, "system.hostname");
  check(win, "registry-enum", ok);

  /* 6. namespace enumeration under a prefix (Phase 4.1 A1a): only matching keys
   * are listed; out-of-namespace keys (system.hostname) must NOT appear. */
  OS1_registry_set("ns4test.alpha", "1");
  OS1_registry_set("ns4test.beta", "2");
  memset(buf, 0, sizeof(buf));
  int un = OS1_registry_enum_under("ns4test.", buf, sizeof(buf));
  ok = un > 0 && contains(buf, "ns4test.alpha") && contains(buf, "ns4test.beta") &&
       !contains(buf, "system.hostname");
  check(win, "registry-enum-under", ok);

  /* 7. deletion (Phase 4.1 A-gap1): delete one key — it disappears (get fails,
   * enum omits it) while the sibling survives. */
  ok = OS1_registry_del("ns4test.alpha") == 0;
  if (ok) {
    char tmp[8];
    ok = OS1_registry_get("ns4test.alpha", tmp, sizeof(tmp)) != 0; /* now absent */
  }
  if (ok) {
    memset(buf, 0, sizeof(buf));
    OS1_registry_enum_under("ns4test.", buf, sizeof(buf));
    ok = !contains(buf, "ns4test.alpha") && contains(buf, "ns4test.beta");
  }
  check(win, "registry-del", ok);

  /* 8. VFS unlink (Phase 4.1 A-gap1): remove a /reg key via the file path. */
  OS1_registry_set("ns4test.gamma", "3");
  ok = OS1_fs_unlink("/reg/ns4test/gamma") == 0;
  if (ok) {
    char tmp[8];
    ok = OS1_registry_get("ns4test.gamma", tmp, sizeof(tmp)) != 0; /* gone */
  }
  check(win, "vfs-unlink-/reg", ok);

  /* 9. VFS write to /reg (Phase 4.1 A-gap2): write a new key via the file path,
   * then overlay at an offset; both reflect in the registry. */
  ok = OS1_fs_write("/reg/ns4test/delta", "abc", 3, 0) == 3;
  if (ok) {
    char v[8];
    memset(v, 0, sizeof(v));
    ok = OS1_registry_get("ns4test.delta", v, sizeof(v)) == 0 &&
         strncmp(v, "abc", 3) == 0;
  }
  if (ok)
    ok = OS1_fs_write("/reg/ns4test/delta", "XY", 2, 1) == 2; /* overlay -> aXY */
  if (ok) {
    char v[8];
    memset(v, 0, sizeof(v));
    ok = OS1_registry_get("ns4test.delta", v, sizeof(v)) == 0 &&
         strncmp(v, "aXY", 3) == 0;
  }
  check(win, "vfs-write-/reg", ok);

  if (hr >= 0)
    OS1low_handle_close(hr);
  if (hw >= 0)
    OS1low_handle_close(hw);

  /* Clean up the keys we created this run (we own them) so re-running the test
   * starts from a clean registry — first-writer-wins would otherwise reject the
   * second run's writes to keys owned by this now-dead process. */
  OS1_registry_del("capreg.test");
  OS1_registry_del("ns4test.beta");
  OS1_registry_del("ns4test.delta");

  printf("[capreg] done: %d failure(s)\n", failures);
  printf_win(win, "done: %d failure(s)\n", failures);
  for (int i = 0; i < 150; i++)
    yield();
  return failures ? 1 : 0;
}
