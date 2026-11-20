/*
 * Dynamic graphics-control acceptance over the real nxres/nxwins programs.
 *
 * The test is bounded, provider-driven and restore-safe. It never assumes a
 * QEMU mode, never grants itself window-manager authority and never leaves a
 * persistent look/display mutation behind.
 */
#include <nxres.h>
#include <os1.h>
#include <os1_surface.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum look_kind {
  LOOK_STYLE = 0,
  LOOK_THEME = 1,
  LOOK_BACKGROUND = 2,
};

static int reap_after_kill(int pid) {
  for (unsigned step = 0U; step < 20U; ++step) {
    int status = 0;
    int result = waitpid(pid, &status, WNOHANG);
    if (result == pid || result < 0)
      return 0;
    OS1_sleep(20);
  }
  return -1;
}

static int wait_child(int pid, int expected_exit) {
  if (pid <= 0)
    return -1;
  for (unsigned step = 0U; step < 250U; ++step) {
    int status = 0;
    int result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      if (!WIFEXITED(status))
        return -2;
      return WEXITSTATUS(status) == expected_exit ? 0 : -3;
    }
    if (result < 0)
      return -4;
    OS1_sleep(20);
  }
  int killed = kill_process(pid);
  int reaped = reap_after_kill(pid);
  return killed < 0 || reaped != 0 ? -5 : -6;
}

static int run_command(const char *path, const char *a, const char *b,
                       const char *c, const char *d, int expected_exit) {
  char *argv[5];
  int argc = 1;
  argv[0] = (char *)path;
  if (a != NULL) argv[argc++] = (char *)a;
  if (b != NULL) argv[argc++] = (char *)b;
  if (c != NULL) argv[argc++] = (char *)c;
  if (d != NULL) argv[argc++] = (char *)d;
  return wait_child(spawn_args(argv[0], argc, argv), expected_exit);
}

/* Observe the real CLI output while its producer is still active. Waiting
 * for exit before reading used to deadlock if the pipe filled or its EOF
 * lagged process wait/reap on an SMP guest. The native PIPE status capability
 * now drives poll; no fabricated readiness, infinite read or test bypass. */
enum { REPORT_BYTES = 4096, REPORT_POLL_STEPS = 1000 };
/* A producer can exit while the parent still has data in the PIPE; reaping
 * and draining must advance concurrently, not in a serial wait chain. */
static int capture_drain(int reader, int child,
                         char report[REPORT_BYTES], size_t *used) {
  int exited = 0;
  for (unsigned step = 0U; step < REPORT_POLL_STEPS; ++step) {
    if (exited == 0) {
      int child_status = 0;
      int completed = waitpid(child, &child_status, WNOHANG);
      if (completed < 0) return -1;
      if (completed == child) {
        if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
          return -1;
        exited = 1;
      }
    }
    struct pollfd item = {.fd = reader, .events = POLLIN};
    int ready = poll(&item, 1U, 10);
    if (ready < 0 || (item.revents & (POLLNVAL | POLLERR)) != 0)
      return -1;
    if ((item.revents & POLLIN) != 0) {
      if (*used >= REPORT_BYTES - 1U) return -1;
      long count = read(reader, report + *used, REPORT_BYTES - 1U - *used);
      if (count < 0) return -1;
      if (count == 0 && exited != 0) return 0;
      if (count > 0) *used += (size_t)count;
    } else if (exited != 0 && (item.revents & POLLHUP) != 0) {
      return 0;
    }
  }
  return -1; /* bounded genuine timeout */
}

static int capture_report(const char *path, const char *verb, const char *arg,
                          char report[REPORT_BYTES]) {
  int ends[2] = {-1, -1};
  if (pipe(ends) != 0) return -1;
  struct spawn_redir redirect = {.child_fd = 1, .parent_fd = ends[1]};
  char *argv[] = {(char *)path, (char *)verb, (char *)arg};
  long pid = OS1low_process_spawn_redir(path, arg == NULL ? 2 : 3, argv,
                                         0U, &redirect, 1);
  int closed_writer = close(ends[1]);
  size_t used = 0U;
  int drained = pid > 0 && closed_writer == 0 ?
                capture_drain(ends[0], (int)pid, report, &used) : -1;
  if (drained != 0 && pid > 0) {
    int killed = kill_process((int)pid);
    if (killed == 0) {
      int reaped = reap_after_kill((int)pid);
      if (reaped != 0) OS1_report_error("gfxcontrol.capture_reap", -EIO);
    }
  }
  report[used] = '\0';
  int closed_reader = close(ends[0]);
  return closed_writer == 0 && closed_reader == 0 && drained == 0 &&
         used > 0U && used < REPORT_BYTES - 1U ? 0 : -1;
}

