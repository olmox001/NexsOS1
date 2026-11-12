#include <hal/core.h>

/* Privileged I/O is tested in QEMU, never executed in host test mode. */
static int test_transport_present(void) {
  return hal_ioport_available() == 1 ? 0 : 1;
}

static int test_transport_stable(void) {
  return hal_ioport_available() == 1 ? 0 : 1;
}

int main(void) {
  int ret = test_transport_present();
  if (ret != 0) return ret;
  return test_transport_stable();
}
