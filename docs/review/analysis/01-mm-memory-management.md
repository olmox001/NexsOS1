# Subsystem Analysis 01 — Memory Management (`mm`)

> Severity/kind tags are defined in [`../TAXONOMY.md`](../TAXONOMY.md).
> Evidence basis: **[verified]** built; **[static]** read-only; **[inferred]** reasoned.

| | |
|---|---|
| **Subsystem** | Memory Management |
| **Sources** | `kernel/mm/pmm.c` (491), `kernel/mm/vmm.c` (317), `kernel/mm/buffer.c` (167), `kernel/lib/kmalloc.c` (286) |
| **Headers** | `kernel/include/kernel/{pmm,vmm,buffer,kmalloc}.h` |
| **Arch hooks** | `arch_vmm_map*`, `arch_vmm_unmap`, `arch_vmm_init_hw`, `arch_vmm_set_pgd`, `arch_cache_clean_range`, `arch_mb/isb`, `arch_platform_get_mem_regions` |
| **Build** | **[verified]** compiles clean (0 warn) on both arches under `-Werror -Wall -Wextra -Wpedantic -Wshadow` |

---

## 1. Purpose & Role

The `mm` subsystem provides four layers:

1. **PMM** (`pmm.c`) — physical page frame allocator. Bitmap-per-zone (DMA ≤16MB,
   Normal), `struct page` array for per-frame metadata, next-fit single-page +
   linear-scan contiguous allocation.
2. **VMM** (`vmm.c`) — page-table construction and the two-phase MMU bring-up
   (bootstrap 128MB → dynamic remap of all detected RAM), plus per-process PGD
   create/destroy. Generic 4-level walker; heavy lifting delegated to `arch_vmm_*`.
3. **Buffer cache** (`buffer.c`) — block cache between the filesystem and
   `virtio_blk`, hash table + LRU, page-sized buffers.
4. **kmalloc** (`kmalloc.c`) — kernel heap: power-of-two buckets (16B–4KB) over a
   32MB bump pool, large allocations passed straight to the PMM.

## 2. Data Flow

```
arch_platform_get_mem_regions ──▶ pmm_early_init (size metadata, place it)
                                  pmm_init (zones, reserve kernel+metadata)
                                        │
            kmalloc ◀── pmm_alloc_pages ┤ pmm_alloc_page / pmm_alloc_aligned
                                        │
   vmm_init (bootstrap 128MB, MMU on) ──┘
   vmm_dynamic_remap (all RAM)                      virtio_blk_read
            │                                              ▲
   process PGDs ◀── vmm_create_pgd            buffer_get ──┘──▶ ext4 / fs
```

## 3. What Works (verified vs static)

- **[verified]** Both kernels build and link with this allocator; userland ELFs
  load (init/nxshell present in the rootfs image).
- **[static]** RAM discovery is dynamic and not hard-capped at 1GB:
  `pmm_early_init` scans `mem_region`s and clamps only at 256GB
  (`pmm.c:108-171`). This is the mechanism behind "map memory up to 4GB".
- **[static]** Double-free and reserved-page frees are detected and `panic`/warn
  (`pmm.c:367-394`); freed frames are poisoned `0xCC` (`pmm.c:402`).
- **[static]** `kfree` takes the lock **before** reading the block magic to close
  an SMP double-free race — a deliberate, correct choice (`kmalloc.c:220-228`).
- **[static]** Buffer cache uses a load-outside-lock + re-check pattern to avoid
  holding the lock across disk I/O (`buffer.c:97-120`).

## 4. Central Invariant (the thing to fix first)

**The entire subsystem assumes an identity / linear mapping** (kernel virtual
address == physical address for the RAM window). Evidence:

- PMM returns `MEMORY_BASE + pfn_to_phys(pfn)` and the caller uses it directly as
  a pointer (`pmm.c:277`, `336`).
- VMM stores that same value as a *physical* PTE address **and** dereferences it
  as a pointer in `get_next_table` (`vmm.c:38-43`).
- The code comments themselves admit the confusion in-line (`vmm.c:54-63`:
  *"Wait, pmm returns… Let's assume… Checking pmm implementation…"*).

This invariant works under QEMU `virt`/`-kernel` today, but it **directly
contradicts** the higher-half memory map advertised in the old README
(`0xFFFF_…`) and is the single biggest blocker to a clean PA/VA model, to W^X,
and to the seL4-style isolation goal. Treat "define and document the PA/VA
model" as the root task that several findings below depend on.

## 5. Findings

