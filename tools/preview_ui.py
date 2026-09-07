#!/usr/bin/env python3
"""Render what the status screen will look like, without a board.

The LCD board's layout cannot be checked any other way until someone is
standing in front of the panel, and "it compiles" says nothing about whether
the IP fits or the rows collide. This draws the real glyphs from the real font
tables at the real layout offsets, so the things that are actually checkable
without hardware — does it fit, does it overlap, is it legible — are checkable.

It is a preview, not a test: it deliberately reads main/font*.c and main/ui.c
rather than hardcoding their values, so it cannot silently disagree with the
firmware, but the sequence of draw calls mirrors ui.c's draw() by hand.

  python3 tools/preview_ui.py            # ASCII art for both boards
  python3 tools/preview_ui.py --png out  # also write out-{board}[-{theme}].png
  python3 tools/preview_ui.py --contrast # the contrast table, from theme.h

Themes are read out of main/theme.h and main/palette.h for the same reason the
fonts and offsets are read out of their sources: a preview that disagrees with
the firmware is worse than no preview. Colours are QUANTISED to RGB565 before
being drawn, so what this renders is what the panel can show rather than what
the theme's source scheme asked for.
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def parse_font(path, name):
    """Pull a canvas_font table and its metrics out of the C source."""
    src = (ROOT / path).read_text()
    body = src.split("[] = {", 1)[1].split("\n};", 1)[0]
    cols = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})", body)]
    meta = {}
    for key in ("glyph_w", "glyph_h", "advance", "line_h", "first", "last"):
        meta[key] = int(re.search(rf"\.{key} = (\d+)", src).group(1))
    meta["fold"] = ".fold_lowercase = true" in src
    n = meta["last"] - meta["first"] + 1
    expect = n * meta["glyph_w"]
    if len(cols) != expect:
        sys.exit(f"{name}: {len(cols)} column entries, expected {expect}")
    meta["cols"] = cols
    meta["name"] = name
    return meta


def parse_palette_triples():
    """Named comma-triples from main/palette.h, e.g. SOL_BASE03 -> (0, 43, 54)."""
    src = (ROOT / "main/palette.h").read_text()
    out = {}
    for name, r, g, b in re.findall(
        r"#define\s+(SOL_\w+)\s+0x([0-9A-Fa-f]{2}),\s*"
        r"0x([0-9A-Fa-f]{2}),\s*0x([0-9A-Fa-f]{2})",
        src,
    ):
        out[name] = (int(r, 16), int(g, 16), int(b, 16))
    return out


def q565(rgb):
    """8-bit RGB through RGB565 and back, the way the panel does it.

    Mirrors palette.h's RGB565() packing and the expansion in
    test/host/test_theme.c: the high bits are replicated into the low ones, so
    0x1F becomes 0xFF and not 0xF8. Drawing the unquantised colour would show a
    picture the hardware cannot produce.
    """
    r, g, b = rgb
    r5, g6, b5 = r >> 3, g >> 2, b >> 3
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


ROLE_INDEX = {"NONE": 0, "PRIMARY": 1, "SECONDARY": 2, "ACCENT": 3}


def parse_themes():
    """The theme table from main/theme.h, as {name: [rgb per ink role]}."""
    src = (ROOT / "main/theme.h").read_text()
    triples = parse_palette_triples()
    themes = {}
    # Entries look like: [DISPLAY_THEME_X] = {"name", { [CANVAS_INK_Y] = ... }},
    for name, body in re.findall(
        r'\[DISPLAY_THEME_\w+\]\s*=\s*\{\s*"([a-z0-9-]+)"\s*,(.*?)\}\s*\}',
        src,
        re.S,
    ):
        roles = [None] * 4
        for role, macro, args in re.findall(
            r"\[CANVAS_INK_(\w+)\]\s*=\s*(RGB565_OF|RGB565)\(([^)]*)\)", body
        ):
            if macro == "RGB565_OF":
                key = args.strip()
                if key not in triples:
                    sys.exit(f"{name}: palette.h has no {key}")
                rgb = triples[key]
            else:
                rgb = tuple(int(v.strip(), 16) for v in args.split(","))
            roles[ROLE_INDEX[role]] = q565(rgb)
        if any(r is None for r in roles):
            sys.exit(f"{name}: theme is missing an ink role")
        themes[name] = roles
    if not themes:
        sys.exit("no themes parsed from main/theme.h")
    return themes


def contrast(fg, bg):
    """WCAG contrast ratio. Both colours are already RGB565-quantised."""

    def lum(c):
        acc = 0.0
        for v, w in zip(c, (0.2126, 0.7152, 0.0722)):
            v /= 255.0
            acc += w * (v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4)
        return acc

    a, b = lum(fg), lum(bg)
    return (max(a, b) + 0.05) / (min(a, b) + 0.05)


def parse_ui_offsets(st7789):
    """Read one board's layout constants out of main/ui.c.

    Reads rather than hardcodes, so this cannot drift from the firmware. There
    is more than one `#if BOARD_DISPLAY_ST7789` in that file, so the block is
    found by the constant it must contain rather than by position.
    """
    src = (ROOT / "main/ui.c").read_text()
    blocks = [
        b for b in src.split("#if BOARD_DISPLAY_ST7789")[1:] if "UI_Y_ADDRESS" in b
    ]
    if len(blocks) != 1:
        sys.exit(f"expected exactly one layout block, found {len(blocks)}")
    lcd, oled = blocks[0].split("#else", 1)
    chosen = lcd if st7789 else oled.split("#endif", 1)[0]

    out = {}
    for key in ("HEADER", "ADDRESS", "NETWORK", "TARGET", "DEBUG"):
        m = re.search(rf"#define UI_Y_{key} (\d+)", chosen)
        out[key] = int(m.group(1)) if m else 0
    m = re.search(r"#define UI_Y_MDNS (\d+)", chosen)
    out["MDNS"] = int(m.group(1)) if m else None

    # The safe-area inset lives in its own block near the top of the file.
    safe = src.split("#if BOARD_DISPLAY_ST7789", 1)[1].split("#endif", 1)[0]
    lcd_safe, oled_safe = safe.split("#else", 1)
    chosen_safe = lcd_safe if st7789 else oled_safe
    out["X_COL"] = int(re.search(r"#define UI_X_COL (\d+)", chosen_safe).group(1))
    return out


class Canvas:
    """The 1bpp surface, matching canvas.h's semantics (including clipping)."""

    def __init__(self, w, h, font):
        self.w, self.h, self.font = w, h, font
        # Stores the ink ROLE per pixel (0..3), matching canvas.h's two planes.
        self.px = [[0] * w for _ in range(h)]
        self.ink = 1

    def set_ink(self, ink):
        self.ink = ink

    def pixel(self, x, y, on=True):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = self.ink if on else 0

    def text(self, x, y, s, sx=1, sy=1):
        f = self.font
        for ch in s:
            if x + f["glyph_w"] * sx > self.w:
                return  # clip, never wrap
            c = ord(ch.upper() if f["fold"] and ch.islower() else ch)
            if not (f["first"] <= c <= f["last"]):
                c = ord("?")
            base = (c - f["first"]) * f["glyph_w"]
            for gx in range(f["glyph_w"]):
                col = f["cols"][base + gx]
                for gy in range(f["glyph_h"]):
                    if col & (1 << gy):
                        for ry in range(sy):
                            for rx in range(sx):
                                self.pixel(x + gx * sx + rx, y + gy * sy + ry)
            x += f["advance"] * sx

    def bar(self, col, row, width, filled, sx=1, sy=1):
        f = self.font
        x0, y0 = col * f["advance"] * sx, row * f["line_h"] * sy
        bw, bh = f["glyph_w"] * sx, f["glyph_h"] * sy
        for i in range(width):
            on = i < filled
            for gx in range(bw):
                for gy in range(bh):
                    edge = gy in (0, bh - 1) or gx in (0, bw - 1)
                    self.pixel(x0 + i * f["advance"] * sx + gx, y0 + gy, on or edge)


