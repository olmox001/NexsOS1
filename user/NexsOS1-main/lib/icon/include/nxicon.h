#ifndef _USER_NXICON_H
#define _USER_NXICON_H

/*
 * user/sys/bin/nxicon.h
 * Dock/launcher icon loading and caching (GFX-NXUI-02 follow-up).
 *
 * Icons live on the VFS at /home/Pictures/icon/{dark,light}/<name>.png — a
 * curated, OS-shipped asset set (NOT arbitrary user images), two full sets so a
 * theme flip never needs to touch disk again once both are warm.  This header
 * owns the one place that knows those paths, which app a window title/exe path
 * maps to, and how the tile-sized ARGB copy is cached — nxui.c (and later
 * nxlauncher.c) just calls nxicon_get() and draws whatever it returns.
 *
 * Same header-only static-inline pattern as nxres.h/nxexec.h: dropped into
 * any translation unit that needs it, no separate .c/link step.
 *
 * TOGGLE (NXICON_ENABLE): compile-time only, on purpose.  With it at 0,
 * nxicon_get() always returns NULL and every caller's existing classic-tile
 * fallback (flat rounded-square fill, unchanged since GFX-NXUI-02) runs
 * instead — one guarded lookup, not a second maintained code path.  A
 * missing/undecodable individual icon degrades the SAME way at runtime
 * (nxicon_get returns NULL for just that one), so "some icons installed"
 * and "icons off entirely" are handled by the exact same caller-side check.
 *
 * SAFETY NOTE: nxicon_get() still calls os1_image_load(), which still runs
 * the full sanitizing decode in graphics_load_image() (bounded read off a
 * capability-gated handle, stbi_info probe, dimension/pixel caps) — that
 * path is not weakened for icons.  What IS skipped, because these are known
 * square OS-shipped assets rather than arbitrary user photos, is nximage.c's
 * aspect-preserving os1_image_fit_size()/os1_image_resample_fit() dance:
 * icons resample straight to tile_size x tile_size, once, then are cached —
 * never re-decoded or re-fit per frame, which matters here because nxui
 * redraws potentially many tiles at ~30Hz.
 */

#include <image.h>
#include <os1.h>
#include <string.h>

#ifndef NXICON_ENABLE
#define NXICON_ENABLE 1
#endif

/* App identity an icon is drawn for.  NXICON_UNKNOWN is a real, drawable icon
 * (the generic "unknown app" glyph, backed by empty.png) — not to be
 * confused with nxicon_get() returning NULL, which means "no icon available
 * at all, use the classic flat tile".
 *
 * One id per compiled program that has a matching asset in the icon set.
 * Two naming mismatches worth flagging explicitly since they're easy to
 * "fix" by mistake later:
 *   - NXICON_MEMSTAT's binary is nxmemstat, but its icon is nxmeminfo.png.
 *   - NXICON_TEXTEDIT's binary is kilo (the editor from user/bin), but its
 *     icon is textedit.png — there is no "kilo.png".
 * ICON POLICY: an icon is optional, never required.  Nothing may depend on a
 * program having one — an unmapped program gets NXICON_UNKNOWN (the preset
 * fallback glyph), and a missing asset gets NULL (classic flat tile).  Visible
 * apps AND system/diagnostic tools (even CLI ones) are mapped:
 *   apps:     the launcher/dock apps above, nxnotify (bell), nxempire
 *             (gamepad, like doom/raptor), demo3d (mesh), lua (nxscript.png),
 *             kilo (textedit).
 *   tools:    nxres (expand), nxkeymap (number), nxproc (rows), nxwins
 *             (maximize), nxenvinit (modifier), nxtimerdiag (clock),
 *             nxpax (archive), nxtestrunner and nxtest_userkit_* (notifOk),
 *             nxroleprobe (search), nxfont (curve).
 *   unmapped on purpose (-> NXICON_UNKNOWN):
 *     - hidden from the launcher: nxbar, nxui, nxexec, env, nxinit, nxntfy_srv;
 *     - examples/tests/helpers in /bin: counter, hello, sdltest, input_test,
 *       ipc_send/ipc_recv, forkbomb, *test, crash, stress, cap*, sandbox*, ...;
 *     - the 51 Coreutils applets and the other external test suites.
 * Add a program here only if it is a real, user-facing app. */
#define NXICON_LAUNCHER 0
#define NXICON_SHELL 1
#define NXICON_FILEM 2
#define NXICON_IMAGE 3
#define NXICON_INFO 4
#define NXICON_MEMSTAT 5
#define NXICON_PERM 6
#define NXICON_POWER 7
#define NXICON_REG 8
#define NXICON_SETTINGS 9
#define NXICON_TOP 10
#define NXICON_DOOM 11
#define NXICON_RAPTOR 12
#define NXICON_TEXTEDIT 13
/* Per-FILETYPE icons (by extension/kind, NOT by app id) — landed for
 * nxfilem's content list, see nxicon_classify_file() below.  folder.png is
 * used for directories; the file_* set covers the extension buckets
 * nxfilem's association table (nxassoc.h) already distinguishes. */
#define NXICON_FOLDER 14
#define NXICON_FILE_C 15
#define NXICON_FILE_TEXT 16
#define NXICON_FILE_CONFIG 17
#define NXICON_FILE_HTML 18
#define NXICON_FILE_SCRIPT 19
#define NXICON_FILE_IMAGE 20
#define NXICON_FILE_VIDEO 21
#define NXICON_FILE_BINARY 22
#define NXICON_FILE_EXECUTABLE 23
#define NXICON_FILE_GENERIC 24
#define NXICON_UNKNOWN 25
#define NXICON_NXSCRIPT 26
/* HydroGlass catalog (system m5 + UI m8): one id per asset, NXICON_G_<NAME>,
 * file = /home/Pictures/icon/{dark,light}/<name>.png (dark = coloured, light =
 * grey).  The legacy ids above (app/filetype ids) now point at these same
 * files; resolve any asset by file name with nxicon_find(). */
