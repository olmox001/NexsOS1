/*
 * nxroleprobe: real mdebug ROOT/MACHINE capability boundary regression.
 * A normal system program, NOT a daemon. The authoritative PID1 manager
 * launches this as a ROOT one-shot in mdebug; user shell cannot raise it.
 * All assertions use real object/registry/process operations and exit status.
 */
#include <nxmanage.h>
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nxrole_check(const char *label, int accepted) {
  /* UART-only diagnostic destination 0 is an existing OS1 SDK API;
   * no machine authority, debug RPC alias or new kernel mechanism. */
  printf_win(0, "[NXROLE][%s] %s\n",
             accepted ? "PASS" : "FAIL", label);
  int written = printf("[NXROLE][%s] %s\n",
                       accepted ? "PASS" : "FAIL", label);
  return accepted && written > 0 ? 0 : 1;
}

static int nxrole_registry(void) {
  int failed = 0;
  char value[64] = {0};
  int read = OS1_registry_object_get("system.manage.pid", value,
                                      sizeof(value));
  failed += nxrole_check("machine registry object visible",
                         read == 0 && strcmp(value, "1") == 0);
  long forbidden = OS1low_handle_create(
      OS1_NS_REG, "system.manage.pid", OS1_RIGHT_WRITE, OBJ_TYPE_REGKEY);
  failed += nxrole_check("ROOT REGKEY write acquisition denied",
                         forbidden == -EACCES);
  if (forbidden >= 0 && OS1low_handle_close((int)forbidden) < 0)
    return failed + 1;
  failed += nxrole_check("ROOT registry object mutation denied",
      OS1_registry_object_set("system.manage.pid", "31337") == -EACCES);
  failed += nxrole_check("ROOT ambient registry mutation denied",
      OS1_registry_set("system.manage.pid", "31337") == -EACCES);
  char service[64] = {0};
  int service_read = OS1_registry_object_get(
      "system.manage.service.exec", service, sizeof(service));
  failed += nxrole_check("named exec service REGKEY published",
      service_read == 0 && atoi(service) > 0);
  return failed;
}

static int nxrole_process(void) {
  int failed = 0;
  failed += nxrole_check("ROOT cannot spawn machine bootstrap extension",
      spawn("/krnl/extension/nxmanageDD") == -EINVAL);
  failed += nxrole_check("ROOT cannot spawn a second initializer",
      spawn("/sbin/nxinit") == -EINVAL);
  failed += nxrole_check("ROOT cannot request machine authority",
      spawn_caps("/krnl/extension/nxmanageDD",
                 PLVL_MACHINE, CAP_ALL) == -EINVAL);
  return failed;
}

static int nxrole_ports(void) {
  int failed = 0;
  long read = OS1low_handle_create(
      OS1_NS_PORT, NXMANAGE_PORT, OS1_RIGHT_READ, OBJ_TYPE_PORT);
  failed += nxrole_check("ROOT cannot create MACHINE receiver",
                         read == -EPERM);
  if (read >= 0 && OS1low_handle_close((int)read) < 0)
    return failed + 1;
  long send = OS1low_handle_create(
      OS1_NS_PORT, NXMANAGE_PORT, OS1_RIGHT_WRITE, OBJ_TYPE_PORT);
  failed += nxrole_check("ROOT cannot acquire private MACHINE send",
                         send == -EPERM);
  if (send >= 0 && OS1low_handle_close((int)send) < 0)
    return failed + 1;
  long private_port = OS1low_handle_create(
      OS1_NS_PORT, "OS1nx_machine_probetest",
      OS1_RIGHT_READ | OS1_PORT_CREATE_PRIVATE, OBJ_TYPE_PORT);
  failed += nxrole_check("ROOT cannot mint machine-only private port",
                         private_port == -EPERM);
  if (private_port >= 0 && OS1low_handle_close((int)private_port) < 0)
    return failed + 1;
  return failed;
}

int main(void) {
  int level = -1;
  unsigned int caps = 0U;
  int identity = OS1_identity(&level, &caps);
  int failed = nxrole_check("runtime identity is ROOT not MACHINE",
                            identity == 0 && level == PLVL_ROOT);
  failed += nxrole_registry();
  failed += nxrole_process();
  failed += nxrole_ports();
  if (failed == 0) {
    printf_win(0, "[NXROLE] all ROOT/MACHINE boundary tests passed\n");
    if (printf("[NXROLE] all ROOT/MACHINE boundary tests passed\n") < 0)
      return 1;
  } else {
    printf_win(0, "[NXROLE][ERROR] %d real boundary tests failed\n",
               failed);
    if (printf("[NXROLE][ERROR] %d actual ROOT/MACHINE tests failed\n",
               failed) < 0)
      return 1;
  }
  return failed == 0 ? 0 : 1;
}