def draw_status(c, y, tall, mdns, ip, ssid, rssi_bars, events, usb, bound, debug):
    """Mirrors main/ui.c draw(). Keep in step with it by hand."""
    f = c.font
    cols = c.w // f["advance"]
    cell_h = f["line_h"]
    x0 = y["X_COL"] * f["advance"]
    c.set_ink(2)
    c.text(x0, y["HEADER"], "AIRKVM")
    c.text((cols - 7) * f["advance"], y["HEADER"], "BOUND" if bound else "IDLE")
    c.set_ink(1)
    c.text(x0, y["ADDRESS"], ip or "NO WIFI", *tall)
    c.set_ink(2)
    if y["MDNS"] is not None:
        c.text(x0, y["MDNS"], f"{mdns}.local")
    c.text(x0, y["NETWORK"], ssid)
    c.set_ink(3)
    c.bar(cols - 4, y["NETWORK"] // cell_h, 4, rssi_bars)
    c.set_ink(2)
    c.text(x0, y["TARGET"], f"{'USB OK' if usb else 'NO HID'}  {events}")
    if debug:
        c.text(x0, y["DEBUG"], debug)


def ascii_art(c):
    return "\n".join("".join("#" if v else "." for v in row) for row in c.px)


def render_png(c, path, colours, mono):
    """One PNG. `colours` is a role->rgb list; `mono` renders every role lit."""
    from PIL import Image

    scale = 2 if c.w > 200 else 6
    bg = (0, 0, 0) if mono else colours[0]
    img = Image.new("RGB", (c.w * scale, c.h * scale), bg)
    px = img.load()
    for yy in range(c.h):
        for xx in range(c.w):
            role = c.px[yy][xx]
            if not role:
                continue
            col = (230, 230, 230) if mono else colours[role]
            for dy in range(scale):
                for dx in range(scale):
                    px[xx * scale + dx, yy * scale + dy] = col
    img.save(path)
    print(f"wrote {path}")


def print_contrast(themes):
    """The ratios, from the same table the firmware draws from.

    Politeness, not enforcement: test/host/test_theme.c is what fails a build
    when a theme drops below the floor. Deleting this function would cost a
    convenient table and nothing else.
    """
    print(f"{'theme':22} {'PRIMARY':>8} {'SECONDARY':>10} {'ACCENT':>8}")
    for name, roles in themes.items():
        bg = roles[0]
        print(
            f"{name:22} {contrast(roles[1], bg):8.2f} "
            f"{contrast(roles[2], bg):10.2f} {contrast(roles[3], bg):8.2f}"
        )
    print("\n(WCAG ratios on the RGB565-quantised colours — what the panel shows.)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", metavar="PREFIX", help="also write PNG previews")
    ap.add_argument(
        "--contrast", action="store_true", help="print the contrast table and exit"
    )
    args = ap.parse_args()

    themes = parse_themes()
    if args.contrast:
        print_contrast(themes)
        return

    f3x5 = parse_font("main/font3x5.c", "3x5")
    f8x16 = parse_font("main/font8x16.c", "8x16")

    boards = [
        ("s3-oled042", 72, 40, f3x5, (1, 2), parse_ui_offsets(False)),
        ("s3-lcd147", 320, 172, f8x16, (2, 2), parse_ui_offsets(True)),
    ]

    for name, w, h, font, tall, y in boards:
        c = Canvas(w, h, font)
        draw_status(
            c,
            y,
            tall,
            mdns="airkvm-lcd",
            ip="192.168.100.100",
            ssid="asn-iot",
            rssi_bars=3,
            events=1234,
            usb=True,
            bound=True,
            debug="KeyA down",
        )
        cols, rows = w // font["advance"], h // font["line_h"]
        print(
            f"=== {name}: {w}x{h}, {font['name']} font, {cols} cols x {rows} rows ==="
        )
        print(ascii_art(c))
        print()
        if not args.png:
            continue

        # The 0.42" OLED is 1bpp and has no palette: every ink role is one lit
        # pixel there, so it gets exactly one image and themes are a no-op.
        # Rendering it five times in five colour schemes would be a picture of
        # a feature that board does not have.
        if font["name"] == "3x5":
            render_png(c, f"{args.png}-{name}.png", None, mono=True)
            continue
        for theme, colours in themes.items():
            render_png(c, f"{args.png}-{name}-{theme}.png", colours, mono=False)


if __name__ == "__main__":
    main()