#define NXICON_G_ANIMATION 27
#define NXICON_G_ARCHIVE 28
#define NXICON_G_ARMATURE 29
#define NXICON_G_ARRAY 30
#define NXICON_G_ARROW 31
#define NXICON_G_ARROWRIGHT 32
#define NXICON_G_BATTERY_000 33
#define NXICON_G_BATTERY_010 34
#define NXICON_G_BATTERY_020 35
#define NXICON_G_BATTERY_030 36
#define NXICON_G_BATTERY_040 37
#define NXICON_G_BATTERY_050 38
#define NXICON_G_BATTERY_060 39
#define NXICON_G_BATTERY_070 40
#define NXICON_G_BATTERY_080 41
#define NXICON_G_BATTERY_090 42
#define NXICON_G_BATTERY_100 43
#define NXICON_G_BELL 44
#define NXICON_G_DELETE 45
#define NXICON_G_CAMERA 46
#define NXICON_G_CAMERA_OFF 47
#define NXICON_G_PADLOCK 48
#define NXICON_G_PADLOCK_OPEN 49
#define NXICON_G_DIRECTORY 50
#define NXICON_G_CYLINDER 51
#define NXICON_G_CIRCLE 52
#define NXICON_G_CLOUD 53
#define NXICON_G_COLUMNS 54
#define NXICON_G_CONE 55
#define NXICON_G_CONSOLE 56
#define NXICON_G_CONSTRAINT 57
#define NXICON_G_GRID 58
#define NXICON_G_CURVE 59
#define NXICON_G_DOCUMENT 60
#define NXICON_G_EASE_AUTO 61
#define NXICON_G_EASE_IN 62
#define NXICON_G_EASE_INOUT 63
#define NXICON_G_EASE_OUT 64
#define NXICON_G_EMPTY 65
#define NXICON_G_EXPAND 66
#define NXICON_G_FALLOFF_CONSTANT 67
#define NXICON_G_FALLOFF_CUSTOM 68
#define NXICON_G_FALLOFF_LINEAR 69
#define NXICON_G_FALLOFF_ROOT 70
#define NXICON_G_FALLOFF_SHARP 71
#define NXICON_G_FALLOFF_SMOOTHER 72
#define NXICON_G_FALLOFF_SPHERE 73
#define NXICON_G_ROWS 74
#define NXICON_G_FILTER 75
#define NXICON_G_FOLDER 76
#define NXICON_G_PHOTO 77
#define NXICON_G_GAMEPAD 78
#define NXICON_G_GEAR 79
#define NXICON_G_RECORD 80
#define NXICON_G_SAVE 81
#define NXICON_G_HANDLE_ALIGNED 82
#define NXICON_G_HANDLE_AUTO 83
#define NXICON_G_HANDLE_AUTOCLAMPED 84
#define NXICON_G_HANDLE_FLAT 85
#define NXICON_G_HANDLE_FREE 86
#define NXICON_G_HANDLE_VECTOR 87
#define NXICON_G_HIDDEN 88
#define NXICON_G_HITBOX 89
#define NXICON_G_HOME 90
#define NXICON_G_EQUALS 91
#define NXICON_G_INSTANCE 92
#define NXICON_G_INTERP_BACK 93
#define NXICON_G_INTERP_BEZIER 94
#define NXICON_G_INTERP_BOUNCE 95
#define NXICON_G_INTERP_CIRC 96
#define NXICON_G_INTERP_CONSTANT 97
#define NXICON_G_INTERP_CUBIC 98
#define NXICON_G_INTERP_ELASTIC 99
#define NXICON_G_INTERP_EXPO 100
#define NXICON_G_INTERP_LINEAR 101
#define NXICON_G_INTERP_QUAD 102
#define NXICON_G_INTERP_QUART 103
#define NXICON_G_INTERP_QUINT 104
#define NXICON_G_INTERP_SINE 105
#define NXICON_G_KEYFRAME 106
#define NXICON_G_LIBRARY 107
#define NXICON_G_LIGHT 108
#define NXICON_G_CLEAR_Z 109
#define NXICON_G_LINE 110
#define NXICON_G_LIST 111
#define NXICON_G_LOCK 112
#define NXICON_G_LOCK_OPEN 113
#define NXICON_G_MAIL 114
#define NXICON_G_PLUS 115
#define NXICON_G_MATERIAL 116
#define NXICON_G_MAXIMIZE 117
#define NXICON_G_MINUS 118
#define NXICON_G_MESH 119
#define NXICON_G_MINIMIZE 120
#define NXICON_G_MIRROR 121
#define NXICON_G_MODIFIER 122
#define NXICON_G_MONITOR 123
#define NXICON_G_FOG 124
#define NXICON_G_NORMALCUSTOM 125
#define NXICON_G_NORMALFACE 126
#define NXICON_G_NORMALVERTEX 127
#define NXICON_G_NOTIFERROR 128
#define NXICON_G_NOTIFOK 129
#define NXICON_G_NUMBER 130
#define NXICON_G_OBJECT 131
#define NXICON_G_PIVOTACTIVE 132
#define NXICON_G_PIVOTCURSOR 133
#define NXICON_G_PIVOTINDIVIDUAL 134
#define NXICON_G_PIVOTMEDIAN 135
#define NXICON_G_PLANE 136
#define NXICON_G_POWER 137
#define NXICON_G_PREFAB 138
#define NXICON_G_CROP 139
#define NXICON_G_CLOCK 140
#define NXICON_G_SCRIPT 141
#define NXICON_G_SEARCH 142
#define NXICON_G_SELEDGE 143
#define NXICON_G_SELFACE 144
#define NXICON_G_SELVERTEX 145
#define NXICON_G_SELECTION 146
#define NXICON_G_SNAP 147
#define NXICON_G_SOUND 148
#define NXICON_G_SPEAKER_0 149
#define NXICON_G_SPEAKER_1 150
#define NXICON_G_SPEAKER_2 151
#define NXICON_G_SPEAKER_3 152
#define NXICON_G_TEXTURE 153
#define NXICON_G_TOMATO 154
#define NXICON_G_TRASH 155
#define NXICON_G_VIDEO 156
#define NXICON_G_VISIBLE 157
#define NXICON_COUNT 158
/* NXICON_NXSCRIPT (26): reserved for a future scripting app (no such program
 * yet); uses script.png, same as the per-filetype html/c icons. */

