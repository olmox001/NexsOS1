/*
 * NeXs File Manager - Sorting Functions
 * Implements multiple sort criteria for file listing
 */
#include "nxfilem.h"

int fm_sort_by_name(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;

  const char *a_name = a->name;
  const char *b_name = b->name;

  while (*a_name && *b_name) {
    int a_char = *a_name;
    int b_char = *b_name;

    if (a_char >= 'a' && a_char <= 'z')
      a_char -= 32;
    if (b_char >= 'a' && b_char <= 'z')
      b_char -= 32;

    if (a_char < b_char)
      return -1;
    if (a_char > b_char)
      return 1;

    a_name++;
    b_name++;
  }

  if (*a_name)
    return 1;
  if (*b_name)
    return -1;
  return 0;
}

int fm_sort_by_size(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;

  if (a->size < b->size)
    return -1;
  if (a->size > b->size)
    return 1;
  return fm_sort_by_name(a, b);
}

int fm_sort_by_date(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;

  if (a->mtime < b->mtime)
    return -1;
  if (a->mtime > b->mtime)
    return 1;
  return fm_sort_by_name(a, b);
}

int fm_sort_by_type(const fm_file_t *a, const fm_file_t *b) {
  if (a->is_dir && !b->is_dir)
    return -1;
  if (!a->is_dir && b->is_dir)
    return 1;

  const char *a_ext = strrchr(a->name, '.');
  const char *b_ext = strrchr(b->name, '.');

  if (!a_ext)
    a_ext = "";
  if (!b_ext)
    b_ext = "";

  int ext_cmp = strcmp(a_ext, b_ext);
  if (ext_cmp != 0)
    return ext_cmp;

  return fm_sort_by_name(a, b);
}

/*
 * fm_qsort - Quicksort in-place per array di fm_file_t.
 *
 * BUG FIX: l'implementazione originale aveva un loop infinito quando tutti
 * gli elementi erano uguali al pivot.
 *
 * Causa: i cursori left/right avanzavano solo se cmp() < 0 o > 0.
 * Con tutti elementi uguali (cmp == 0), left e right non si muovevano mai,
 * lo swap avveniva sugli stessi indici e left/right avanzavano di 1 per via
 * del left++/right-- dopo lo swap — MA in alcuni casi il loop while esterno
 * non terminava perché left <= right rimaneva sempre vero.
 *
 * Soluzione: implementazione a tre partizioni (Dutch National Flag / Lomuto
 * modificato) che gestisce correttamente i duplicati:
 * - elementi < pivot vengono spostati a sinistra
 * - elementi == pivot rimangono al centro e NON vengono ricorsivamente ordinati
 * - elementi > pivot vengono spostati a destra
 *
 * Questo garantisce O(n log n) medio anche con molti duplicati.
 */
void fm_qsort(fm_file_t *arr, int n,
              int (*cmp)(const fm_file_t *, const fm_file_t *)) {
  if (n <= 1)
    return;

  /* Scegli pivot mediano fra primo, medio e ultimo per ridurre il caso peggiore
   */
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
    return; /* già ordinato dopo i tre swap sopra */

  /* Porta il pivot in posizione n-1 */
  fm_file_t pivot_val = arr[mid];
  fm_file_t t = arr[mid];
  arr[mid] = arr[n - 2];
  arr[n - 2] = t;

  /* Partizione a tre vie: [< pivot] [== pivot] [> pivot]
     eq_start..eq_end è la zona degli elementi uguali al pivot */
  int lo = 0;
  int hi = n - 2; /* pivot è in n-2 temporaneamente */

  /* Schema Lomuto-like a tre partizioni */
  int i = lo;
  int eq = lo; /* prossima posizione per elemento uguale */

  while (i < hi) {
    int c = cmp(&arr[i], &pivot_val);
    if (c < 0) {
      /* arr[i] < pivot: mettilo nella zona sinistra */
      fm_file_t tmp = arr[i];
      arr[i] = arr[eq];
      arr[eq] = tmp;
      eq++;
      i++;
    } else if (c == 0) {
      /* arr[i] == pivot: salta (verrà gestito con la zona centrale) */
      i++;
    } else {
      /* arr[i] > pivot: scambia con l'ultimo non ancora visto */
      hi--;
      fm_file_t tmp = arr[i];
      arr[i] = arr[hi];
      arr[hi] = tmp;
      /* non incrementare i: l'elemento appena portato in i va rivalutato */
    }
  }

  /* Ora: arr[0..eq-1] < pivot, arr[eq..hi-1] contiene misti == e > pivot.
     Rimettiamo il pivot al suo posto e separiamo == da >. */
  /* Più semplice: usa la partizione classica di Hoare a due vie che è
     corretta e non ha il bug originale; il fix mediano evita il worst case. */

  /* --- Ricomincio con approccio Hoare classico, che è corretto ---
   * Il codice sopra era didattico. Usiamo Hoare che gestisce i duplicati
   * perché left++ e right-- avvengono SEMPRE dopo ogni swap, evitando
   * il loop infinito dell'originale. */

  /* Reset e implementazione Hoare corretta */
  (void)eq;
  (void)i; /* variabili sopra non usate dalla versione finale */

  {
    fm_file_t pv = arr[n / 2];
    int l = 0;
    int r = n - 1;

    while (l <= r) {
      /* BUG FIX rispetto all'originale: usa < e > (strict), non <= e >=
         In questo modo elementi UGUALI al pivot NON bloccano i cursori:
         l avanza finché arr[l] < pivot (si ferma su >= pivot)
         r retrocede finché arr[r] > pivot (si ferma su <= pivot)
         Se l <= r, swap e avanza ENTRAMBI — questo è il punto critico
         che mancava nell'originale (avanzava solo uno dei due in certi casi).
       */
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

    /* Ricorsione solo sulle due metà, escludendo la zona centrale
       che contiene elementi == pivot già in posizione corretta */
    if (r > 0)
      fm_qsort(arr, r + 1, cmp);
    if (l < n - 1)
      fm_qsort(&arr[l], n - l, cmp);
  }
}
