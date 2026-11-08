#include <hal/diagnostic_protocol.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned log_calls;
static unsigned fault_calls;
static uint32_t last_level;
static char last_text[64];

void hal_irq_disable_all(void) {
  abort();
}

void hal_cpu_halt(void) {
  abort();
}

static int capture_text(const char *fmt, va_list args) {
  if (strcmp(fmt, "value=%u") != 0)
    return -1;
  unsigned value = va_arg(args, unsigned);
  last_text[0] = '0' + (char)(value % 10U);
  last_text[1] = '\0';
  return (int)value;
}

static int test_vlog(uint32_t level, const char *fmt, va_list args) {
  last_level = level;
  ++log_calls;
  return capture_text(fmt, args);
}

static void test_vfault(const char *fmt, va_list args) {
  ++fault_calls;
  (void)capture_text(fmt, args);
}

static void test_vpanic(const char *fmt, va_list args) {
  (void)fmt;
  (void)args;
  abort();
}

static struct nxhal_diagnostic_protocol_v1 valid_protocol(void) {
  struct nxhal_diagnostic_protocol_v1 protocol = {
      .abi_version = NXHAL_DIAGNOSTIC_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_diagnostic_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .vlog = test_vlog,
      .vfault = test_vfault,
      .vpanic = test_vpanic,
  };
  return protocol;
}

int main(void) {
  struct nxhal_diagnostic_protocol_v1 protocol = valid_protocol();

  if (nxhal_diagnostic_bind(NULL) != NXHAL_DIAGNOSTIC_BIND_E_NULL)
    return 1;
  protocol.abi_version = 0U;
  if (nxhal_diagnostic_bind(&protocol) != NXHAL_DIAGNOSTIC_BIND_E_VERSION)
    return 2;
  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_diagnostic_bind(&protocol) != NXHAL_DIAGNOSTIC_BIND_E_SIZE)
    return 3;
  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_diagnostic_bind(&protocol) != NXHAL_DIAGNOSTIC_BIND_E_RESERVED)
    return 4;
  protocol = valid_protocol();
  protocol.vlog = NULL;
  if (nxhal_diagnostic_bind(&protocol) != NXHAL_DIAGNOSTIC_BIND_E_MISSING)
    return 5;
  if (nxhal_diagnostic_is_bound())
    return 6;

  protocol = valid_protocol();
  if (nxhal_diagnostic_bind(&protocol) != NXHAL_DIAGNOSTIC_BIND_OK)
    return 7;
  if (!nxhal_diagnostic_is_bound())
    return 8;
  if (nxhal_diagnostic_bind(&protocol) != NXHAL_DIAGNOSTIC_BIND_E_STATE)
    return 9;

  if (nxhal_diag_info("value=%u", 7U) != 7)
    return 10;
  if (log_calls != 1U || last_level != NXHAL_DIAG_INFO ||
      strcmp(last_text, "7") != 0)
    return 11;

  if (nxhal_diag_warn("value=%u", 3U) != 3)
    return 12;
  if (log_calls != 2U || last_level != NXHAL_DIAG_WARNING)
    return 13;

  nxhal_diag_fault("value=%u", 5U);
  if (fault_calls != 1U || strcmp(last_text, "5") != 0)
    return 14;

  return 0;
}