#define NXICON_THEME_DARK 0
#define NXICON_THEME_LIGHT 1

static const char *const nxicon_paths[2][NXICON_COUNT] = {
    /* dark */
    {
        "/home/Pictures/icon/dark/grid.png",
        "/home/Pictures/icon/dark/console.png",
        "/home/Pictures/icon/dark/directory.png",
        "/home/Pictures/icon/dark/photo.png",
        "/home/Pictures/icon/dark/monitor.png",
        "/home/Pictures/icon/dark/columns.png",
        "/home/Pictures/icon/dark/padlock.png",
        "/home/Pictures/icon/dark/power.png",
        "/home/Pictures/icon/dark/library.png",
        "/home/Pictures/icon/dark/gear.png",
        "/home/Pictures/icon/dark/list.png",
        "/home/Pictures/icon/dark/gamepad.png",
        "/home/Pictures/icon/dark/gamepad.png",
        "/home/Pictures/icon/dark/document.png",
        "/home/Pictures/icon/dark/folder.png",
        "/home/Pictures/icon/dark/script.png",
        "/home/Pictures/icon/dark/list.png",
        "/home/Pictures/icon/dark/modifier.png",
        "/home/Pictures/icon/dark/script.png",
        "/home/Pictures/icon/dark/console.png",
        "/home/Pictures/icon/dark/photo.png",
        "/home/Pictures/icon/dark/video.png",
        "/home/Pictures/icon/dark/cylinder.png",
        "/home/Pictures/icon/dark/animation.png",
        "/home/Pictures/icon/dark/document.png",
        "/home/Pictures/icon/dark/empty.png",
        "/home/Pictures/icon/dark/script.png",
        "/home/Pictures/icon/dark/animation.png",
        "/home/Pictures/icon/dark/archive.png",
        "/home/Pictures/icon/dark/armature.png",
        "/home/Pictures/icon/dark/array.png",
        "/home/Pictures/icon/dark/arrow.png",
        "/home/Pictures/icon/dark/arrowRight.png",
        "/home/Pictures/icon/dark/battery_000.png",
        "/home/Pictures/icon/dark/battery_010.png",
        "/home/Pictures/icon/dark/battery_020.png",
        "/home/Pictures/icon/dark/battery_030.png",
        "/home/Pictures/icon/dark/battery_040.png",
        "/home/Pictures/icon/dark/battery_050.png",
        "/home/Pictures/icon/dark/battery_060.png",
        "/home/Pictures/icon/dark/battery_070.png",
        "/home/Pictures/icon/dark/battery_080.png",
        "/home/Pictures/icon/dark/battery_090.png",
        "/home/Pictures/icon/dark/battery_100.png",
        "/home/Pictures/icon/dark/bell.png",
        "/home/Pictures/icon/dark/delete.png",
        "/home/Pictures/icon/dark/camera.png",
        "/home/Pictures/icon/dark/camera_off.png",
        "/home/Pictures/icon/dark/padlock.png",
        "/home/Pictures/icon/dark/padlock_open.png",
        "/home/Pictures/icon/dark/directory.png",
        "/home/Pictures/icon/dark/cylinder.png",
        "/home/Pictures/icon/dark/circle.png",
        "/home/Pictures/icon/dark/cloud.png",
        "/home/Pictures/icon/dark/columns.png",
        "/home/Pictures/icon/dark/cone.png",
        "/home/Pictures/icon/dark/console.png",
        "/home/Pictures/icon/dark/constraint.png",
        "/home/Pictures/icon/dark/grid.png",
        "/home/Pictures/icon/dark/curve.png",
        "/home/Pictures/icon/dark/document.png",
        "/home/Pictures/icon/dark/ease_auto.png",
        "/home/Pictures/icon/dark/ease_in.png",
        "/home/Pictures/icon/dark/ease_inout.png",
        "/home/Pictures/icon/dark/ease_out.png",
        "/home/Pictures/icon/dark/empty.png",
        "/home/Pictures/icon/dark/expand.png",
        "/home/Pictures/icon/dark/falloff_constant.png",
        "/home/Pictures/icon/dark/falloff_custom.png",
        "/home/Pictures/icon/dark/falloff_linear.png",
        "/home/Pictures/icon/dark/falloff_root.png",
        "/home/Pictures/icon/dark/falloff_sharp.png",
        "/home/Pictures/icon/dark/falloff_smoother.png",
        "/home/Pictures/icon/dark/falloff_sphere.png",
        "/home/Pictures/icon/dark/rows.png",
        "/home/Pictures/icon/dark/filter.png",
        "/home/Pictures/icon/dark/folder.png",
        "/home/Pictures/icon/dark/photo.png",
        "/home/Pictures/icon/dark/gamepad.png",
        "/home/Pictures/icon/dark/gear.png",
        "/home/Pictures/icon/dark/record.png",
        "/home/Pictures/icon/dark/save.png",
        "/home/Pictures/icon/dark/handle_aligned.png",
        "/home/Pictures/icon/dark/handle_auto.png",
        "/home/Pictures/icon/dark/handle_autoclamped.png",
        "/home/Pictures/icon/dark/handle_flat.png",
        "/home/Pictures/icon/dark/handle_free.png",
        "/home/Pictures/icon/dark/handle_vector.png",
        "/home/Pictures/icon/dark/hidden.png",
        "/home/Pictures/icon/dark/hitbox.png",
        "/home/Pictures/icon/dark/home.png",
        "/home/Pictures/icon/dark/equals.png",
        "/home/Pictures/icon/dark/instance.png",
        "/home/Pictures/icon/dark/interp_back.png",
        "/home/Pictures/icon/dark/interp_bezier.png",
        "/home/Pictures/icon/dark/interp_bounce.png",
        "/home/Pictures/icon/dark/interp_circ.png",
        "/home/Pictures/icon/dark/interp_constant.png",
        "/home/Pictures/icon/dark/interp_cubic.png",
        "/home/Pictures/icon/dark/interp_elastic.png",
        "/home/Pictures/icon/dark/interp_expo.png",
        "/home/Pictures/icon/dark/interp_linear.png",
        "/home/Pictures/icon/dark/interp_quad.png",
        "/home/Pictures/icon/dark/interp_quart.png",
        "/home/Pictures/icon/dark/interp_quint.png",
        "/home/Pictures/icon/dark/interp_sine.png",
        "/home/Pictures/icon/dark/keyframe.png",
        "/home/Pictures/icon/dark/library.png",
        "/home/Pictures/icon/dark/light.png",
        "/home/Pictures/icon/dark/clear_z.png",
        "/home/Pictures/icon/dark/line.png",
        "/home/Pictures/icon/dark/list.png",
        "/home/Pictures/icon/dark/lock.png",
        "/home/Pictures/icon/dark/lock_open.png",
        "/home/Pictures/icon/dark/mail.png",
        "/home/Pictures/icon/dark/plus.png",
        "/home/Pictures/icon/dark/material.png",
        "/home/Pictures/icon/dark/maximize.png",
        "/home/Pictures/icon/dark/minus.png",
        "/home/Pictures/icon/dark/mesh.png",
        "/home/Pictures/icon/dark/minimize.png",
        "/home/Pictures/icon/dark/mirror.png",
        "/home/Pictures/icon/dark/modifier.png",
        "/home/Pictures/icon/dark/monitor.png",
        "/home/Pictures/icon/dark/fog.png",
        "/home/Pictures/icon/dark/normalCustom.png",
        "/home/Pictures/icon/dark/normalFace.png",
        "/home/Pictures/icon/dark/normalVertex.png",
        "/home/Pictures/icon/dark/notifError.png",
        "/home/Pictures/icon/dark/notifOk.png",
        "/home/Pictures/icon/dark/number.png",
        "/home/Pictures/icon/dark/object.png",
        "/home/Pictures/icon/dark/pivotActive.png",
        "/home/Pictures/icon/dark/pivotCursor.png",
        "/home/Pictures/icon/dark/pivotIndividual.png",
        "/home/Pictures/icon/dark/pivotMedian.png",
        "/home/Pictures/icon/dark/plane.png",
        "/home/Pictures/icon/dark/power.png",
        "/home/Pictures/icon/dark/prefab.png",
        "/home/Pictures/icon/dark/crop.png",
        "/home/Pictures/icon/dark/clock.png",
        "/home/Pictures/icon/dark/script.png",
        "/home/Pictures/icon/dark/search.png",
        "/home/Pictures/icon/dark/selEdge.png",
        "/home/Pictures/icon/dark/selFace.png",
        "/home/Pictures/icon/dark/selVertex.png",
        "/home/Pictures/icon/dark/selection.png",
        "/home/Pictures/icon/dark/snap.png",
        "/home/Pictures/icon/dark/sound.png",
        "/home/Pictures/icon/dark/speaker_0.png",
        "/home/Pictures/icon/dark/speaker_1.png",
        "/home/Pictures/icon/dark/speaker_2.png",
        "/home/Pictures/icon/dark/speaker_3.png",
        "/home/Pictures/icon/dark/texture.png",
        "/home/Pictures/icon/dark/tomato.png",
        "/home/Pictures/icon/dark/trash.png",
        "/home/Pictures/icon/dark/video.png",
        "/home/Pictures/icon/dark/visible.png",
    },
    /* light */
    {
        "/home/Pictures/icon/light/grid.png",
        "/home/Pictures/icon/light/console.png",
        "/home/Pictures/icon/light/directory.png",
        "/home/Pictures/icon/light/photo.png",
        "/home/Pictures/icon/light/monitor.png",
        "/home/Pictures/icon/light/columns.png",
        "/home/Pictures/icon/light/padlock.png",
        "/home/Pictures/icon/light/power.png",
        "/home/Pictures/icon/light/library.png",
        "/home/Pictures/icon/light/gear.png",
        "/home/Pictures/icon/light/list.png",
        "/home/Pictures/icon/light/gamepad.png",
        "/home/Pictures/icon/light/gamepad.png",
        "/home/Pictures/icon/light/document.png",
        "/home/Pictures/icon/light/folder.png",
        "/home/Pictures/icon/light/script.png",
        "/home/Pictures/icon/light/list.png",
        "/home/Pictures/icon/light/modifier.png",
        "/home/Pictures/icon/light/script.png",
        "/home/Pictures/icon/light/console.png",
        "/home/Pictures/icon/light/photo.png",
        "/home/Pictures/icon/light/video.png",
        "/home/Pictures/icon/light/cylinder.png",
        "/home/Pictures/icon/light/animation.png",
        "/home/Pictures/icon/light/document.png",
        "/home/Pictures/icon/light/empty.png",
        "/home/Pictures/icon/light/script.png",
        "/home/Pictures/icon/light/animation.png",
        "/home/Pictures/icon/light/archive.png",
        "/home/Pictures/icon/light/armature.png",
        "/home/Pictures/icon/light/array.png",
        "/home/Pictures/icon/light/arrow.png",
        "/home/Pictures/icon/light/arrowRight.png",
        "/home/Pictures/icon/light/battery_000.png",
        "/home/Pictures/icon/light/battery_010.png",
        "/home/Pictures/icon/light/battery_020.png",
        "/home/Pictures/icon/light/battery_030.png",
        "/home/Pictures/icon/light/battery_040.png",
        "/home/Pictures/icon/light/battery_050.png",
        "/home/Pictures/icon/light/battery_060.png",
        "/home/Pictures/icon/light/battery_070.png",
        "/home/Pictures/icon/light/battery_080.png",
        "/home/Pictures/icon/light/battery_090.png",
        "/home/Pictures/icon/light/battery_100.png",
        "/home/Pictures/icon/light/bell.png",
        "/home/Pictures/icon/light/delete.png",
        "/home/Pictures/icon/light/camera.png",
        "/home/Pictures/icon/light/camera_off.png",
        "/home/Pictures/icon/light/padlock.png",
        "/home/Pictures/icon/light/padlock_open.png",
        "/home/Pictures/icon/light/directory.png",
        "/home/Pictures/icon/light/cylinder.png",
        "/home/Pictures/icon/light/circle.png",
        "/home/Pictures/icon/light/cloud.png",
        "/home/Pictures/icon/light/columns.png",
        "/home/Pictures/icon/light/cone.png",
        "/home/Pictures/icon/light/console.png",
        "/home/Pictures/icon/light/constraint.png",
        "/home/Pictures/icon/light/grid.png",
        "/home/Pictures/icon/light/curve.png",
        "/home/Pictures/icon/light/document.png",
        "/home/Pictures/icon/light/ease_auto.png",
        "/home/Pictures/icon/light/ease_in.png",
        "/home/Pictures/icon/light/ease_inout.png",
        "/home/Pictures/icon/light/ease_out.png",
        "/home/Pictures/icon/light/empty.png",
        "/home/Pictures/icon/light/expand.png",
        "/home/Pictures/icon/light/falloff_constant.png",
        "/home/Pictures/icon/light/falloff_custom.png",
        "/home/Pictures/icon/light/falloff_linear.png",
        "/home/Pictures/icon/light/falloff_root.png",
        "/home/Pictures/icon/light/falloff_sharp.png",
        "/home/Pictures/icon/light/falloff_smoother.png",
        "/home/Pictures/icon/light/falloff_sphere.png",
        "/home/Pictures/icon/light/rows.png",
        "/home/Pictures/icon/light/filter.png",
        "/home/Pictures/icon/light/folder.png",
        "/home/Pictures/icon/light/photo.png",
        "/home/Pictures/icon/light/gamepad.png",
        "/home/Pictures/icon/light/gear.png",
        "/home/Pictures/icon/light/record.png",
        "/home/Pictures/icon/light/save.png",
        "/home/Pictures/icon/light/handle_aligned.png",
        "/home/Pictures/icon/light/handle_auto.png",
        "/home/Pictures/icon/light/handle_autoclamped.png",
        "/home/Pictures/icon/light/handle_flat.png",
        "/home/Pictures/icon/light/handle_free.png",
        "/home/Pictures/icon/light/handle_vector.png",
        "/home/Pictures/icon/light/hidden.png",
        "/home/Pictures/icon/light/hitbox.png",
        "/home/Pictures/icon/light/home.png",
        "/home/Pictures/icon/light/equals.png",
        "/home/Pictures/icon/light/instance.png",
        "/home/Pictures/icon/light/interp_back.png",
        "/home/Pictures/icon/light/interp_bezier.png",
        "/home/Pictures/icon/light/interp_bounce.png",
        "/home/Pictures/icon/light/interp_circ.png",
        "/home/Pictures/icon/light/interp_constant.png",
        "/home/Pictures/icon/light/interp_cubic.png",
        "/home/Pictures/icon/light/interp_elastic.png",
        "/home/Pictures/icon/light/interp_expo.png",
        "/home/Pictures/icon/light/interp_linear.png",
        "/home/Pictures/icon/light/interp_quad.png",
        "/home/Pictures/icon/light/interp_quart.png",
        "/home/Pictures/icon/light/interp_quint.png",
        "/home/Pictures/icon/light/interp_sine.png",
        "/home/Pictures/icon/light/keyframe.png",
        "/home/Pictures/icon/light/library.png",
        "/home/Pictures/icon/light/light.png",
        "/home/Pictures/icon/light/clear_z.png",
        "/home/Pictures/icon/light/line.png",
        "/home/Pictures/icon/light/list.png",
        "/home/Pictures/icon/light/lock.png",
        "/home/Pictures/icon/light/lock_open.png",
        "/home/Pictures/icon/light/mail.png",
        "/home/Pictures/icon/light/plus.png",
        "/home/Pictures/icon/light/material.png",
        "/home/Pictures/icon/light/maximize.png",
        "/home/Pictures/icon/light/minus.png",
        "/home/Pictures/icon/light/mesh.png",
        "/home/Pictures/icon/light/minimize.png",
        "/home/Pictures/icon/light/mirror.png",
        "/home/Pictures/icon/light/modifier.png",
        "/home/Pictures/icon/light/monitor.png",
        "/home/Pictures/icon/light/fog.png",
        "/home/Pictures/icon/light/normalCustom.png",
        "/home/Pictures/icon/light/normalFace.png",
        "/home/Pictures/icon/light/normalVertex.png",
        "/home/Pictures/icon/light/notifError.png",
        "/home/Pictures/icon/light/notifOk.png",
        "/home/Pictures/icon/light/number.png",
        "/home/Pictures/icon/light/object.png",
        "/home/Pictures/icon/light/pivotActive.png",
        "/home/Pictures/icon/light/pivotCursor.png",
        "/home/Pictures/icon/light/pivotIndividual.png",
        "/home/Pictures/icon/light/pivotMedian.png",
        "/home/Pictures/icon/light/plane.png",
        "/home/Pictures/icon/light/power.png",
        "/home/Pictures/icon/light/prefab.png",
        "/home/Pictures/icon/light/crop.png",
        "/home/Pictures/icon/light/clock.png",
        "/home/Pictures/icon/light/script.png",
        "/home/Pictures/icon/light/search.png",
        "/home/Pictures/icon/light/selEdge.png",
        "/home/Pictures/icon/light/selFace.png",
        "/home/Pictures/icon/light/selVertex.png",
        "/home/Pictures/icon/light/selection.png",
        "/home/Pictures/icon/light/snap.png",
        "/home/Pictures/icon/light/sound.png",
        "/home/Pictures/icon/light/speaker_0.png",
        "/home/Pictures/icon/light/speaker_1.png",
        "/home/Pictures/icon/light/speaker_2.png",
        "/home/Pictures/icon/light/speaker_3.png",
        "/home/Pictures/icon/light/texture.png",
        "/home/Pictures/icon/light/tomato.png",
        "/home/Pictures/icon/light/trash.png",
        "/home/Pictures/icon/light/video.png",
        "/home/Pictures/icon/light/visible.png",
    },
};

