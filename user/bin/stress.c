/*
 * user/bin/stress.c
 * Long-duration kernel stress driver (perf brief §6).
 *
 * Independently toggleable lanes, fixed-seed deterministic PRNG, and periodic
 * on-stdout MEMSTAT samples (CSV) so a headless serial capture over a 2-hour
 * run IS the time series the drift analysis (tools/analyze_drift.py) consumes.
 * Stdout (serial) is used rather than a log FILE on purpose: it always works
 * headless and avoids the console-contention Heisenbug (samples are emitted
 * once per --interval, default 60 s, not per cycle).
 *
 * Lanes (each exercises the kernel paths the campaign touches):
 *   spawn  — spawn a trivial child (/bin/hello) and wait it: process_create /
 *            teardown, PMM, scheduler, ASID/PCID switch, object-handle teardown,
 *            the timer/sleep teardown path.  A --fault fraction spawns /bin/crash
 *            instead, exercising the ABNORMAL (fault-kill) teardown path.
 *   alloc  — userland malloc/free across zipf-ish size classes: sbrk -> PMM.
 *   file   — open/write/read/close churn: VFS + ext4 + buffer cache.
 *   gui    — create_window/destroy_window churn: compositor window paths.
 *
 * Usage (run from the shell, headless serial captured to a file):
 *   stress [--dur S] [--seed HEX] [--interval S] [--fault PCT]
 *          [--spawn] [--alloc] [--file] [--gui] [--all]
 *   Defaults: --all lanes, --dur 7200 (2 h), --seed 0xDEADBEEF, --interval 60,
 *             --fault 7.  If any explicit lane flag is given, only those run.
 */
#include <os1.h>
#include <string.h>  /* strcmp */
#include <stdlib.h>  /* atoi, atoll, malloc, free */

/* parse_ull - decimal, or hex with a 0x/0X prefix (for --seed). */
static unsigned long long parse_ull(const char *s) {
  unsigned long long v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    for (; *s; s++) {
      char c = *s;
      unsigned d;
      if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
      else break;
      v = (v << 4) | d;
    }
    return v;
  }
  for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (unsigned)(*s - '0');
  return v;
}

/* ---- deterministic PRNG (xorshift64) ---- */
static unsigned long long rng_state;
static unsigned long long xrand(void) {
  unsigned long long x = rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  rng_state = x;
  return x;
}

/* lane bitmask */
#define LANE_SPAWN 1u
#define LANE_ALLOC 2u
#define LANE_FILE  4u
#define LANE_GUI   8u

