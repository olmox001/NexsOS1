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
    /* Same self-registration pattern as nxsettings: nxfilem is user-launched
     * (not a singleton), so it publishes its own pid here on window
     * creation (see fm_ui_init in ui.c) so an external style/theme/bg change
     * repaints its already-open window instead of only the next launch. */
    "srv.filem_pid",
    /* nxshell self-registers srv.shell_pid on window creation (nxshell.c);
     * last-opened shell wins if several are open — others still track theme
     * via the per-loop nxres_theme_is_light() poll in the shell event loop. */
    "srv.shell_pid",
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

/* os1_bg_colors - the 16 background anchor colours (compositor's
 * backgrounds[].bg_color field, not the gradient stops) as parallel ARGB
 * literals, indexed by the same id that BG_* enums use in kernel/graphics/
 * compositor_style.h.  Same role as os1_bg_names above: the userland-
 * facing name table that the kernel's style/theme/background mirror in the
 * registry is keyed by.  The pattern — kernel enum + kernel struct +
 * style_names.h os1_bg_names + this os1_bg_colors — is the SAME pattern
 * the codebase already established for the *names*, just extended one step
 * further so a userland app that wants to paint against the current
 * background does not have to invent its own colour-resolution code
 * (nxbar's "X" glyph, nxui's dock-tile bevel, any future cutout button
 * that should read as a hole through to the desktop).  Order MUST match
 * os1_bg_names above.  If a background is added, the chain to update is
 * identical to the names' chain: kernel enum -> kernel backgrounds[] ->
 * style_names.h (here) -> os1_bg_names -> os1_bg_colors. */
static const unsigned int os1_bg_colors[] = {
    0xFF121212u, /* BG_BLACK         */
    0xFFB33A3Au, /* BG_RED           */
    0xFF3E8E5Bu, /* BG_GREEN         */
    0xFFC9A227u, /* BG_YELLOW        */
    0xFF143060u, /* BG_BLUE          */
    0xFF8E44ADu, /* BG_MAGENTA       */
    0xFF2A9D8Fu, /* BG_CYAN          */
    0xFFD8D8DCu, /* BG_WHITE         */
    0xFF4A4A52u, /* BG_GRAY          */
    0xFFE8604Fu, /* BG_BRIGHT_RED    */
    0xFF52B788u, /* BG_BRIGHT_GREEN  */
    0xFFF2C94Cu, /* BG_BRIGHT_YELLOW */
    0xFF4A78C0u, /* BG_BRIGHT_BLUE   */
    0xFFB07CD1u, /* BG_BRIGHT_MAGENTA*/
    0xFF56C9C9u, /* BG_BRIGHT_CYAN   */
    0xFFF5F5F7u, /* BG_BRIGHT_WHITE  */
};
#define OS1_BG_COLORS_COUNT                                                    \
  (int)(sizeof(os1_bg_colors) / sizeof(os1_bg_colors[0]))

/* nxres_bg_color - the ARGB colour of the currently applied background,
 * as the userland-side lookup against the registry's "background.name"
 * key (the same name every other app — nxsettings, nxlauncher, etc. —
 * already reads).  Resolves the name to an id via the shared
 * os1_bg_names table, then reads the colour out of the parallel
 * os1_bg_colors table — the same two-step name-to-X pattern nxsettings
 * already uses for background NAMES (NXS_BG = os1_bg_names), just
 * extended one step further so callers can pick the actual anchor
 * colour without re-implementing the table themselves.  Returns the dark
 * default (0xFF1C1C24u) on any read failure, unknown name, or
 * out-of-range id, matching the dark default every other "look" getter
 * in this header falls back to.  Inherits the change-propagation path of
 * the rest of the look: a successful nxres_set_background() pings every
 * registered service via INPUT_TYPE_LOOK_CHANGED, and the app re-derives
 * its palette on that ping. */
static inline unsigned int nxres_bg_color(void) {
  char val[32] = {0};
  if (nxres_get_background(val, sizeof(val)) != 0)
    return 0xFF1C1C24u; /* read failed */
  for (int i = 0; i < OS1_BG_COUNT && i < OS1_BG_COLORS_COUNT; i++) {
    if (strncmp(val, os1_bg_names[i], 32) == 0)
      return os1_bg_colors[i];
  }
  return 0xFF1C1C24u; /* unknown name */
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