/* One cache slot per (theme, app id).  `tried` distinguishes "never asked"
 * from "asked and it's missing/bad" so a permanently-absent icon costs one
 * failed load for the whole process lifetime, not one per redraw. */
typedef struct {
  os1_image_t *img;
  int tried;
} nxicon_slot_t;

static nxicon_slot_t nxicon_cache[2][NXICON_COUNT];
static int nxicon_tile_size; /* size the cache was last built for */

/* nxicon_ci_prefix - case-insensitive prefix test: 1 if 's' starts with
 * 'prefix' ignoring ASCII case, 0 otherwise. This environment's os1.h only
 * declares strcasecmp() (whole-string), not a bounded/prefix strncasecmp(),
 * so this is a tiny self-contained helper in the same header-only style as
 * the rest of nxicon.h — no new libc dependency. */
static inline int nxicon_ci_prefix(const char *s, const char *prefix) {
  while (*prefix) {
    if (!*s)
      return 0;
    char a = *s, b = *prefix;
    if (a >= 'A' && a <= 'Z')
      a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z')
      b = (char)(b + 32);
    if (a != b)
      return 0;
    s++;
    prefix++;
  }
  return 1;
}

/*
 * nxicon_classify - map a window title to an NXICON_* id, for a RUNNING
 * window (nxui's dock). Use nxicon_classify_path() below for an app-table
 * entry's executable path (nxlauncher's grid) instead — a window title can
 * carry extra text a bare exe path never would (e.g. nxshell titles its
 * window "NXShell PID 6": different case than the binary name "nxshell",
 * plus a trailing " PID <n>"), so this is deliberately looser:
 *
 *   - strips any directory prefix first (some windows carry the full exe
 *     path as their title instead of a bare name);
 *   - matches case-INSENSITIVELY, since an app is free to title-case its
 *     own window ("NXShell") while its binary on disk is lowercase
 *     ("nxshell");
 *   - matches by PREFIX, not exact, so a trailing suffix ("NXShell PID 6",
 *     "nxfilem — folder view") doesn't defeat the match.
 *
 * Checked longest/most-specific prefix first where one binary name could
 * otherwise shadow another (none currently collide, but nxmemstat/nxsettings
 * etc. are kept in a stable, deliberate order rather than alphabetical).
 *
 * Anything unrecognised is NXICON_UNKNOWN — a valid icon choice, not a
 * failure.
 */
