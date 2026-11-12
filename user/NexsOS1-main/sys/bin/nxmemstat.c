/*
 * user/sys/bin/nxmemstat.c
 * NEXS system-statistics frontend (ASTRA stratified service model).
 *
 * Usage:
 *   nxmemstat              open a window and show live system stats, redrawing
 *                          only when a meaningful value changes (virtio UI)
 *   nxmemstat --log [S]    headless: print one MEMSTAT CSV record every S seconds
 *                          (default 60) to stdout/serial, for a long-duration
 *                          stress capture analysed by tools/analyze_drift.py
 *
 * Thin frontend over the reusable helper nxmemstat.h.  Runs as a /sys/bin ROOT
 * service so it may call the ROOT-gated OS1_sys_stats(); USER apps use this
 * window, never the raw syscall.  The GUI renders to a compositor window (the
 * virtio user interface), kept distinct from the UART kernel log.
 */
#include "nxmemstat.h"
#include <os1.h>
#include <string.h>

static int str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Live windowed monitor (default). */
static int cmd_window(void) {
  int win = _sys_create_window(120, 120, 560, 280, "NEXS System Monitor");
  if (win < 0)
    return 1;
  struct os1_sysstats prev;
  memset(&prev, 0, sizeof(prev));
  unsigned long sig = 0; /* seed: forces first render */
  while (1) {
    nxmemstat_render_if_changed(win, &prev, &sig, 1000);
    OS1_sleep(1000);
  }
  return 0;
}

/* Headless serial CSV logger for the stress campaign.  If drive_argc>0, first
 * spawn drive_argv[0] (the load driver, e.g. /bin/stress) as a child: this
 * service is ROOT, so the /bin child is correctly created at USER level — the
 * shell is foreground-only and a USER process cannot spawn a ROOT logger, so
 * the ROOT logger drives the USER load instead. */
static int cmd_log(unsigned long interval_s, int drive_argc, char *const drive_argv[]) {
  if (interval_s == 0)
    interval_s = 60;
  struct os1_sysstats s;
  if (nxmemstat_snapshot(&s) < 0) {
    print("[nxmemstat] OS1_sys_stats denied (need ROOT) or failed\n");
    return 1;
  }
  if (drive_argc > 0) {
    int pid = spawn_args(drive_argv[0], drive_argc, drive_argv);
    if (pid > 0)
      printf("[nxmemstat] spawned load driver '%s' pid=%d\n", drive_argv[0], pid);
    else
      printf("[nxmemstat] failed to spawn '%s' (err %d)\n", drive_argv[0], pid);
  }
  print("[nxmemstat] logging system stats (CSV, MEMSTAT prefix)\n");
  print("MEMSTAT,t_s,free,lcr,runs,alloc_calls,free_calls,search_ns_total,"
        "search_ns_max,km_inuse,km_hi,km_live,ctxsw,runnable,zomb,objF,objP,"
        "objR,objW,objC,cycles\n");
  unsigned long long start = os1_mono_ns();
  for (;;) {
    if (nxmemstat_snapshot(&s) > 0) {
      char line[256];
      unsigned long t = (unsigned long)((os1_mono_ns() - start) / 1000000000ULL);
      nxmemstat_csv_line(line, (int)sizeof(line), &s, t);
      print(line);
    }
    OS1_sleep((int)(interval_s * 1000UL));
  }
  return 0;
}

int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    if (str_eq(argv[i], "--log")) {
      unsigned long iv = 0;
      int j = i + 1;
      if (j < argc && argv[j][0] != '-') { /* optional interval seconds */
        iv = (unsigned long)atoi(argv[j]);
        j++;
      }
      /* optional load driver: --run <prog> [args...] (spawned as a USER child) */
      int drive_argc = 0;
      char *const *drive_argv = 0;
      for (int k = j; k < argc; k++) {
        if (str_eq(argv[k], "--run")) {
          drive_argv = (char *const *)&argv[k + 1];
          drive_argc = argc - (k + 1);
          break;
        }
      }
      return cmd_log(iv, drive_argc, drive_argv);
    }
  }
  return cmd_window();
}
