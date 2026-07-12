#ifndef _USER_NXRES_H
#define _USER_NXRES_H

/*
 * user/sys/bin/nxres.h
 * Compositor look (style/theme/background) — single canonical getter/setter
 * pair, shared by every consumer instead of each one re-reading raw registry
 * keys and re-deriving id<->name mappings on its own (nxsettings, nxbar,
 * nxntfy_srv, nxui and nxlauncher used to do this independently, and only
 * nxres.c's own copy was ever kept in sync with the kernel enum order).
 *
 * Same pattern as nxinfo.h/nxperm.h/nxproc.h: a header-only, static-inline
 * helper layer over the plain OS1_ syscalls, dropped into any translation
 * unit that needs it.
 *
 * Ground truth: kernel/core/syscall_dispatch.c's SYS_SET_STYLE handler
 * writes style.name/theme.color/background.name into the registry itself on
 * every successful change (any caller, not just nxres) — see
 * kernel/lib/registry.c registry_caller_owner() for why a ROOT-level service
 * changing the look does not get its write rejected as "not the owner".
 * The registry mirror here is kept as a second write for callers running
 * against a kernel build that predates that mirror; it is idempotent
 * (same value) against a kernel that already wrote it.
 *
 * Change propagation: a successful nxres_set_*() posts through the EXISTING
 * async notification transport — send()/IPC_TYPE_NOTIFY, the exact
 * mechanism nxntfy_srv.c already uses for popups — instead of a new bespoke
 * broadcast mechanism or per-frame registry polling:
 *   - the user-facing popup goes to srv.notify_pid via notify(), same as
 *     any other system notification;
 *   - a SILENT ping (same IPC_TYPE_NOTIFY, tagged with IPC_LOOK_PING_MAGIC
 *     in data2, posix_types.h, so it is never confused with a real user
 *     notification) goes to every singleton system service's registered
 *     pid — srv.dock_pid (nxui), srv.bar_pid (nxbar), srv.launcher_pid
 *     (nxlauncher) — published by init the same way it already publishes
 *     srv.notify_pid (register_service_pid, user/sys/bin/nxinit.c) and
 *     refreshed on every respawn so the ping can never land on a corpse's
 *     stale pid.  A windowed app sees this ping as INPUT_TYPE_LOOK_CHANGED
 *     from its EXISTING input_poll_event() loop (input.h/lib.c) — NOT a
 *     second try_recv() loop: input_poll_event() is the single consumer of
 *     an app's IPC mailbox, and a competing try_recv() elsewhere in the
 *     same app would silently steal keyboard/mouse messages before
 *     input_poll_event ever saw them (the bug in this feature's first cut).
 *     nxntfy_srv is the one exception — it has no input_poll_event loop of
 *     its own (its window is passive/click-through), so it checks
 *     msg.data2 directly in its own recv()/try_recv() loop, which already
 *     is that app's single mailbox consumer.
 * nxsettings is on the same ping list even though it is not an init-
 * supervised singleton: it self-registers "srv.settings_pid" when its
 * window opens (main_gui() in nxsettings.c) so an EXTERNAL change (made via
 * `nxres` from a shell, or by another app) still repaints its already-open
 * window instead of only taking effect the next time it is opened. */
#include <os1.h>
#include <string.h>
#include <style_names.h>

/* The well-known registry keys init.c publishes each singleton system
 * service's live pid under (register_service_pid).  srv.notify_pid is
 * deliberately NOT here: it already gets the user-facing notify() popup
 * above, and a second silent ping would just be a wasted send(). */
static const char *const nxres_look_ping_targets[] = {
    "srv.dock_pid",
    "srv.bar_pid",
    "srv.launcher_pid",
    /* nxsettings is not init-supervised (user-launched, not a singleton
     * service), so it self-registers this key on window creation instead of
     * init publishing it — see main_gui() in nxsettings.c.  Last-opened
     * instance wins if more than one is ever open at once. */
    "srv.settings_pid",
};
#define NXRES_LOOK_PING_NTARGETS                                               \
  (int)(sizeof(nxres_look_ping_targets) / sizeof(nxres_look_ping_targets[0]))