static inline int nxicon_classify(const char *title) {
  if (!title)
    return NXICON_UNKNOWN;
  const char *base = strrchr(title, '/');
  base = base ? base + 1 : title;
  if (nxicon_ci_prefix(base, "nxlauncher"))
    return NXICON_LAUNCHER;
  if (nxicon_ci_prefix(base, "nxshell"))
    return NXICON_SHELL;
  if (nxicon_ci_prefix(base, "nxfilem"))
    return NXICON_FILEM;
  if (nxicon_ci_prefix(base, "nximage"))
    return NXICON_IMAGE;
  if (nxicon_ci_prefix(base, "nxinfo"))
    return NXICON_INFO;
  if (nxicon_ci_prefix(base, "nxmemstat"))
    return NXICON_MEMSTAT;
  if (nxicon_ci_prefix(base, "nxperm"))
    return NXICON_PERM;
  if (nxicon_ci_prefix(base, "nxpower"))
    return NXICON_POWER;
  if (nxicon_ci_prefix(base, "nxreg"))
    return NXICON_REG;
  if (nxicon_ci_prefix(base, "nxsettings"))
    return NXICON_SETTINGS;
  if (nxicon_ci_prefix(base, "nxtop"))
    return NXICON_TOP;
  if (nxicon_ci_prefix(base, "doom"))
    return NXICON_DOOM;
  if (nxicon_ci_prefix(base, "raptor"))
    return NXICON_RAPTOR;
  if (nxicon_ci_prefix(base, "kilo"))
    return NXICON_TEXTEDIT;
  if (nxicon_ci_prefix(base, "nxscript"))
    return NXICON_NXSCRIPT;
  if (nxicon_ci_prefix(base, "nxnotify"))
    return NXICON_G_BELL;
  if (nxicon_ci_prefix(base, "nxempire"))
    return NXICON_G_GAMEPAD;
  if (nxicon_ci_prefix(base, "demo3d"))
    return NXICON_G_MESH;
  if (nxicon_ci_prefix(base, "nxres"))
    return NXICON_G_EXPAND;
  if (nxicon_ci_prefix(base, "nxkeymap"))
    return NXICON_G_NUMBER;
  if (nxicon_ci_prefix(base, "nxproc"))
    return NXICON_G_ROWS;
  if (nxicon_ci_prefix(base, "nxwins"))
    return NXICON_G_MAXIMIZE;
  if (nxicon_ci_prefix(base, "nxenvinit"))
    return NXICON_G_MODIFIER;
  if (nxicon_ci_prefix(base, "nxtimerdiag"))
    return NXICON_G_CLOCK;
  if (nxicon_ci_prefix(base, "nxpax"))
    return NXICON_G_ARCHIVE;
  if (nxicon_ci_prefix(base, "nxtestrunner"))
    return NXICON_G_NOTIFOK;
  if (nxicon_ci_prefix(base, "nxroleprobe"))
    return NXICON_G_SEARCH;
  if (nxicon_ci_prefix(base, "nxfont"))
    return NXICON_G_CURVE;
  if (nxicon_ci_prefix(base, "nxtest_userkit_"))
    return NXICON_G_NOTIFOK;
  /* "lua" deliberately NOT prefix-matched here: it would also catch "luatest"
   * (a test suite, no icon).  lua is mapped by exact name in
   * nxicon_classify_path() only. */
  return NXICON_UNKNOWN;
}

