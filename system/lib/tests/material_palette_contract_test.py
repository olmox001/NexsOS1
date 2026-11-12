#!/usr/bin/env python3
"""Real full-theme parity tests for kernel bootstrap and userland compositor.

Both must render the same OS1_ theme/background enums while remaining
separately buildable. The SDK tokens are the canonical Material colors.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOKENS = (ROOT / "system/include/contract/os1_material_colors.h").read_text()
KERNEL = (ROOT / "kernel/graphics/compositor_style.c").read_text()
USER = (ROOT / "system/lib/gfx/theme.c").read_text()
MIRROR = (ROOT / "user/sys/bin/nxres.h").read_text()

BACKGROUNDS = (
    "BLACK", "RED", "GREEN", "YELLOW", "BLUE", "MAGENTA", "CYAN",
    "WHITE", "GRAY", "BRIGHT_RED", "BRIGHT_GREEN", "BRIGHT_YELLOW",
    "BRIGHT_BLUE", "BRIGHT_MAGENTA", "BRIGHT_CYAN", "BRIGHT_WHITE",
)
THEME_FIELDS = (
    "win_bg", "title_active", "title_inactive",
    "title_text_active", "title_text_inactive", "close_btn",
    "border", "accent",
)


def token(name):
    pattern = r"^#define\s+" + re.escape(name) + r"\s+(0x[0-9A-Fa-f]+)U\s*$"
    match = re.search(pattern, TOKENS, re.M)
    if match is None:
        raise ValueError("missing Material token: " + name)
    return int(match.group(1), 16)


def rgba(expr):
    if expr.startswith("OS1_MATERIAL_"):
        return token(expr)
    return int(expr.rstrip("uU"), 16)


def initializer(source, preset, field):
    pattern = (r"\[" + re.escape(preset) + r"\]\s*=\s*\{[^}]*?\."
               + re.escape(field) +
               r"\s*=\s*(0x[0-9A-Fa-f]+[uU]?|OS1_MATERIAL_[A-Z_]+)")
    match = re.search(pattern, source, re.S)
    if match is None:
        raise ValueError("missing theme field: " + preset + "." + field)
    return rgba(match.group(1))


class MaterialPaletteParity(unittest.TestCase):
    def test_all_light_and_dark_fields_match(self):
        for theme in ("LIGHT", "DARK"):
            for field in THEME_FIELDS:
                with self.subTest(theme=theme, field=field):
                    self.assertEqual(
                        initializer(KERNEL, "THEME_" + theme, field),
                        initializer(USER, "OS1_THEME_" + theme, field))

    def test_all_sixteen_desktop_backgrounds_and_gradients_match(self):
        for background in BACKGROUNDS:
            for field in ("bg_color", "bg_top", "bg_bottom"):
                with self.subTest(background=background, field=field):
                    self.assertEqual(
                        initializer(KERNEL, "BG_" + background, field),
                        initializer(USER, "OS1_BG_" + background, field))

    def test_registry_color_mirror_matches_all_sixteen(self):
        entries = re.findall(
            r"(0x[0-9A-Fa-f]+[uU]?|OS1_MATERIAL_[A-Z_]+)\s*,\s*"
            r"/\*\s*(BG_[A-Z_]+)\s*\*/", MIRROR)
        found = dict((name, rgba(value)) for value, name in entries)
        self.assertEqual(len(found), len(BACKGROUNDS))
        for background in BACKGROUNDS:
            name = "BG_" + background
            with self.subTest(background=background):
                self.assertEqual(
                    found[name], initializer(KERNEL, name, "bg_color"))

    def test_material_neutrals_are_warm_and_grey_is_light(self):
        self.assertLess(token("OS1_MATERIAL_BG_GRAY") & 255, 255)
        self.assertGreater(token("OS1_MATERIAL_BG_GRAY") & 255, 170)
        self.assertNotEqual(token("OS1_MATERIAL_BG_WHITE"), 0xFFFFFFFF)
        self.assertNotEqual(token("OS1_MATERIAL_BG_BRIGHT_WHITE"),
                            0xFFFFFFFF)


if __name__ == "__main__":
    unittest.main()
