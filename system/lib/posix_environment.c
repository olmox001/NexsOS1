/*
 * NX-SDK POSIX environment view.
 *
 * The registry remains authoritative for OS1_env_get/set/unset. POSIX tools
 * which enumerate environ receive a bounded process-local snapshot built
 * at ELF entry, with machine defaults shadowed by per-process variables.
 * There is no duplicate environment service, PID routing, or new syscall.
 *
 * A failed/oversized snapshot reports an error: never produce an apparently
 * valid but silently truncated environ. Each process owns its own storage.
 */
#include <errno.h>
#include <os1.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define OS1_ENV_VIEW_MAX 64U
#define OS1_ENV_VIEW_KEY_MAX 96U
#define OS1_ENV_VIEW_VAL_MAX 128U
#define OS1_ENV_VIEW_RECORD_MAX (OS1_ENV_VIEW_KEY_MAX + OS1_ENV_VIEW_VAL_MAX + 1U)
#define OS1_ENV_VIEW_KEYS_MAX 4096U

struct os1_env_view {
  char record[OS1_ENV_VIEW_MAX][OS1_ENV_VIEW_RECORD_MAX];
  char *entry[OS1_ENV_VIEW_MAX + 1U];
  unsigned used;
};

static struct os1_env_view *os1_env_view_storage(void) {
  static struct os1_env_view owner;
  return &owner;
}

/* POSIX ABI pointer; content is a view, not a separate authority. */
static char *os1_empty_environment[] = {NULL};
char **environ = os1_empty_environment;

static int os1_env_view_add(struct os1_env_view *view, const char *name) {
  char value[OS1_ENV_VIEW_VAL_MAX];
  if (name == NULL || *name == '\0' || strchr(name, '=') != NULL)
    return -EINVAL;
  if (OS1_env_get(name, value, sizeof(value)) != 0)
    return 0;
  size_t name_len = strlen(name);
  if (name_len >= OS1_ENV_VIEW_KEY_MAX)
    return -EOVERFLOW;
  unsigned index = view->used;
  for (unsigned i = 0U; i < OS1_ENV_VIEW_MAX; ++i) {
    if (i >= view->used)
      break;
    if (strncmp(view->record[i], name, name_len) == 0 &&
        view->record[i][name_len] == '=') {
      index = i;
      break;
    }
  }
  if (index >= OS1_ENV_VIEW_MAX)
    return -EOVERFLOW;
  int n = snprintf(view->record[index], sizeof(view->record[index]),
                   "%s=%s", name, value);
  if (n < 0 || (size_t)n >= sizeof(view->record[index]))
    return -EOVERFLOW;
  view->entry[index] = view->record[index];
  if (index == view->used)
    ++view->used;
  view->entry[view->used] = NULL;
  return 0;
}

static int os1_env_view_scan(struct os1_env_view *view,
                             char *keys, int bytes, const char *prefix) {
  if (bytes < 0 || bytes >= (int)OS1_ENV_VIEW_KEYS_MAX)
    return -EOVERFLOW;
  keys[bytes] = '\0';
  const size_t plen = prefix == NULL ? 0U : strlen(prefix);
  size_t start = 0U;
  for (size_t i = 0U; i <= (size_t)bytes; ++i) {
    if (keys[i] != '\n' && keys[i] != '\0')
      continue;
    keys[i] = '\0';
    if (i > start) {
      const char *name = keys + start;
      if (prefix != NULL) {
        if (strncmp(name, prefix, plen) != 0)
          return -EINVAL;
        name += plen;
      }
      int rc = os1_env_view_add(view, name);
      if (rc != 0)
        return rc;
    }
    start = i + 1U;
  }
  return 0;
}

/* Called from the shared architecture-independent SDK ELF entry protocol.
 * No runtime allocations and no hidden fallback to a stale environment. */
int os1_posix_environ_init(void) {
  struct os1_env_view *owner = os1_env_view_storage();
  char keys[OS1_ENV_VIEW_KEYS_MAX];
  owner->used = 0U;
  owner->entry[0] = NULL;
  int n = OS1_registry_enum_under("sys.env.", keys, sizeof(keys));
  if (n < 0 || os1_env_view_scan(owner, keys, n, "sys.env.") != 0)
    return -EOVERFLOW;
  n = OS1_env_enum(keys, sizeof(keys));
  if (n < 0 || os1_env_view_scan(owner, keys, n, NULL) != 0)
    return -EOVERFLOW;
  environ = owner->entry;
  return 0;
}