/*
 * nxicon_classify_path - map an app-table executable path (e.g.
 * "/sys/bin/nxshell") to an NXICON_* id by an EXACT basename match.
 * Anything unrecognised is NXICON_UNKNOWN (the generic app icon).
 */
static inline int nxicon_classify_path(const char *path) {
  if (!path)
    return NXICON_UNKNOWN;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (strcmp(base, "nxlauncher") == 0)
    return NXICON_LAUNCHER;
  if (strcmp(base, "nxshell") == 0)
    return NXICON_SHELL;
  if (strcmp(base, "nxfilem") == 0)
    return NXICON_FILEM;
  if (strcmp(base, "nximage") == 0)
    return NXICON_IMAGE;
  if (strcmp(base, "nxinfo") == 0)
    return NXICON_INFO;
  if (strcmp(base, "nxmemstat") == 0)
    return NXICON_MEMSTAT;
  if (strcmp(base, "nxperm") == 0)
    return NXICON_PERM;
  if (strcmp(base, "nxpower") == 0)
    return NXICON_POWER;
  if (strcmp(base, "nxreg") == 0)
    return NXICON_REG;
  if (strcmp(base, "nxsettings") == 0)
    return NXICON_SETTINGS;
  if (strcmp(base, "nxtop") == 0)
    return NXICON_TOP;
  if (strcmp(base, "doom") == 0)
    return NXICON_DOOM;
  if (strcmp(base, "raptor") == 0)
    return NXICON_RAPTOR;
  if (strcmp(base, "kilo") == 0)
    return NXICON_TEXTEDIT;
  if (strcmp(base, "nxscript") == 0)
    return NXICON_NXSCRIPT;
  if (strcmp(base, "nxnotify") == 0)
    return NXICON_G_BELL;
  if (strcmp(base, "nxempire") == 0)
    return NXICON_G_GAMEPAD;
  if (strcmp(base, "demo3d") == 0)
    return NXICON_G_MESH;
  if (strcmp(base, "lua") == 0)
    return NXICON_NXSCRIPT;
  if (strcmp(base, "nxres") == 0)
    return NXICON_G_EXPAND;
  if (strcmp(base, "nxkeymap") == 0)
    return NXICON_G_NUMBER;
  if (strcmp(base, "nxproc") == 0)
    return NXICON_G_ROWS;
  if (strcmp(base, "nxwins") == 0)
    return NXICON_G_MAXIMIZE;
  if (strcmp(base, "nxenvinit") == 0)
    return NXICON_G_MODIFIER;
  if (strcmp(base, "nxtimerdiag") == 0)
    return NXICON_G_CLOCK;
  if (strcmp(base, "nxpax") == 0)
    return NXICON_G_ARCHIVE;
  if (strcmp(base, "nxtestrunner") == 0)
    return NXICON_G_NOTIFOK;
  if (strcmp(base, "nxroleprobe") == 0)
    return NXICON_G_SEARCH;
  if (strcmp(base, "nxfont") == 0)
    return NXICON_G_CURVE;
  if (nxicon_ci_prefix(base, "nxtest_userkit_"))
    return NXICON_G_NOTIFOK;
  return NXICON_UNKNOWN;
}