static int observe_mode(const struct os1_display_mode_v1 *original,
                        struct os1_display_mode_v1 *out) {
  char report[REPORT_BYTES];
  if (capture_report("/sys/bin/nxres", "modes", NULL, report) != 0) return -1;
  char *next = NULL;
  char *line = strtok_r(report, "\n", &next);
  if (line == NULL || strstr(line, "hardware modes") == NULL) return -1;
  uint64_t best = UINT64_MAX;
  unsigned seen = 0U;
  for (unsigned i = 0U; i < OS1_DISPLAY_MODE_MAX; ++i) {
    line = strtok_r(NULL, "\n", &next);
    if (line == NULL) break;
    int index, width, height;
    if (sscanf(line, " %d: %dx%d", &index, &width, &height) != 3 ||
        index != (int)i || width <= 0 || height <= 0) return -1;
    ++seen;
    uint32_t w = (uint32_t)width, h = (uint32_t)height;
    uint64_t area = (uint64_t)w * h;
    int match = original == NULL ? strstr(line, " current") != NULL :
        w >= original->width && h >= original->height &&
        (w != original->width || h != original->height) && area < best;
    if (match) {
      *out = (struct os1_display_mode_v1){.index = i, .width = w, .height = h};
      best = area;
    }
  }
  if (seen == 0U) return -1;
  return best != UINT64_MAX ? 0 : (original == NULL ? -1 : 1);
}

/* A mode SET is a prepare operation. The userland compositor must present a
 * full frame before VirtIO-GPU commits it; CURRENT is the confirmation. */
static int test_wait_current_mode(uint32_t width, uint32_t height) {
  for (unsigned tick = 0U; tick < 150U; ++tick) {
    struct os1_display_mode_v1 mode;
    if (OS1_display_mode_query(0U, &mode) == 0) {
      uint32_t count = mode.count > OS1_DISPLAY_MODE_MAX ?
                       OS1_DISPLAY_MODE_MAX : mode.count;
      for (uint32_t i = 0U; i < OS1_DISPLAY_MODE_MAX && i < count; ++i) {
        if (i != 0U && OS1_display_mode_query(i, &mode) != 0) return -EIO;
        if ((mode.flags & OS1_DISPLAY_MODE_CURRENT) &&
            mode.width == width && mode.height == height) return 0;
      }
    }
    OS1_sleep(20);
  }
  return -EAGAIN; /* The physical scanout did not commit within the test window. */
}

/* Find a fresh index+generation after an atomic switch. A reenumeration can
 * reorder the catalog; original indices are not identities. */
static int test_find_mode(uint32_t width, uint32_t height,
                          uint32_t *index, uint64_t *generation) {
  struct os1_display_mode_v1 mode;
  if (OS1_display_mode_query(0U, &mode) != 0) return -ENODEV;
  uint32_t count = mode.count > OS1_DISPLAY_MODE_MAX ?
                   OS1_DISPLAY_MODE_MAX : mode.count;
  for (uint32_t i = 0U; i < OS1_DISPLAY_MODE_MAX && i < count; ++i) {
    if (i != 0U && OS1_display_mode_query(i, &mode) != 0) return -EIO;
    if (mode.width == width && mode.height == height) {
      *index = i;
      *generation = mode.generation;
      return 0;
    }
  }
  return -ENOENT;
}

/* A true privileged mode+restore exercises the display capability object.
 * An ordinary test caller must receive a real denial on the same operation. */
