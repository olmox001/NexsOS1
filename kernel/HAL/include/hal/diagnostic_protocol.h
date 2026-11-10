#ifndef NXHAL_DIAGNOSTIC_PROTOCOL_H
#define NXHAL_DIAGNOSTIC_PROTOCOL_H

#include <stdarg.h>
#include <stdint.h>

#define NXHAL_DIAGNOSTIC_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_diag_level {
  NXHAL_DIAG_EMERG = 0,
  NXHAL_DIAG_ALERT = 1,
  NXHAL_DIAG_CRIT = 2,
  NXHAL_DIAG_ERROR = 3,
  NXHAL_DIAG_WARNING = 4,
  NXHAL_DIAG_NOTICE = 5,
  NXHAL_DIAG_INFO = 6,
  NXHAL_DIAG_DEBUG = 7
};

enum nxhal_diagnostic_bind_status {
  NXHAL_DIAGNOSTIC_BIND_OK = 0,
  NXHAL_DIAGNOSTIC_BIND_E_NULL = -1,
  NXHAL_DIAGNOSTIC_BIND_E_VERSION = -2,
  NXHAL_DIAGNOSTIC_BIND_E_SIZE = -3,
  NXHAL_DIAGNOSTIC_BIND_E_RESERVED = -4,
  NXHAL_DIAGNOSTIC_BIND_E_MISSING = -5,
  NXHAL_DIAGNOSTIC_BIND_E_STATE = -6
};

struct nxhal_diagnostic_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;
  int (*vlog)(uint32_t level, const char *fmt, va_list args);
};

int nxhal_diagnostic_bind(const struct nxhal_diagnostic_protocol_v1 *protocol);
int nxhal_diagnostic_is_bound(void);

int nxhal_diag_log(uint32_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int nxhal_diag_info(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
int nxhal_diag_warn(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
int nxhal_diag_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
int nxhal_diag_debug(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* NXHAL_DIAGNOSTIC_PROTOCOL_H */