static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(int argc, char *argv[]) {
  unsigned long long dur_s = 7200;       /* 2 hours */
  unsigned long long seed = 0xDEADBEEFULL;
  unsigned long long interval_s = 60;
  int fault_pct = 7;
  unsigned int lanes = 0; /* 0 => default-all after parse */

  for (int i = 1; i < argc; i++) {
    if (str_eq(argv[i], "--dur") && i + 1 < argc) dur_s = parse_ull(argv[++i]);
    else if (str_eq(argv[i], "--seed") && i + 1 < argc) seed = parse_ull(argv[++i]);
    else if (str_eq(argv[i], "--interval") && i + 1 < argc) interval_s = parse_ull(argv[++i]);
    else if (str_eq(argv[i], "--fault") && i + 1 < argc) fault_pct = atoi(argv[++i]);
    else if (str_eq(argv[i], "--spawn")) lanes |= LANE_SPAWN;
    else if (str_eq(argv[i], "--alloc")) lanes |= LANE_ALLOC;
    else if (str_eq(argv[i], "--file"))  lanes |= LANE_FILE;
    else if (str_eq(argv[i], "--gui"))   lanes |= LANE_GUI;
    else if (str_eq(argv[i], "--all"))   lanes |= (LANE_SPAWN|LANE_ALLOC|LANE_FILE|LANE_GUI);
  }
  if (lanes == 0) lanes = (LANE_SPAWN|LANE_ALLOC|LANE_FILE|LANE_GUI);
  rng_state = seed ? seed : 1;

  unsigned long long start = os1_mono_ns();
  unsigned long long dur_ns = dur_s * 1000000000ULL;
  unsigned long long interval_ns = interval_s * 1000000000ULL;
  unsigned long long next_stat = 0; /* emit one immediately */
  unsigned long long cycles = 0, spawns = 0, faults = 0, allocs = 0, files = 0, guis = 0, errs = 0;

  printf("[stress] start seed=0x%lx dur=%lus interval=%lus fault=%d%% lanes=0x%x\n",
         (unsigned long)seed, (unsigned long)dur_s, (unsigned long)interval_s, fault_pct, lanes);
  printf("MEMSTAT,t_s,free,lcr,runs,alloc_calls,free_calls,search_ns_total,search_ns_max,"
         "km_inuse,km_hi,km_live,ctxsw,runnable,zomb,objF,objP,objR,objW,cycles\n");

  for (;;) {
    unsigned long long now = os1_mono_ns();
    unsigned long long elapsed = now - start;
    if (elapsed >= dur_ns) break;

    /* periodic stats sample */
    if (elapsed >= next_stat) {
      struct os1_sysstats s;
      if (OS1_sys_stats(&s) > 0) {
        printf("MEMSTAT,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
               (unsigned long)(elapsed / 1000000000ULL),
               (unsigned long)s.pmm_free_pages, (unsigned long)s.pmm_largest_contig_run,
               (unsigned long)s.pmm_free_run_count, (unsigned long)s.pmm_alloc_calls,
               (unsigned long)s.pmm_free_calls, (unsigned long)s.pmm_alloc_search_ns_total,
               (unsigned long)s.pmm_alloc_search_ns_max, (unsigned long)s.km_bytes_in_use,
               (unsigned long)s.km_high_water_bytes, (unsigned long)s.km_live_allocs,
               (unsigned long)s.sched_ctx_switches, (unsigned long)s.sched_runnable,
               (unsigned long)s.sched_zombie_count,
               (unsigned long)s.obj_live_by_type[OBJ_TYPE_FILE],
               (unsigned long)s.obj_live_by_type[OBJ_TYPE_PROCESS],
               (unsigned long)s.obj_live_by_type[OBJ_TYPE_REGKEY],
               (unsigned long)s.obj_live_by_type[OBJ_TYPE_WINDOW],
               (unsigned long)cycles);
      }
      next_stat = elapsed + interval_ns;
    }

    /* --- spawn lane (primary): spawn a trivial child and reap it --- */
    if (lanes & LANE_SPAWN) {
      int do_fault = (fault_pct > 0) && ((int)(xrand() % 100) < fault_pct);
      const char *path = do_fault ? "/bin/crash" : "/bin/hello";
      int pid = spawn(path);
      if (pid > 0) { wait(pid); spawns++; if (do_fault) faults++; }
      else errs++;
    }

    /* --- alloc lane: malloc/free across size classes (sbrk -> PMM) --- */
    if (lanes & LANE_ALLOC) {
      void *ptrs[16];
      int n = (int)(xrand() % 16) + 1;
      for (int k = 0; k < n; k++) {
        /* zipf-ish: mostly small, occasionally large */
        unsigned long sz = (xrand() % 10 == 0) ? (1u << (12 + (xrand() % 4)))  /* 4K..32K */
                                               : (16u << (xrand() % 6));        /* 16..512 */
        ptrs[k] = malloc(sz);
        if (ptrs[k]) { *(volatile char *)ptrs[k] = 1; } /* touch first byte */
      }
      for (int k = 0; k < n; k++) if (ptrs[k]) free(ptrs[k]);
      allocs++;
    }

    /* --- file lane: write/read churn (VFS + ext4 + buffer cache) --- */
    if (lanes & LANE_FILE) {
      static const char *scratch = "/etc/stress.tmp";
      char buf[256];
      for (int k = 0; k < (int)sizeof(buf); k++) buf[k] = (char)(xrand() & 0xff);
      int off = (int)(xrand() % 8192);
      int w = file_write(scratch, buf, (int)sizeof(buf), off);
      if (w == (int)sizeof(buf)) {
        char rb[256];
        file_read(scratch, rb, (int)sizeof(rb), off);
        files++;
      } else {
        /* scratch not writable on this fs layout: fall back to read churn of an
         * existing file so the lane still exercises VFS/ext4/buffer cache. */
        char rb[128];
        if (file_read("/etc/init.cfg", rb, (int)sizeof(rb), 0) > 0) files++;
        else errs++;
      }
    }

    /* --- gui lane: create + destroy a window (compositor paths) --- */
    if (lanes & LANE_GUI) {
      int w = 100 + (int)(xrand() % 300);
      int h = 80 + (int)(xrand() % 200);
      int win = create_window(50, 50, w, h, "stress");
      if (win > 0) { destroy_window(win); guis++; }
      /* a failed create (e.g. window cap) is expected under load: not an error */
    }

    cycles++;
  }

  unsigned long long total_s = (os1_mono_ns() - start) / 1000000000ULL;
  printf("[stress] DONE cycles=%lu spawns=%lu faults=%lu allocs=%lu files=%lu guis=%lu errs=%lu in %lus\n",
         (unsigned long)cycles, (unsigned long)spawns, (unsigned long)faults,
         (unsigned long)allocs, (unsigned long)files, (unsigned long)guis,
         (unsigned long)errs, (unsigned long)total_s);
  return 0;
}