static int test_mode_control(int level) {
  struct os1_display_mode_v1 original;
  struct os1_display_mode_v1 candidate;
  if (observe_mode(NULL, &original) != 0) return 1;
  int selected = observe_mode(&original, &candidate);
  if (selected > 0) return run_command("/sys/bin/nxres", "modes", NULL, NULL, NULL, 0);
  if (selected < 0) return 2;
  struct os1_display_mode_v1 before;
  if (OS1_display_mode_query(original.index, &before) != 0) return 3;
  if (level > PLVL_ROOT) {
    /* Nonprivileged child must not mutate physical scanout. */
    int denied = (int)OS1_display_mode_set(candidate.index, before.generation);
    if (denied != -EPERM) return 4;
    struct os1_display_mode_v1 unchanged;
    if (observe_mode(NULL, &unchanged) != 0) return 5;
    return unchanged.width == original.width &&
           unchanged.height == original.height ? 0 : 6;
  }
  int changed = (int)OS1_display_mode_set(candidate.index, before.generation);
  if (changed != 0) return 7;
  int committed = test_wait_current_mode(candidate.width, candidate.height);
  /* Never leave a prepared/committed mode behind, even when verification
   * failed. Resolve the original mode against the CURRENT catalog first. */
  uint32_t restore_index = 0U;
  uint64_t restore_generation = 0U;
  int located = test_find_mode(original.width, original.height,
                               &restore_index, &restore_generation);
  if (located != 0) {
    printf("[gfxcontroltest] restore candidate absent err=%d\n", located);
    return 8;
  }
  int restored = (int)OS1_display_mode_set(restore_index, restore_generation);
  if (restored != 0) return 9;
  int restored_visible = test_wait_current_mode(original.width, original.height);
  if (committed != 0 || restored_visible != 0) {
    printf("[gfxcontroltest] mode commit=%d restore=%d (prepare is not a commit)\n",
           committed, restored_visible);
    return 10;
  }
  return 0;
}

static int test_zoom_steps(void) {
  struct os1_display_mode_v1 physical;
  if (observe_mode(NULL, &physical) != 0) return 1;
  const int samples[] = {300, 200, 150, 100};
  int error = 0;
  for (int i = 0; i < 4; ++i) {
    int percent = samples[i];
    int changed = OS1_display_set_zoom(percent);
    int w = 0, h = 0;
    int observed = OS1_display_get_size(&w, &h);
    if (changed != 0 || observed != 0 ||
        w != (int)(physical.width * 100U / (uint32_t)percent) ||
        h != (int)(physical.height * 100U / (uint32_t)percent))
      error = i + 2;
    if (error != 0) break;
  }
  /* Always leave the desktop at its canonical boot scale. */
  int restored = OS1_display_set_zoom(OS1_DISPLAY_ZOOM_DEFAULT_PERCENT);
  if (restored != 0) return 7;
  struct os1_display_mode_v1 after;
  if (observe_mode(NULL, &after) != 0) return 8;
  if (after.width != physical.width || after.height != physical.height) return 9;
  return error;
}

static int test_zoom_contract(void) {
  struct os1_display_mode_v1 physical;
  int logical_w = 0;
  int logical_h = 0;
  if (observe_mode(NULL, &physical) != 0 ||
      OS1_display_get_size(&logical_w, &logical_h) != 0)
    return 1;
  if ((uint32_t)logical_w != physical.width ||
      (uint32_t)logical_h != physical.height)
    return 2;

  char normal[16], below[16], above[16];
  int n1 = snprintf(normal, sizeof(normal), "%d",
                    OS1_DISPLAY_ZOOM_DEFAULT_PERCENT);
  int n2 = snprintf(below, sizeof(below), "%d",
                    OS1_DISPLAY_ZOOM_MIN_PERCENT - 1);
  int n3 = snprintf(above, sizeof(above), "%d",
                    OS1_DISPLAY_ZOOM_MAX_PERCENT + 1);
  if (n1 <= 0 || n2 <= 0 || n3 <= 0)
    return 3;
  if (run_command("/sys/bin/nxres", "zoom", normal, NULL, NULL, 0) != 0)
    return 4;
  if (run_command("/sys/bin/nxres", "zoom", below, NULL, NULL, 1) != 0)
    return 5;
  if (run_command("/sys/bin/nxres", "zoom", above, NULL, NULL, 1) != 0)
    return 6;
  int after_w = 0, after_h = 0;
  if (OS1_display_get_size(&after_w, &after_h) != 0)
    return 7;
  if (after_w != logical_w || after_h != logical_h) {
    struct os1_display_workspace_v1 workspace;
    long snapshot = OS1_display_workspace_query(&workspace);
    int recorded = printf(
           "[gfxcontroltest] zoom mismatch expected=%dx%d actual=%dx%d "
           "workspace_status=%ld mode=%u zoom=%u base=%ux%u physical=%ux%u\n",
           logical_w, logical_h, after_w, after_h, snapshot,
           snapshot == 0 ? workspace.mode : 0U,
           snapshot == 0 ? workspace.zoom_percent : 0U,
           snapshot == 0 ? workspace.base_width : 0U,
           snapshot == 0 ? workspace.base_height : 0U,
           snapshot == 0 ? workspace.physical_width : 0U,
           snapshot == 0 ? workspace.physical_height : 0U);
    if (recorded < 0) OS1_report_error("gfxcontrol.zoom_mismatch", -EIO);
    return 8;
  }
  return test_zoom_steps();
}

