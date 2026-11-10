#include <hal/core.h>
#include <hal/diagnostic_protocol.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

static struct nxhal_diagnostic_protocol_v1 diagnostic_protocol;
enum nxhal_diagnostic_state {
  NXHAL_DIAGNOSTIC_UNBOUND = 0,
  NXHAL_DIAGNOSTIC_BINDING = 1,
  NXHAL_DIAGNOSTIC_BOUND = 2
};

static uint32_t diagnostic_bound;

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
  if (protocol->vlog == NULL)
    return NXHAL_DIAGNOSTIC_BIND_E_MISSING;
  uint32_t expected = NXHAL_DIAGNOSTIC_UNBOUND;
  if (!__atomic_compare_exchange_n(&diagnostic_bound, &expected,
                                   NXHAL_DIAGNOSTIC_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_DIAGNOSTIC_BIND_E_STATE;

  diagnostic_protocol = *protocol;
  __atomic_store_n(&diagnostic_bound, NXHAL_DIAGNOSTIC_BOUND, __ATOMIC_RELEASE);
  return NXHAL_DIAGNOSTIC_BIND_OK;
}

int nxhal_diagnostic_is_bound(void) {
  return __atomic_load_n(&diagnostic_bound, __ATOMIC_ACQUIRE) ==
         NXHAL_DIAGNOSTIC_BOUND;
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

#define NXHAL_DEFINE_LOG_WRAPPER(name, level_value)                            \
  int name(const char *fmt, ...) {                                             \
    va_list args;                                                              \
    int result;                                                                \
    va_start(args, fmt);                                                       \
    result = nxhal_diag_vlog(level_value, fmt, args);                          \
    va_end(args);                                                              \
    return result;                                                             \
  }

NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_info, NXHAL_DIAG_INFO)
NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_warn, NXHAL_DIAG_WARNING)
NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_error, NXHAL_DIAG_ERROR)
NXHAL_DEFINE_LOG_WRAPPER(nxhal_diag_debug, NXHAL_DIAG_DEBUG)
