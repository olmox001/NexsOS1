/*
 * include/abi/style_names.h
 * Single source of truth for the compositor style/theme/background NAME
 * tables (as opposed to the numeric ids in kernel/include/kernel/
 * compositor_style.h, which userland never includes).
 *
 * Order MUST match the STYLE_/THEME_/BG_ enums in compositor_style.h;
 * these are parallel arrays indexed by the same small ints that travel over
 * SYS_SET_STYLE (arg0/arg1/arg2).  Previously duplicated verbatim in
 * nxres.c, nxsettings.c and the kernel registry-write side; a change to
 * one used to silently desync from the others.
 *
 * Included by both kernel (kernel/core/syscall_dispatch.c, to publish the
 * name into the registry after a successful style change) and userland
 * (nxres.c, nxsettings.c, ...), so it must stay freestanding (no libc calls,
 * just static const data) and safe to include in multiple translation
 * units.
 */
#ifndef _API_STYLE_NAMES_H
#define _API_STYLE_NAMES_H

static const char *const os1_style_names[] = {
    "nexs", "classic", "material", "glass", "minimal", "retro",
};
#define OS1_STYLE_COUNT \
  (int)(sizeof(os1_style_names) / sizeof(os1_style_names[0]))

static const char *const os1_theme_names[] = {
    "light", "dark",
};
#define OS1_THEME_COUNT \
  (int)(sizeof(os1_theme_names) / sizeof(os1_theme_names[0]))

static const char *const os1_bg_names[] = {
    "black", "red",      "green", "yellow", "blue",   "magenta",
    "cyan",  "white",    "gray",  "bred",   "bgreen", "byellow",
    "bblue", "bmagenta", "bcyan", "bwhite",
};
#define OS1_BG_COUNT (int)(sizeof(os1_bg_names) / sizeof(os1_bg_names[0]))

#endif /* _API_STYLE_NAMES_H */