static int read_look(enum look_kind kind, char *value, size_t size) {
  if (kind == LOOK_STYLE)
    return nxres_get_style(value, size);
  if (kind == LOOK_THEME)
    return nxres_get_theme(value, size);
  if (kind == LOOK_BACKGROUND)
    return nxres_get_background(value, size);
  return -1;
}

static const char *alternate_name(const char *current,
                                  const char *const names[], int count) {
  if (current == NULL || names == NULL || count < 2)
    return NULL;
  for (int i = 0; i < count; ++i) {
    if (strncmp(current, names[i], 32) != 0)
      return names[i];
  }
  return NULL;
}

static int test_look_property(enum look_kind kind, const char *verb,
                              const char *const names[], int count) {
  char original[32] = {0};
  char changed[32] = {0};
  if (read_look(kind, original, sizeof(original)) != 0)
    return 1;
  const char *alternate = alternate_name(original, names, count);
  if (alternate == NULL)
    return 2;

  int status =
      run_command("/sys/bin/nxres", verb, alternate, NULL, NULL, 0);
  if (status == 0 && read_look(kind, changed, sizeof(changed)) != 0)
    status = 3;
  if (status == 0 && strncmp(changed, alternate, sizeof(changed)) != 0)
    status = 4;

  int restored =
      run_command("/sys/bin/nxres", verb, original, NULL, NULL, 0);
  if (restored == 0 && read_look(kind, changed, sizeof(changed)) != 0)
    restored = 5;
  if (restored == 0 && strncmp(changed, original, sizeof(changed)) != 0)
    restored = 6;
  return status != 0 ? status : restored;
}

static int inspect_window(struct os1_window_snapshot_v1 *snapshot) {
  char id[16], report[REPORT_BYTES], state[16];
  if (snprintf(id, sizeof(id), "%d", snapshot->id) <= 0)
    return -1;
  int captured = capture_report("/sys/bin/nxwins", "inspect", id, report);
  if (captured != 0) {
    int written = printf("[gfxcontroltest] nxwins inspect capture failed id=%d rc=%d\n",
                         snapshot->id, captured);
    if (written < 0) OS1_report_error("gfxcontrol.inspect_capture", -EIO);
    return -1;
  }
  int observed_id = 0;
  int fields = sscanf(report, "%d %d %15s pos=%d,%d surface=%dx%d draw=%dx%d",
      &observed_id, &snapshot->pid, state, &snapshot->x, &snapshot->y,
      &snapshot->width, &snapshot->height, &snapshot->draw_width, &snapshot->draw_height);
  if (fields != 9 || observed_id != snapshot->id || snapshot->width <= 0 ||
      snapshot->height <= 0) {
    int written = printf(
        "[gfxcontroltest] nxwins inspect parse failed id=%d seen=%d fields=%d "
        "report=%.150s\n", snapshot->id, observed_id, fields, report);
    if (written < 0) OS1_report_error("gfxcontrol.inspect_parse", -EIO);
    return -1;
  }
  snapshot->flags = strcmp(state, "focus") == 0 ? WININFO_FOCUSED : 0U;
  return 0;
}