| ID | Sev | Kind | Location | Summary |
|----|-----|------|----------|---------|
| MM-PMM-01 | W3 | STUB | `pmm.c:14-18` | `pmm_init_region()` is a stub that only prints; multi-region management never implemented. |
| MM-PMM-02 | W3 | BUG · SECURITY | `pmm.c:302-342` vs `247-285` | `pmm_alloc_pages`/`_aligned` skip the cache-clean+barrier that `pmm_alloc_page` does → stale cache lines in DMA buffers. |
| MM-PMM-03 | W2 | PERF | `pmm.c:67-84, 302-318` | Contiguous alloc is an O(n) bitmap scan from 0 every call; no buddy structure. |
| MM-PMM-04 | W2 | WRONG-DESIGN | `pmm.c:302-342, 420-451` | Contiguous/aligned alloc only searches `ZONE_NORMAL` → impossible to get contiguous **DMA-zone** memory. |
| MM-PMM-05 | W2 | BAD-IMPL | `pmm.c` (global `free_pages` vs `z->free_pages`) | Two consistency models: global counter is atomic, per-zone counter is plain-under-lock; easy to desync. |
| MM-PMM-06 | W1 | REFINE | `pmm.c:264, 313` | `next_free_pfn` next-fit is ignored by the contiguous path; allocator policy is inconsistent. |
| MM-PMM-07 | W2 | WRONG-DESIGN · DOC | `pmm.c:277, 456-473` | Returns physical addresses used as pointers; the identity-map invariant is undocumented (see §4). |
| MM-VMM-01 | W3 | SECURITY | `vmm.c:202, 234-235` | **All** RAM mapped `PAGE_KERNEL_EXEC` → no W^X; kernel data/heap/stacks are executable. |
| MM-VMM-02 | W3 | WRONG-DESIGN | `vmm.c:37-43, 54-63, 114-144` | Page-table walker dereferences PTE physical addresses as pointers; only valid under identity map; blocks higher-half kernel. |
| MM-VMM-03 | W2 | REFINE (TODO) | `vmm.c:253-257` | `vmm_dynamic_remap` leaks the old PGD; needs IPI + TLB sync to free safely. |
| MM-VMM-04 | W3 | BUG | `vmm.c:272-316` | `vmm_destroy_pgd` frees table pages but **never frees user RAM frames** (no frame refcount) → user memory leak on exit; magic indices; empty dead loop at `309-313`. |
| MM-VMM-05 | W3 | BUG · SECURITY | `vmm.c:153-163` | No cross-CPU TLB shootdown on unmap/remap → stale translations on other cores (correctness + isolation). |
| MM-VMM-06 | W2 | REFINE | `vmm.c:88-98, 168-182` | Generic map path is 4KB-only; 2MB blocks exist only in `arch_vmm_map_range`; inconsistent + slow for big maps. |
| MM-VMM-07 | W1 | DOC | `vmm.c:1-6` | Header says "AArch64 4-level" but the file is compiled for amd64 too. |
| MM-BUF-01 | W3 | BUG | `buffer.c:46-59, 80-82` | If all buffers are referenced/dirty, eviction frees nothing yet `buffer_get` keeps allocating — `MAX_BUFFERS` is not a hard cap → unbounded growth. |
| MM-BUF-02 | W2 | PERF | `buffer.c:90-120` | Two CPUs missing the same block both allocate and both DMA-read it; the loser is discarded — wasted disk I/O, no in-flight lock. |
| MM-BUF-03 | W2 | BUG · SECURITY | `buffer.c` (no per-buffer lock) | No content lock: `buffer_sync` writes while readers read the same `buf->data`; data race. |
| MM-BUF-04 | W2 | REFINE | `buffer.c:141-166` | `buffer_sync` silently flushes only the first 64 dirty buffers per call. |
| MM-BUF-05 | W1 | REFINE | `buffer.c:14-21` | Weak hash (`block % 64`), fixed bucket count. |
| MM-KM-01 | W3 | MISSING · WRONG-DESIGN | `kmalloc.c:71-104, 149-167` | The "incomplete malloc/free" you flagged: bump pointer only grows; freed small blocks are trapped per-bucket; no coalescing, no return-to-PMM, no growth → hard `NULL` on heap exhaustion. |
| MM-KM-02 | W2 | WRONG-DESIGN | `kmalloc.c:130-167` | No cross-bucket reuse: a free 512B block can't satisfy a 256B request; structural fragmentation. |
| MM-KM-03 | W2 | BUG | `kmalloc.c:180-186` | Large allocations return `page_start + 32` → **not page-aligned**; a footgun for DMA/page-expecting callers. |
| MM-KM-04 | W2 | REFINE | `kmalloc.c:257-285` | `krealloc` always alloc+copy+free, even when shrinking within the same bucket. |
| MM-KM-05 | W1 | PERF | `kmalloc.c:24-29, 44-66` | 32B (16-aligned) header on a 16B min bucket → `kmalloc(1)` consumes a 64B bucket. |
| MM-KM-06 | W2 | PERF | `kmalloc.c:15, 41` | Single 32MB pool behind one global lock; no per-CPU magazines → SMP contention. |