/*
 * nxicon_classify_file - map a filesystem entry (name + is_dir) to an
 * NXICON_* id for nxfilem's content list.  Directories always get
 * NXICON_FOLDER regardless of name.  Files are classified by extension
 * (case-insensitive, via os1_image_ext_eq from image.h — same helper
 * os1_image_path_has_known_ext already uses), falling back to
 * NXICON_FILE_GENERIC for anything unrecognised.  Kept in the same
 * header-only, static-inline style as nxicon_classify()/nxicon_classify_path()
 * above; deliberately independent of nxassoc.h (icon choice and launch
 * association are separate concerns that happen to share extension lists).
 */
static inline int nxicon_classify_file(const char *name, int is_dir) {
  if (is_dir)
    return NXICON_FOLDER;
  if (!name)
    return NXICON_FILE_GENERIC;
  const char *ext = strrchr(name, '.');
  if (!ext || ext == name)
    return NXICON_FILE_GENERIC;

  static const char *const c_exts[] = {".c",   ".h", ".cpp", ".cc",
                                       ".hpp", ".s", NULL};
  static const char *const text_exts[] = {".txt", ".md",  ".log",
                                          ".rst", ".csv", NULL};
  static const char *const cfg_exts[] = {".cfg",  ".json", ".ini", ".toml",
                                         ".yaml", ".yml",  NULL};
  static const char *const html_exts[] = {".html", ".htm", ".css", NULL};
  static const char *const script_exts[] = {".lua", ".sh", ".py", NULL};
  static const char *const video_exts[] = {".mp4", ".mkv",  ".avi", ".webm",
                                           ".mpg", ".mpeg", NULL};
  static const char *const bin_exts[] = {".bin", ".dat", ".o", ".elf", NULL};

  if (os1_image_path_has_known_ext(name))
    return NXICON_FILE_IMAGE;
  for (int i = 0; c_exts[i]; i++)
    if (os1_image_ext_eq(ext, c_exts[i]))
      return NXICON_FILE_C;
  for (int i = 0; script_exts[i]; i++)
    if (os1_image_ext_eq(ext, script_exts[i]))
      return NXICON_FILE_SCRIPT;
  for (int i = 0; html_exts[i]; i++)
    if (os1_image_ext_eq(ext, html_exts[i]))
      return NXICON_FILE_HTML;
  for (int i = 0; cfg_exts[i]; i++)
    if (os1_image_ext_eq(ext, cfg_exts[i]))
      return NXICON_FILE_CONFIG;
  for (int i = 0; text_exts[i]; i++)
    if (os1_image_ext_eq(ext, text_exts[i]))
      return NXICON_FILE_TEXT;
  for (int i = 0; video_exts[i]; i++)
    if (os1_image_ext_eq(ext, video_exts[i]))
      return NXICON_FILE_VIDEO;
  if (os1_image_ext_eq(ext, ".elf"))
    return NXICON_FILE_EXECUTABLE;
  for (int i = 0; bin_exts[i]; i++)
    if (os1_image_ext_eq(ext, bin_exts[i]))
      return NXICON_FILE_BINARY;
  return NXICON_FILE_GENERIC;
}