static int focused_foreign_window(struct os1_window_snapshot_v1 *snapshot) {
  struct window_info windows[32];
  long count = OS1_window_enum(windows, 32U);
  int self = get_pid();
  if (snapshot == NULL || count < 0)
    return -1;
  for (int i = 0; i < 32 && (long)i < count; ++i) {
    if ((windows[i].flags & WININFO_FOCUSED) == 0 || windows[i].pid == self)
      continue;
    *snapshot = (struct os1_window_snapshot_v1){
        .abi_version = OS1_COMPOSITOR_ABI_V1,
        .struct_size = sizeof(*snapshot),
        .id = windows[i].id};
    return inspect_window(snapshot) == 0 ? 0 : -2;
  }
  return -3;
}

static int nxwins_read_paths(const struct os1_window_snapshot_v1 *snapshot,
                             const char *id) {
  if (run_command("/sys/bin/nxwins", NULL, NULL, NULL, NULL, 0) != 0)
    return 1;
  if (run_command("/sys/bin/nxwins", "inspect", id, NULL, NULL, 0) != 0)
    return 2;
  if (run_command("/sys/bin/nxwins", "focus", id, NULL, NULL, 0) != 0)
    return 3;
  struct os1_window_snapshot_v1 after = {
      .abi_version = OS1_COMPOSITOR_ABI_V1,
      .struct_size = sizeof(after),
      .id = snapshot->id};
  if (inspect_window(&after) != 0)
    return 4;
  return (after.flags & WININFO_FOCUSED) != 0U ? 0 : 5;
}

static int direct_window_denials(const struct os1_window_snapshot_v1 *snap) {
  int draw_w = snap->draw_width > 0 ? snap->draw_width : snap->width;
  int draw_h = snap->draw_height > 0 ? snap->draw_height : snap->height;
  if (OS1_window_move(snap->id, snap->x, snap->y) != -EPERM)
    return 1;
  if (OS1_window_resize(snap->id, snap->width, snap->height) != -EPERM)
    return 2;
  if (OS1_window_present_resize(snap->id, draw_w, draw_h) != -EPERM)
    return 3;
  return 0;
}

static int nxwins_denial_paths(const struct os1_window_snapshot_v1 *snap,
                               const char *id) {
  char x[16], y[16], width[16], height[16];
  if (snprintf(x, sizeof(x), "%d", snap->x) <= 0 ||
      snprintf(y, sizeof(y), "%d", snap->y) <= 0 ||
      snprintf(width, sizeof(width), "%d", snap->width) <= 0 ||
      snprintf(height, sizeof(height), "%d", snap->height) <= 0)
    return 1;
  if (run_command("/sys/bin/nxwins", "move", id, x, y, 1) != 0)
    return 2;
  if (run_command("/sys/bin/nxwins", "resize", id, width, height, 1) != 0)
    return 3;
  int draw_w = snap->draw_width > 0 ? snap->draw_width : snap->width;
  int draw_h = snap->draw_height > 0 ? snap->draw_height : snap->height;
  if (snprintf(width, sizeof(width), "%d", draw_w) <= 0 ||
      snprintf(height, sizeof(height), "%d", draw_h) <= 0)
    return 4;
  return run_command("/sys/bin/nxwins", "present", id, width, height, 1) == 0
             ? 0
             : 5;
}

static int test_window_control(void) {
  struct os1_window_snapshot_v1 before;
  if (focused_foreign_window(&before) != 0)
    return 1;
  char id[16];
  if (snprintf(id, sizeof(id), "%d", before.id) <= 0)
    return 2;
  int read_paths = nxwins_read_paths(&before, id);
  if (read_paths != 0)
    return 10 + read_paths;
  int direct = direct_window_denials(&before);
  if (direct != 0)
    return 20 + direct;
  int commands = nxwins_denial_paths(&before, id);
  if (commands != 0)
    return 30 + commands;

  struct os1_window_snapshot_v1 after = {
      .abi_version = OS1_COMPOSITOR_ABI_V1,
      .struct_size = sizeof(after),
      .id = before.id};
  if (inspect_window(&after) != 0)
    return 40;
  return before.x == after.x && before.y == after.y &&
                 before.width == after.width && before.height == after.height &&
                 before.draw_width == after.draw_width &&
                 before.draw_height == after.draw_height
             ? 0
             : 41;
}

