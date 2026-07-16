/*
 * user/sys/lib/malloc.c
 * Userland Heap Allocator — segregated free lists + boundary-tag coalescing
 *
 * Built entirely on top of OS1's SYS_SBRK (#216) via _sys_sbrk(); no other
 * kernel primitive is required. This replaces the earlier "singly linked
 * free/alloc chain" design with the standard technique (Knuth's boundary
 * tags, as used by dlmalloc-family allocators) so correctness no longer
 * depends on maintaining an address-order invariant by hand.
 *
 * ---- Block layout ----
 *
 *   [ header (16B) ][ payload (>=32B, 16B-aligned) ][ footer (8B) ]
 *
 *   header.size / footer.size both hold the TOTAL block size (header +
 *   payload + footer) with bit0 used as the FREE flag. Every block's total
 *   size is a multiple of 16 by construction, so bit0 is otherwise always
 *   0 and safe to steal for the flag.
 *
 *   Storing the size at BOTH ends means any block's neighbours — forward
 *   (address + size) or backward (read the size word immediately before
 *   this block's header) — can be located and inspected in O(1), for ANY
 *   block, allocated or free, however it was created. This is what makes
 *   coalescing safe without assuming a block's list-neighbour is its
 *   memory-neighbour (the bug in the previous design, USR-MALLOC-02).
 *
 *   A FREE block additionally overlays two pointers (next/prev) at the
 *   start of its payload — the intrusive doubly-linked list node for its
 *   size bin. Safe because payload is always >= 32 bytes when free.
 *
 * ---- Allocation strategy ----
 *
 *   8 segregated bins by payload size (32/64/128/256/512/1024/2048/+inf).
 *   malloc() does first-fit *within and above* the requested bin — an
 *   approximate best-fit that is O(1) amortized for the common case
 *   instead of O(n) over the whole heap.
 *
 * ---- What this fixes relative to the original allocator ----
 *
 *   USR-MALLOC-02 (coalescing corruption): gone by construction — no
 *     "assume physical adjacency" step exists anymore; boundary tags let
 *     us verify it directly.
 *   USR-MALLOC-03 (no backward coalescing): implemented (see free()).
 *   USR-MALLOC-04 (heap never shrinks): the trailing free block is
 *     returned to the kernel via a negative sbrk() in free().
 *   USR-MALLOC-05 (payload not 16-byte aligned): fixed for real — the
 *     16-byte header plus the size-residue trick below guarantee every
 *     payload starts 16-byte aligned (given a 16-aligned heap start,
 *     which grow_heap() enforces defensively on first use).
 *   USR-MALLOC-06 (realloc shrink check): realloc() below compares
 *     against actual old payload capacity, same tradeoff as before
 *     (documented, not a correctness bug) — still returns in place when
 *     it fits, to avoid pointless copies.
 */
#include <os1.h>
#include <stddef.h>
#include <stdint.h>

typedef struct header {
  size_t size;   /* total block size | FREE_BIT (see above) */
  size_t canary; /* unused; pads header to 16B so payload is 16B-aligned */
} header_t;

typedef struct free_node {
  struct free_node *next;
  struct free_node *prev;
} free_node_t;

#define HEADER_SIZE (sizeof(header_t)) /* 16 */
#define FOOTER_SIZE (sizeof(size_t))   /* 8  */
#define FREE_BIT ((size_t)1)
#define SIZE_MASK (~FREE_BIT)
#define MALLOC_ALIGN 16UL
#define MIN_PAYLOAD 32UL /* room for a free_node_t plus slack */
#define MIN_BLOCK (HEADER_SIZE + MIN_PAYLOAD + FOOTER_SIZE)
#define NUM_BINS 8

static free_node_t *bins[NUM_BINS];
static uint8_t *heap_start = NULL;
static uint8_t *heap_end = NULL; /* current program break */

/* ---------------- boundary-tag helpers ---------------- */

static inline size_t blk_size(header_t *h) { return h->size & SIZE_MASK; }
static inline int blk_free(header_t *h) { return (int)(h->size & FREE_BIT); }

