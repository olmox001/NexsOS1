#ifndef _KERNEL_HAL_UACCESS_H
#define _KERNEL_HAL_UACCESS_H

#include <kernel/nx_contract.h>
#include <kernel/types.h>

int hal_copy_from_user(void *dest, const void *src, size_t size) NX_MUST_USE;
int hal_copy_to_user(void *dest, const void *src, size_t size) NX_MUST_USE;
int hal_copy_string_from_user_n(char *dest, const char *src, size_t max_len,
                                size_t *out_len,
                                int *out_truncated) NX_MUST_USE;
int hal_copy_string_from_user(char *dest, const char *src,
                              size_t max_len) NX_MUST_USE;

#ifndef E2BIG
#define E2BIG 7
#endif

static inline int hal_copy_string_from_user_strict(char *dest,
                                                   const char *src,
                                                   size_t max_len) {
  int truncated = 0;
  int result = hal_copy_string_from_user_n(dest, src, max_len, (size_t *)0,
                                           &truncated);
  if (result != 0)
    return result;
  return truncated ? -E2BIG : 0;
}

#endif /* _KERNEL_HAL_UACCESS_H */