/* Real kernel resource acceptance: inspect current mode, mutate logical-only
 * dimensions, verify independent scanout, then restore the exact prior policy. */
static int test_workspace_manual(int level) {
  struct os1_display_workspace_v1 before;
  if (OS1_display_workspace_query(&before) != 0) return 1;
  long result = OS1_display_workspace_set(OS1_DISPLAY_WORKSPACE_MANUAL,
                                          960U, 600U, 125U);
  if (level > PLVL_ROOT)
    return result == -EPERM ? 0 : 2;
  int failure = result != 0 ? 3 : 0;
  struct os1_display_workspace_v1 after;
  if (failure == 0 && OS1_display_workspace_query(&after) != 0)
    failure = 4;
  if (failure == 0 &&
      (after.mode != OS1_DISPLAY_WORKSPACE_MANUAL ||
       after.base_width != 960U || after.base_height != 600U ||
       after.logical_width != 768U || after.logical_height != 480U ||
       after.physical_width != before.physical_width ||
       after.physical_height != before.physical_height))
    failure = 5;
  long restored = OS1_display_workspace_set(before.mode,
       before.mode == OS1_DISPLAY_WORKSPACE_AUTO ? 0U : before.base_width,
       before.mode == OS1_DISPLAY_WORKSPACE_AUTO ? 0U : before.base_height,
       before.zoom_percent);
  if (restored != 0) return 6;
  if (OS1_display_workspace_query(&after) != 0) return 7;
  if (after.mode != before.mode || after.zoom_percent != before.zoom_percent ||
      after.logical_width != before.logical_width ||
      after.logical_height != before.logical_height) return 8;
  return failure;
}

static int test_workspace_validation(int level) {
  struct os1_display_workspace_v1 before, after;
  if (OS1_display_workspace_query(&before) != 0) return 1;
  long bad_auto = OS1_display_workspace_set(
      OS1_DISPLAY_WORKSPACE_AUTO, 1024U, 0U, 100U);
  long bad_manual = OS1_display_workspace_set(
      OS1_DISPLAY_WORKSPACE_MANUAL, 0U, 900U, 100U);
  long bad_zoom = OS1_display_workspace_set(
      OS1_DISPLAY_WORKSPACE_AUTO, 0U, 0U, 99U);
  if (level <= PLVL_ROOT) {
    if (bad_auto != -EINVAL || bad_manual != -ERANGE ||
        bad_zoom != -EINVAL) return 2;
  } else if (bad_auto != -EPERM || bad_manual != -EPERM ||
             bad_zoom != -EPERM) return 3;
  if (OS1_display_workspace_query(&after) != 0) return 4;
  return before.mode == after.mode &&
         before.zoom_percent == after.zoom_percent &&
         before.logical_width == after.logical_width &&
         before.logical_height == after.logical_height ? 0 : 5;
}

/* Exercise the real resource-backed plugin contract, not a fake UI layer.
 * This test verifies edge anchors, workarea reservation, overlay ordering,
 * and that removing a plugin automatically releases its reserved region. */