/* Writes both the header and the mirrored footer in one place, so the two
 * can never drift apart. Every size/flag change goes through this. */
static void set_block(header_t *h, size_t total_size, int is_free) {
  size_t enc = (total_size & SIZE_MASK) | (is_free ? FREE_BIT : 0);
  h->size = enc;
  *(size_t *)((uint8_t *)h + total_size - FOOTER_SIZE) = enc;
}

static header_t *node_to_header(free_node_t *n) {
  return (header_t *)((uint8_t *)n - HEADER_SIZE);
}

static int bin_index(size_t payload) {
  static const size_t thresh[NUM_BINS - 1] = {32,  64,   128, 256,
                                              512, 1024, 2048};
  for (int i = 0; i < NUM_BINS - 1; i++)
    if (payload <= thresh[i])
      return i;
  return NUM_BINS - 1;
}

static void bin_remove(header_t *h) {
  free_node_t *n = (free_node_t *)(h + 1);
  int idx = bin_index(blk_size(h) - HEADER_SIZE - FOOTER_SIZE);
  if (n->prev)
    n->prev->next = n->next;
  else
    bins[idx] = n->next;
  if (n->next)
    n->next->prev = n->prev;
}

static void bin_insert(header_t *h) {
  int idx = bin_index(blk_size(h) - HEADER_SIZE - FOOTER_SIZE);
  free_node_t *n = (free_node_t *)(h + 1);
  n->prev = NULL;
  n->next = bins[idx];
  if (bins[idx])
    bins[idx]->prev = n;
  bins[idx] = n;
}

/* ---------------- sbrk shims (unchanged ABI) ---------------- */

void *OS1low_vm_sbrk(intptr_t increment) { return _sys_sbrk(increment); }
void *sbrk(intptr_t increment) { return OS1low_vm_sbrk(increment); }

/*
 * sbrk_failed - did this sbrk() return an error rather than a break address?
 *
 * SYS_SBRK (kernel/sched/process.c sys_sbrk) returns a NEGATIVE errno on
 * failure — -ENOMEM when the heap hits SBRK_HEAP_LIMIT or a page can't be
 * allocated, -EINVAL on a bad shrink — NOT the (void *)-1 that plain sbrk()
 * uses.  Any value in the top page ([-4095, -1]) is such an error code; a
 * real user-heap break never lands there.  Checking only ==(void *)-1 let
 * -ENOMEM (-12 -> 0xfffffffffffffff4) slip through as a "pointer", and
 * grow_heap()/set_block() then wrote a block header to it: the Data Abort at
 * addr=0xfffffffffffffff4 in malloc under memory pressure (Lua GC stress).
 */
static inline int sbrk_failed(void *p) {
  return (uintptr_t)p >= (uintptr_t)(-4095);
}

/* Grows the heap by exactly min_total bytes (already a multiple of 16) and
 * returns a fresh block header covering the extension, marked free. On the
 * very first call it defensively aligns the initial break up to 16 bytes
 * (via a tiny probe + pad sbrk) in case the kernel's initial break isn't
 * already aligned — everything after that stays aligned by induction,
 * since every block's total size is a multiple of 16. */
static header_t *grow_heap(size_t min_total) {
  if (!heap_start) {
    void *probe = sbrk(0);
    if (!sbrk_failed(probe)) {
      uintptr_t addr = (uintptr_t)probe;
      uintptr_t pad =
          (MALLOC_ALIGN - (addr & (MALLOC_ALIGN - 1))) & (MALLOC_ALIGN - 1);
      if (pad) {
        if (sbrk_failed(sbrk((intptr_t)pad)))
          return NULL;
      }
    }
  }

  void *p = sbrk((intptr_t)min_total);
  if (sbrk_failed(p))
    return NULL;

  if (!heap_start)
    heap_start = (uint8_t *)p;
  heap_end = (uint8_t *)p + min_total;

  header_t *h = (header_t *)p;
  set_block(h, min_total, 1);
  return h;
}

/* ---------------- malloc ---------------- */

