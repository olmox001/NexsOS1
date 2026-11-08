#include <hal/core.h>
#include <hal/diagnostic_protocol.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

static struct nxhal_diagnostic_protocol_v1 diagnostic_protocol;
static volatile uint32_t diagnostic_bound;

static void nxhal_diagnostic_unbound(void) __attribute__((noreturn));
static void nxhal_diagnostic_unbound(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

int nxhal_diagnostic_bind(const struct nxhal_diagnostic_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_DIAGNOSTIC_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_DIAGNOSTIC_PROTOCOL_ABI_VERSION)
    return NXHAL_DIAGNOSTIC_BIND_E_VERSION;
  if (protocol->struct_size < sizeof(struct nxhal_diagnostic_protocol_v1))
    return NXHAL_DIAGNOSTIC_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_DIAGNOSTIC_BIND_E_RESERVED;
  if (protocol->vlog == NULL || protocol->vfault == NULL ||
      protocol->vpanic == NULL)
    return NXHAL_DIAGNOSTIC_BIND_E_MISSING;
  if (!__sync_bool_compare_and_swap(&diagnostic_bound, 0U, 1U))
    return NXHAL_DIAGNOSTIC_BIND_E_STATE;

  diagnostic_protocol = *protocol;
  __sync_synchronize();
  diagnostic_bound = 2U;
  return NXHAL_DIAGNOSTIC_BIND_OK;
}

int nxhal_diagnostic_is_bound(void) {
  return diagnostic_bound == 2U;
}

static int nxhal_diag_vlog(uint32_t level, const char *fmt, va_list args) {
  if (!nxhal_diagnostic_is_bound())
    nxhal_diagnostic_unbound();
  return diagnostic_protocol.vlog(level, fmt, args);
}

int nxhal_diag_log(uint32_t level, const char *fmt, ...) {
  va_list args;
  int result;

  va_start(args, fmt);
  result = nxhal_diag_vlog(level, fmt, args);
  va_end(args);
  return result;
}

#define NXHAL_DEFINE_LOG_WRAPPER(name, level_value) \
  int name(const char *fmt, ...) {                    \
    va_list args;                                     \
    int result;                                       \
    va_start(args, fmt);                              \
    result = nxhal_diag_vlog(level_value, fmt, args); \
    va_end(args);                                     \
    return result;                                    \
  }

NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_info, NXHAL_DIAG_INFO)
NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_warn, NXHAL_DIAG_WARNING)
NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_error, NXHAL_DIAG_ERROR)
NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_debug, NXHAL_DIAG_DEBUG)

void nxhal_diag_fault(const char *fmt, ...) {
  va_list args;

  if (!nxhal_diagnostic_is_bound())
    nxhal_diagnostic_unbound();
  va_start(args, fmt);
  diagnostic_protocol.vfault(fmt, args);
  va_end(args);
}

void nxhal_diag_panic(const char *fmt, ...) {
  va_list args;

  if (!nxhal_diagnostic_is_bound())
    nxhal_diagnostic_unbound();
  va_start(args, fmt);
  diagnostic_protocol.vpanic(fmt, args);
  va_end(args);
  nxhal_diagnostic_unbound();
}