/*
 * nxicon_find - icon id for an asset file name without directory/extension
 * (e.g. "gear", "battery_050", "padlock"), searching the HydroGlass catalog
 * (ids from 27 up).  Returns NXICON_UNKNOWN if not found.  Linear scan: resolve
 * once and keep the id, don't call per frame.  Manual loop on purpose (minimal
 * libc here, see nxicon_ci_prefix).
 */
static inline int nxicon_find(const char *name) {
  if (!name)
    return NXICON_UNKNOWN;
  for (int i = 27; i < NXICON_COUNT; i++) {
    const char *p = strrchr(nxicon_paths[NXICON_THEME_DARK][i], '/');
    p = p ? p + 1 : nxicon_paths[NXICON_THEME_DARK][i];
    const char *q = name;
    while (*q && *p == *q) {
      p++;
      q++;
    }
    if (!*q && strcmp(p, ".png") == 0)
      return i;
  }
  return NXICON_UNKNOWN;
}

/* nxicon_reset_cache - drop every cached scaled icon and retry flag (both
 * themes).  Only needed if the tile size itself changes (a future DPI/zoom
 * feature); a plain theme flip does NOT need this, since both themes are
 * cached side by side already. */
static inline void nxicon_reset_cache(void) {
  for (int t = 0; t < 2; t++)
    for (int i = 0; i < NXICON_COUNT; i++) {
      if (nxicon_cache[t][i].img)
        os1_image_free(nxicon_cache[t][i].img);
      nxicon_cache[t][i].img = NULL;
      nxicon_cache[t][i].tried = 0;
    }
  nxicon_tile_size = 0;
}

/*
 * nxicon_get - cached tile_size x tile_size ARGB icon for (app_id, light), or
 * NULL if icons are disabled or this asset is missing/undecodable.  Callers
 * always treat NULL as "fall back to the classic flat-color rounded tile" —
 * see the header comment above.
 */
static inline os1_image_t *nxicon_get(int app_id, int light, int tile_size) {
#if !NXICON_ENABLE
  (void)app_id;
  (void)light;
  (void)tile_size;
  return NULL;
#else
  if (app_id < 0 || app_id >= NXICON_COUNT || tile_size <= 0)
    return NULL;

  if (nxicon_tile_size != tile_size)
    nxicon_reset_cache(); /* size changed since the cache was built: rebuild
                           * lazily, one icon at a time, as each is next
                           * requested — not an eager reload of all of them. */
  nxicon_tile_size = tile_size;

  int theme = light ? NXICON_THEME_LIGHT : NXICON_THEME_DARK;
  nxicon_slot_t *slot = &nxicon_cache[theme][app_id];
  if (slot->img)
    return slot->img;
  if (slot->tried)
    return NULL;
  slot->tried = 1;

  os1_image_t *src = os1_image_load(nxicon_paths[theme][app_id]);
  if (!src)
    return NULL;

  /* Trusted-asset fast path: known-square OS icon, so resample straight to
   * the tile size instead of nximage.c's aspect-fit dance for arbitrary user
   * photos.  Skip the resample entirely if the asset already matches. */
  os1_image_t *scaled = (src->w == tile_size && src->h == tile_size)
                            ? src
                            : os1_image_resample(src, tile_size, tile_size);
  if (scaled != src)
    os1_image_free(src);
  if (!scaled)
    return NULL;

  slot->img = scaled;
  return slot->img;
#endif
}

#endif /* _USER_NXICON_H */