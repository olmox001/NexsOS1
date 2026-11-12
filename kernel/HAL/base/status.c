#include "nxolmo/base/status.h"

const char *nx_status_name(nx_status status) {
    switch (status) {
    case NX_STATUS_OK: return "OK";
    case NX_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case NX_STATUS_TRUNCATED: return "TRUNCATED";
    case NX_STATUS_MALFORMED: return "MALFORMED";
    case NX_STATUS_LIMIT_EXCEEDED: return "LIMIT_EXCEEDED";
    case NX_STATUS_NO_SPACE: return "NO_SPACE";
    case NX_STATUS_OVERFLOW: return "OVERFLOW";
    case NX_STATUS_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case NX_STATUS_NOT_FOUND: return "NOT_FOUND";
    case NX_STATUS_STALE_REFERENCE: return "STALE_REFERENCE";
    case NX_STATUS_CONFLICT: return "CONFLICT";
    case NX_STATUS_ACCESS_DENIED: return "ACCESS_DENIED";
    case NX_STATUS_BUSY: return "BUSY";
    case NX_STATUS_RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case NX_STATUS_INTEGRITY_FAILURE: return "INTEGRITY_FAILURE";
    case NX_STATUS_CORRUPT: return "CORRUPT";
    case NX_STATUS_CANCELLED: return "CANCELLED";
    case NX_STATUS_TIMEOUT: return "TIMEOUT";
    case NX_STATUS_ALREADY_COMMITTED: return "ALREADY_COMMITTED";
    case NX_STATUS_IO_ERROR: return "IO_ERROR";
    case NX_STATUS_RETRY: return "RETRY";
    case NX_STATUS_INTERNAL: return "INTERNAL";
    case NX_STATUS_BUDGET_EXHAUSTED: return "BUDGET_EXHAUSTED";
    case NX_STATUS_VERSION_MISMATCH: return "VERSION_MISMATCH";
    case NX_STATUS_UNAVAILABLE: return "UNAVAILABLE";
    default: return "UNKNOWN_STATUS";
    }
}
