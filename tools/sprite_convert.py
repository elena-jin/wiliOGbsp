#!/usr/bin/env python3
"""Crop the Photon character sheet into RGB565 frames for the ST7789.

The panel driver stores pixels the way st7789_rgb565() does (native
RGB565, high byte first on the wire inside st7789_blit). Blits are
opaque, so every sheet-background pixel is rewritten to one solid
color that the app also uses to clear the screen.

Boxes were measured on assets/character-sheet.png at 1672x941:
Default (happy) is the map puppy, Excited is the sparkle puppy.
If that file is missing or a different size, this draws a small
two-frame puppy in the same colors instead of guessing a crop.
"""

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("sprite_convert: install Pillow in the venv (pip install pillow)")

ROOT = Path(__file__).resolve().parents[1]
SHEET = ROOT / "assets" / "character-sheet.png"
HEADER = ROOT / "apps" / "puppy" / "display" / "puppy_sprites.h"
PREVIEW = ROOT / "apps" / "puppy" / "preview"

SHEET_SIZE = (1672, 941)
# Inclusive pixel bounds from a connected-component pass, then checked by eye.
DEFAULT_BOX = (446, 298, 611, 461)
EXCITED_BOX = (966, 316, 1117, 465)

# 3x the old 48px frame. 2x (96) also fits, but 144 still leaves room for the
# jump: three steps of 8px is 24px, and 144 + 24 = 168 <= the 240px panel.
# Crop stays in sheet pixels; the resize below is nearest-neighbor, so the
# pixels stay hard (no blur).
FRAME = 144
# Fallback art is drawn on this grid, then nearest-scaled up to FRAME.
NATIVE = 48
# Sheet paper. st7789_rgb565(251, 251, 240) == 0xFFDE.
BG = (251, 251, 240)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def is_paper(rgb):
    r, g, b = rgb
    return r > 235 and g > 235 and b > 215 and abs(r - g) < 20


def flatten(im):
    out = im.convert("RGB")
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            if is_paper(px[x, y]):
                px[x, y] = BG
    return out


def scale_box(sheet, box):
    # Native-resolution crop, then a hard nearest-neighbor scale. Bilinear
    # would blur the sheet's pixels.
    crop = sheet.crop(box)
    scaled = crop.resize((FRAME, FRAME), Image.Resampling.NEAREST)
    return flatten(scaled)


def draw_fallback():
    """A small puppy: cream body, brown ear, purple bandana, pink cheeks.
    Frame 1 adds yellow sparkles. Drawn at NATIVE, then nearest-scaled to
    FRAME. Used only when the sheet cannot be cropped.
    """
    frames = []
    for excited in (False, True):
        im = Image.new("RGB", (NATIVE, NATIVE), BG)
        px = im.load()

        def dot(x, y, c):
            if 0 <= x < NATIVE and 0 <= y < NATIVE:
                px[x, y] = c

        def rect(x0, y0, x1, y1, c):
            for y in range(y0, y1):
                for x in range(x0, x1):
                    dot(x, y, c)

        cream = (252, 236, 214)
        brown = (146, 96, 64)
        purple = (124, 92, 196)
        pink = (244, 160, 170)
        black = (40, 32, 36)
        white = (255, 255, 255)
        yellow = (255, 214, 64)
        rect(12, 16, 36, 40, cream)
        rect(10, 18, 38, 36, cream)
        rect(26, 14, 36, 24, brown)
        rect(16, 20, 22, 26, black)
        rect(28, 20, 34, 26, black)
        dot(17, 21, white)
        dot(29, 21, white)
        rect(18, 28, 22, 31, pink)
        rect(28, 28, 32, 31, pink)
        rect(14, 32, 34, 38, purple)
        dot(23, 34, white)
        dot(24, 34, white)
        if excited:
            for sx, sy in ((8, 10), (38, 12), (6, 28), (40, 26), (20, 8)):
                dot(sx, sy, yellow)
                dot(sx + 1, sy, yellow)
        frames.append(im.resize((FRAME, FRAME), Image.Resampling.NEAREST))
    return frames


def load_frames():
    if not SHEET.is_file():
        print(f"sprite_convert: {SHEET} missing, drawing a fallback puppy")
        return draw_fallback(), True
    sheet = Image.open(SHEET).convert("RGB")
    if sheet.size != SHEET_SIZE:
        print(f"sprite_convert: sheet is {sheet.size}, expected {SHEET_SIZE}; drawing a fallback puppy")
        return draw_fallback(), True
    return [scale_box(sheet, DEFAULT_BOX), scale_box(sheet, EXCITED_BOX)], False


def emit_array(name, im):
    px = im.load()
    words = []
    for y in range(FRAME):
        for x in range(FRAME):
            r, g, b = px[x, y]
            words.append(f"0x{rgb565(r, g, b):04X}")
    lines = [f"static const uint16_t {name}[{FRAME * FRAME}] = {{"]
    for i in range(0, len(words), 12):
        lines.append("    " + ", ".join(words[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    frames, fallback = load_frames()
    names = ("puppy_default", "puppy_excited")
    files = ("default.png", "excited.png")
    PREVIEW.mkdir(parents=True, exist_ok=True)
    HEADER.parent.mkdir(parents=True, exist_ok=True)
    for im, fn in zip(frames, files):
        im.save(PREVIEW / fn)
        print(f"preview {PREVIEW / fn}")
    bg = rgb565(*BG)
    body = "\n\n".join(emit_array(n, im) for n, im in zip(names, frames))
    note = "fallback drawing" if fallback else "cropped from assets/character-sheet.png"
    flash_bytes = 2 * FRAME * FRAME * 2
    print(f"sprite flash {flash_bytes} bytes ({FRAME}x{FRAME}, two const frames)")
    HEADER.write_text(
        f"""/* Generated by tools/sprite_convert.py. {note}.
 * {FRAME}x{FRAME}, row-major, st7789_rgb565() byte order (not pre-swapped).
 * Paper background is st7789_rgb565{BG} = 0x{bg:04X}.
 * static const: both frames stay in flash (.rodata), {flash_bytes} bytes total. */
#ifndef PUPPY_SPRITES_H
#define PUPPY_SPRITES_H
#include <stdint.h>

#define PUPPY_W  {FRAME}u
#define PUPPY_H  {FRAME}u
#define PUPPY_BG 0x{bg:04X}u

{body}

#endif
"""
    )
    print(f"header {HEADER} fallback={fallback}")


if __name__ == "__main__":
    main()