static int test_plugin_anchors(int level) {
  struct os1_surface_layout_v1 before, current, top, bottom;
  if (OS1_surface_layout_query(0, &before) != 0) return 1;
  int first = create_window(0, 0, 240, 40, "layout-probe-top");
  if (first <= 0) return 2;
  long result = OS1_surface_layout_apply(
      first, OS1_SURFACE_LAYOUT_ANCHOR_TOP,
      OS1_SURFACE_LAYOUT_LAYER_OVERLAY, 29U, 3U, 90U, 240U, 40U);
  if (level > PLVL_ROOT) {
    destroy_window(first);
    return result == -EPERM ? 0 : 3;
  }
  int failure = result == 0 ? 0 : 4;
  int second = create_window(0, 0, 240, 44, "layout-probe-bottom");
  if (second <= 0) failure = 5;
  if (second > 0 && failure == 0) {
    result = OS1_surface_layout_apply(second,
          OS1_SURFACE_LAYOUT_ANCHOR_BOTTOM,
          OS1_SURFACE_LAYOUT_LAYER_OVERLAY, 28U, 5U, 75U, 240U, 44U);
    if (result != 0) failure = 6;
  }
  if (failure == 0 &&
      (OS1_surface_layout_query(first, &top) != 0 ||
       OS1_surface_layout_query(second, &bottom) != 0 ||
       OS1_surface_layout_query(0, &current) != 0)) failure = 7;
  if (failure == 0 &&
      (top.x != ((int)current.work_width - 240) / 2 ||
       top.y != 3 || bottom.y != (int)current.work_y +
          (int)current.work_height + 80 - 5 - 44 ||
       current.work_y < 93 || current.work_height >= before.work_height))
    failure = 8;
  struct os1_window_snapshot_v1 a = {
      .abi_version = OS1_COMPOSITOR_ABI_V1, .struct_size = sizeof(a),
      .id = first};
  struct os1_window_snapshot_v1 b = {
      .abi_version = OS1_COMPOSITOR_ABI_V1, .struct_size = sizeof(b),
      .id = second};
  if (failure == 0 && (OS1_window_snapshot(&a) != 0 ||
                       OS1_window_snapshot(&b) != 0 ||
                       a.z_order <= b.z_order)) failure = 9;
  if (second > 0) destroy_window(second);
  destroy_window(first);
  if (OS1_surface_layout_query(0, &current) != 0) return 10;
  if (current.work_y != before.work_y ||
      current.work_height != before.work_height) return 11;
  return failure;
}

/* A bottom-anchored plugin must move together with logical Auto/Manual
 * changes before the owning userland service has a chance to poll them. */
static int test_plugin_workspace_reflow(int level) {
  if (level > PLVL_ROOT) return 0;
  struct os1_display_workspace_v1 original;
  if (OS1_display_workspace_query(&original) != 0) return 1;
  int id = create_window(0, 0, 200, 36, "layout-probe-reflow");
  if (id <= 0) return 2;
  int failure = 0;
  long rc = OS1_surface_layout_apply(
      id, OS1_SURFACE_LAYOUT_ANCHOR_BOTTOM,
      OS1_SURFACE_LAYOUT_LAYER_OVERLAY, 27U, 4U, 36U, 200U, 36U);
  if (rc != 0) failure = 3;
  if (failure == 0 && OS1_display_workspace_set(
       OS1_DISPLAY_WORKSPACE_MANUAL, 800U, 600U, 100U) != 0)
    failure = 4;
  struct os1_surface_layout_v1 layout;
  if (failure == 0 && OS1_surface_layout_query(id, &layout) != 0)
    failure = 5;
  if (failure == 0 &&
      (layout.x != 300 || layout.y != 560 ||
       layout.work_width != 800U)) failure = 6;
  long restored = OS1_display_workspace_set(original.mode,
      original.mode == OS1_DISPLAY_WORKSPACE_AUTO ? 0U : original.base_width,
      original.mode == OS1_DISPLAY_WORKSPACE_AUTO ? 0U : original.base_height,
      original.zoom_percent);
  destroy_window(id);
  if (restored != 0) return 7;
  return failure;
}

/* Use the actual compositor geometry and object-backed snapshot rather
 * than fixed QEMU pixels. Oversize windows must keep their source pixels
 * while the presentation extent fits inside the current reserved workarea. */