/* nxres_broadcast_look - silent IPC_TYPE_NOTIFY ping to every registered
 * system service so its already-running event loop can re-derive its
 * palette.  Best-effort: a service that is not up yet (registry key absent)
 * or whose pid is momentarily stale (mid-respawn) is simply skipped — the
 * next nxres_set_*() call (or the service's own startup read) catches it. */
static inline void nxres_broadcast_look(void) {
  for (int i = 0; i < NXRES_LOOK_PING_NTARGETS; i++) {
    char pidbuf[16];
    if (OS1_registry_get(nxres_look_ping_targets[i], pidbuf, sizeof(pidbuf)) !=
        0)
      continue;
    int pid = atoi(pidbuf);
    if (pid <= 0)
      continue;
    struct ipc_message msg = {0};
    msg.type = IPC_TYPE_NOTIFY;
    msg.data2 = IPC_LOOK_PING_MAGIC;
    send(pid, &msg);
  }
}

/* nxres_get_style/theme/background - the current APPLIED look, as last
 * mirrored into the registry by SYS_SET_STYLE (kernel-side) or a prior
 * nxres_set_*() call.  Returns 0 and fills 'buf', or -1 if the key is
 * absent (never set since boot — should not happen post registry_init). */
static inline int nxres_get_style(char *buf, size_t sz) {
  return OS1_registry_get("style.name", buf, sz);
}
static inline int nxres_get_theme(char *buf, size_t sz) {
  return OS1_registry_get("theme.color", buf, sz);
}
static inline int nxres_get_background(char *buf, size_t sz) {
  return OS1_registry_get("background.name", buf, sz);
}

/* nxres_theme_is_light - the one boolean every app's palette switch actually
 * needs.  Defaults to dark (0) on any read failure or unrecognised value,
 * matching registry_init's "dark" default. */
static inline int nxres_theme_is_light(void) {
  char val[8] = {0};
  return nxres_get_theme(val, sizeof(val)) == 0 &&
         strncmp(val, "light", 6) == 0;
}

/* nxres_name_to_id - linear lookup in one of the os1_*_names[] tables
 * (style_names.h).  Exact match up to and including the NUL; -1 if absent. */
static inline int nxres_name_to_id(const char *name, const char *const *list,
                                   int n) {
  for (int i = 0; i < n; i++)
    if (strncmp(name, list[i], 32) == 0)
      return i;
  return -1;
}

/* nxres_set_style/theme/background - resolve 'name' against the shared
 * tables, apply it via SYS_SET_STYLE (OS1_display_set_style/background),
 * and mirror the name into the registry on success.  Returns 0 on success,
 * -1 for an unknown name, -2 if the syscall itself failed. */
static inline int nxres_set_style(const char *name) {
  int id = nxres_name_to_id(name, os1_style_names, OS1_STYLE_COUNT);
  if (id < 0)
    return -1;
  if (OS1_display_set_style(id, -1) != 0)
    return -2;
  OS1_registry_set("style.name", os1_style_names[id]);
  notify("Style", os1_style_names[id]);
  nxres_broadcast_look();
  return 0;
}
static inline int nxres_set_theme(const char *name) {
  int id = nxres_name_to_id(name, os1_theme_names, OS1_THEME_COUNT);
  if (id < 0)
    return -1;
  if (OS1_display_set_style(-1, id) != 0)
    return -2;
  OS1_registry_set("theme.color", os1_theme_names[id]);
  notify("Theme", os1_theme_names[id]);
  nxres_broadcast_look();
  return 0;
}
static inline int nxres_set_background(const char *name) {
  int id = nxres_name_to_id(name, os1_bg_names, OS1_BG_COUNT);
  if (id < 0)
    return -1;
  if (OS1_display_set_background(id) != 0)
    return -2;
  OS1_registry_set("background.name", os1_bg_names[id]);
  notify("Background", os1_bg_names[id]);
  nxres_broadcast_look();
  return 0;
}

#endif /* _USER_NXRES_H */