### Detailed entries (selected)

**MM-KM-01 — kmalloc/kfree are incomplete `[static]`**
The heap is a bump allocator (`heap_ptr` only advances, `kmalloc.c:149-162`) with
per-bucket LIFO free lists (`kfree`, `kmalloc.c:240-251`). Consequences:
freed memory is *never* returned to the PMM; a workload that allocates many
4KB-bucket objects then frees them cannot reuse that space for 256B objects;
once `heap_ptr` reaches `heap_end`, small allocations fail permanently
(`kmalloc.c:164-167`) even though the free lists may be full. This is adequate
for a fixed boot-time working set but not for long-running services — exactly the
userland-service direction the project is heading. *Fix direction:* a real slab
allocator with per-size caches that can grow/shrink against the PMM, plus
coalescing for the large path; align this with MM-PMM-03 (buddy allocator).

**MM-VMM-01 — no W^X `[static, security]`**
`vmm_init` maps the bootstrap RAM with `PAGE_KERNEL_EXEC` (`vmm.c:202`) and
`vmm_dynamic_remap` maps *every* usable region `PAGE_KERNEL_EXEC`
(`vmm.c:234-235`). The kernel heap, stacks, and page tables are therefore
executable. Under the stated seL4-style isolation goal this is a baseline that
must change to per-section permissions (`.text` RX, `.rodata` RO, everything else
RW-NX). Depends on §4 (a real PA/VA model) to be done cleanly.

**MM-VMM-04 — process teardown leaks frames `[static]`**
`vmm_destroy_pgd` walks index 0's private PUD and frees the *table* pages
(`vmm.c:300, 303, 306`) but explicitly does **not** free the RAM frames the user
mapped ("we don't free the actual RAM frames here as they might be shared",
`vmm.c:296-299`). There is no frame refcount wired into the PMM `struct page`
(the field exists, `pmm.c`, but isn't incremented on map). So normal
(non-shared) user pages leak on every process exit. *Fix direction:* per-frame
refcounting integrated with map/unmap, freed on teardown when refcount hits 0.

## 6. Refactor Direction (toward the declared goals)

| Goal (yours) | mm implication |
|---|---|
| **seL4-style isolation** | W^X (MM-VMM-01); real per-process teardown with frame refcounts (MM-VMM-04); TLB shootbown (MM-VMM-05); a documented PA/VA model (§4). |
| **Plan 9 "everything is a file"** | The buffer cache and PMM stats are natural backends for a `/dev`/`/proc`-like namespace later; keep their APIs query-able. |
| **Run long-lived userland services** | A growable slab+buddy allocator (MM-KM-01/02, MM-PMM-03) instead of bump+bucket. |
| **Real hardware + networking (DMA)** | Cache-coherent contiguous/aligned DMA alloc from the DMA zone (MM-PMM-02/04). |

**Suggested sequencing:** (1) document & assert the PA/VA invariant; (2) W^X; (3)
buddy PMM + slab kmalloc; (4) frame-refcounted address-space teardown + TLB
shootdown; (5) buffer-cache hardening (hard cap, per-buffer lock, full sync).

## 7. Verification Notes

- Build: **[verified]** clean, both arches.
- Runtime **aarch64 [verified]** (headless QEMU `virt`, `-m 4G -smp 4`): PMM
  reports `Total detected RAM: 3967 MB` and `DMA zone: 128 pages, Normal zone:
  1005551 pages`; VMM logs `Mapping region 0x40000000 - 0x137fff000` (~4GB) and
  `Dynamic remapping successful`. So the dynamic RAM discovery and the "map up to
  4GB" goal are confirmed **on aarch64**. kmalloc logs `Heap: 32 MB`.
- Runtime **amd64 [verified]**: on the `make run` path (QEMU `-kernel`), PMM does
  **not** see 4GB — the boot protocol magic arrives as `0x0` and the platform
  falls back to a `safe 1GB default`; a divide-by-zero (`Vector 0`) then aborts
  boot shortly after kmalloc init. The 4GB result on amd64 is therefore **not**
  reproduced on this path (see the cross-cutting boot/arch findings; this is an
  architecture-parity defect, not strictly an `mm` defect).
