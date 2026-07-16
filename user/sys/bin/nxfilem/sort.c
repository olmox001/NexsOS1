/*
 * user/sys/bin/nxfilem/sort.c
 * Comparators + an in-place, dir-first Hoare-partition quicksort.
 */
#include "nxfilem.h"

int fm_sort_by_name(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;

  const char *pa = a->name;
  const char *pb = b->name;
  while (*pa && *pb) {
    int ca = *pa, cb = *pb;
    if (ca >= 'a' && ca <= 'z')
      ca -= 32;
    if (cb >= 'a' && cb <= 'z')
      cb -= 32;
    if (ca != cb)
      return ca < cb ? -1 : 1;
    pa++;
    pb++;
  }
  if (*pa)
    return 1;
  if (*pb)
    return -1;
  return 0;
}

int fm_sort_by_size(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;
  if (a->size != b->size)
    return a->size < b->size ? -1 : 1;
  return fm_sort_by_name(a, b);
}

int fm_sort_by_date(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;
  if (a->mtime != b->mtime)
    return a->mtime < b->mtime ? -1 : 1;
  return fm_sort_by_name(a, b);
}

int fm_sort_by_type(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;

  const char *ea = strrchr(a->name, '.');
  const char *eb = strrchr(b->name, '.');
  ea = ea ? ea : "";
  eb = eb ? eb : "";
  int c = strcmp(ea, eb);
  return c != 0 ? c : fm_sort_by_name(a, b);
}

/*
 * fm_qsort - Hoare-partition quicksort with a median-of-three pivot
 * (bounds worst-case behaviour on already-sorted directory listings, which
 * are common). Strict '<'/'>' comparisons in the partition loop mean equal
 * elements never block the cursors, so this terminates correctly even with
 * many same-named-prefix entries.
 */
void fm_qsort(fm_file_t *arr, int n,
             int (*cmp)(const fm_file_t *, const fm_file_t *)) {
  if (n <= 1)
    return;

  int mid = n / 2;
  if (cmp(&arr[0], &arr[mid]) > 0) {
    fm_file_t t = arr[0];
    arr[0] = arr[mid];
    arr[mid] = t;
  }
  if (cmp(&arr[0], &arr[n - 1]) > 0) {
    fm_file_t t = arr[0];
    arr[0] = arr[n - 1];
    arr[n - 1] = t;
  }
  if (cmp(&arr[mid], &arr[n - 1]) > 0) {
    fm_file_t t = arr[mid];
    arr[mid] = arr[n - 1];
    arr[n - 1] = t;
  }
  if (n <= 3)
    return;

  fm_file_t pv = arr[mid];
  int l = 0, r = n - 1;
  while (l <= r) {
    while (cmp(&arr[l], &pv) < 0)
      l++;
    while (cmp(&arr[r], &pv) > 0)
      r--;
    if (l <= r) {
      fm_file_t tmp = arr[l];
      arr[l] = arr[r];
      arr[r] = tmp;
      l++;
      r--;
    }
  }

  if (r > 0)
    fm_qsort(arr, r + 1, cmp);
  if (l < n - 1)
    fm_qsort(&arr[l], n - l, cmp);
}