void *malloc(size_t size) {
  if (size == 0)
    return NULL;

  /* Round up to 16, then add an 8-byte residue so that
   * HEADER(16) + payload + FOOTER(8) is itself always a multiple of 16
   * — this is what keeps every block's payload 16-byte aligned. */
  size_t payload = (size + (MALLOC_ALIGN - 1)) & ~(MALLOC_ALIGN - 1);
  if (payload < MIN_PAYLOAD)
    payload = MIN_PAYLOAD;
  payload += 8;

  size_t need = HEADER_SIZE + payload + FOOTER_SIZE;

  int start_bin = bin_index(payload);
  for (int b = start_bin; b < NUM_BINS; b++) {
    for (free_node_t *n = bins[b]; n; n = n->next) {
      header_t *h = node_to_header(n);
      if (blk_size(h) < need)
        continue;

      bin_remove(h);
      size_t total = blk_size(h);
      size_t remainder = total - need;

      if (remainder >= MIN_BLOCK) {
        set_block(h, need, 0);
        header_t *rem = (header_t *)((uint8_t *)h + need);
        set_block(rem, remainder, 1);
        bin_insert(rem);
      } else {
        set_block(h, total, 0); /* keep the few extra bytes */
      }
      return (void *)(h + 1);
    }
  }

  /* Nothing fits in any bin: extend the heap for exactly this request. */
  header_t *h = grow_heap(need);
  if (!h)
    return NULL;
  set_block(h, need, 0);
  return (void *)(h + 1);
}

/* ---------------- free ---------------- */

void free(void *ptr) {
  if (!ptr)
    return;

  header_t *h = (header_t *)ptr - 1;
  size_t total = blk_size(h);
  set_block(h, total, 1);

  /* Forward coalesce: the boundary tag lets us check the ACTUAL next
   * block in memory directly, whatever it is — no assumption needed. */
  uint8_t *next_addr = (uint8_t *)h + total;
  if (next_addr < heap_end) {
    header_t *nh = (header_t *)next_addr;
    if (blk_free(nh)) {
      bin_remove(nh);
      total += blk_size(nh);
      set_block(h, total, 1);
    }
  }

  /* Backward coalesce: read the size word immediately before this
   * block's header (the previous block's footer) to jump straight to
   * its header in O(1) and check whether IT is free. */
  if ((uint8_t *)h > heap_start) {
    size_t prev_enc = *((size_t *)h - 1);
    size_t prev_size = prev_enc & SIZE_MASK;
    header_t *ph = (header_t *)((uint8_t *)h - prev_size);
    if (blk_free(ph)) {
      bin_remove(ph);
      total = prev_size + total;
      set_block(ph, total, 1);
      h = ph;
    }
  }

  /* If the coalesced free block now reaches the end of the heap, hand
   * it back to the kernel instead of keeping it around unused. */
  if ((uint8_t *)h + total == heap_end) {
    heap_end = (uint8_t *)h;
    sbrk(-(intptr_t)total);
    return;
  }

  bin_insert(h);
}

/* ---------------- realloc ---------------- */

void *realloc(void *ptr, size_t size) {
  if (!ptr)
    return malloc(size);
  if (size == 0) {
    free(ptr);
    return NULL;
  }

  header_t *h = (header_t *)ptr - 1;
  size_t old_payload = blk_size(h) - HEADER_SIZE - FOOTER_SIZE;

  if (old_payload >= size)
    return ptr; /* already fits */

  void *new_ptr = malloc(size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, old_payload);
    free(ptr);
  }
  return new_ptr;
}

/* ---------------- calloc ---------------- */

void *calloc(size_t nmemb, size_t size) {
  /* Pre-multiply overflow check (USR-MALLOC-01): rejects any input whose
   * product would wrap, without relying on the wraparound happening. */
  if (nmemb != 0 && size > SIZE_MAX / nmemb)
    return NULL;

  size_t total = nmemb * size;
  void *ptr = malloc(total);
  if (ptr)
    memset(ptr, 0, total);
  return ptr;
}