static int test_normal_window_fit(void) {
  struct os1_surface_layout_v1 area;
  if (OS1_surface_layout_query(0, &area) != 0) return 1;
  if (area.work_width < 160U || area.work_height < 160U) return 2;
  int wanted_w = area.work_width < 3800U ?
                      (int)area.work_width + 128 : 4096;
  int wanted_h = area.work_height < 3800U ?
                      (int)area.work_height + 128 : 4096;
  int id = create_window(-200, -200, wanted_w, wanted_h, "layout-probe-fit");
  if (id <= 0) return 3;
  struct os1_window_snapshot_v1 snap = {
      .abi_version = OS1_COMPOSITOR_ABI_V1, .struct_size = sizeof(snap),
      .id = id};
  int failure = OS1_window_snapshot(&snap) == 0 ? 0 : 4;
  if (failure == 0 &&
      (snap.width != wanted_w || snap.height != wanted_h ||
       snap.draw_width > (int)area.work_width ||
       snap.draw_height > (int)area.work_height ||
       snap.x < area.work_x || snap.y < area.work_y ||
       snap.x + snap.draw_width > area.work_x + (int)area.work_width ||
       snap.y + snap.draw_height >
           area.work_y + (int)area.work_height)) failure = 5;
  int resized = OS1_window_resize(id, 320, 160);
  if (failure == 0 && resized != 0) failure = 6;
  if (failure == 0 &&
      (OS1_window_snapshot(&snap) != 0 || snap.width != 320 ||
       snap.height != 160 || snap.draw_width != 320 ||
       snap.draw_height != 160)) failure = 7;
  destroy_window(id);
  return failure;
}

/* Even a ROOT client cannot borrow another process's surface registration.
 * The resource capability is intentionally weaker than WINDOW_MANAGE. */
static int test_plugin_foreign_owner(void) {
  struct window_info windows[32];
  long count = OS1_window_enum(windows, 32U);
  if (count < 0) return 1;
  int foreign = 0;
  for (long i = 0; i < count && i < 32L; ++i) {
    if (foreign == 0 && windows[i].pid != get_pid() &&
        windows[i].id > 0) foreign = windows[i].id;
  }
  if (foreign == 0) return 2;
  struct os1_surface_layout_v1 before, after;
  if (OS1_surface_layout_query(foreign, &before) != 0) return 3;
  long status = OS1_surface_layout_apply(
      foreign, OS1_SURFACE_LAYOUT_ANCHOR_TOP,
      OS1_SURFACE_LAYOUT_LAYER_OVERLAY, 16U, 0U, 26U, 200U, 26U);
  if (status != -EPERM) return 4;
  if (OS1_surface_layout_query(foreign, &after) != 0) return 5;
  return before.x == after.x && before.y == after.y &&
      before.anchor == after.anchor && before.layer == after.layer &&
      before.order == after.order && before.reserved == after.reserved
      ? 0 : 6;
}

static int report_case(const char *name, int status) {
  if (printf("[gfxcontroltest] %-24s %s (%d)\n", name,
             status == 0 ? "PASS" : "FAIL", status) < 0)
    return 1;
  return status == 0 ? 0 : 1;
}

int main(void) {
  int level = PLVL_GUEST;
  unsigned int caps = 0U;
  if (OS1_identity(&level, &caps) != 0 || (caps & CAP_WINDOW) == 0U)
    return 1;
  int failures = 0;
  failures += report_case(level <= PLVL_ROOT ? "ROOT provider mode+restore"
                                               : "USER mode write denied",
                          test_mode_control(level));
  failures += report_case("zoom bounds + real steps", test_zoom_contract());
  failures += report_case(
      "style mutate + restore",
      test_look_property(LOOK_STYLE, "style", os1_style_names, OS1_STYLE_COUNT));
  failures += report_case(
      "theme mutate + restore",
      test_look_property(LOOK_THEME, "theme", os1_theme_names, OS1_THEME_COUNT));
  failures += report_case("background + restore",
                          test_look_property(LOOK_BACKGROUND, "bg",
                                             os1_bg_names, OS1_BG_COUNT));
  failures += report_case("nxwins authority", test_window_control());
  failures += report_case("workspace manual + restore", test_workspace_manual(level));
  failures += report_case("workspace invalid requests", test_workspace_validation(level));
  failures += report_case("plugin anchors/workarea/Z", test_plugin_anchors(level));
  failures += report_case("plugin workspace reflow", test_plugin_workspace_reflow(level));
  failures += report_case("normal window workarea fit", test_normal_window_fit());
  failures += report_case("foreign plugin owner denied", test_plugin_foreign_owner());
  if (printf("[gfxcontroltest] done: %d/12 passed, %d failure(s)\n",
             12 - failures, failures) < 0)
    return 1;
  return failures;
}
