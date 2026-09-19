#!/usr/bin/env python3
"""Regenerates firmware/include/portraits_data.h from the bundled
"1-bit dialogue portraits" pack (assets/1-bit dialogue portraits/x2
64x64px/*.png at the repo root - see that folder's license.txt for terms).

Run from anywhere; paths below are relative to this file, not the cwd:

    python3 firmware/tools/gen_portraits.py

_GROUPS below is the frame manifest: which numbered portrait files (1-225,
matching the filenames in x2 64x64px/) are frames of the same character, in
the pack's own frame order. The pack ships no such manifest, so this was
recovered automatically and then spot-checked by eye:

  1. Downsample each portrait to 8x8 grayscale (kills the per-frame dither
     noise in hair/clothes that makes a naive full-resolution pixel diff
     unusable - two frames of the same character differ there by a few
     hundred; two different characters differ by several thousand).
  2. Sum the absolute difference between consecutive portraits in that
     space; a new group starts wherever it jumps past ~1700 (there's a
     clean gap in the sorted diffs between ~1600 and ~1900 - nothing landed
     in between).
  3. Rendered contact sheets and eyeballed the grouping against several
     ranges (1-16, 108-123, 150-165, 195-225) - all matched frame-for-frame.

99 characters, 1-4 frames each, 225 files total. Re-run this whole process
(not just re-embed _GROUPS by hand) if the pack is ever replaced - the
threshold was tuned against this specific pack's dither pattern.

Two characters (index 44 and 66 below - portraits 112 and 156) ship only one
frame each, i.e. no second frame for PortraitFace's idle blink to flash to.
Rather than exclude them from blinking, _SYNTHETIC_BLINKS below synthesizes
one: for each character, one or more (x0, y0, x1, y1) boxes hand-picked by
eye (pun noted) against that character's own art - see the box-finding
crops this was done with, not kept in the repo, but reproducible by cropping
+ upscaling the source PNG around the eye and reading pixel coordinates off
a grid overlay. make_blink() below fills each box solid (closing the eye)
and cuts a thin light line back through its vertical middle (an eyelid
crease) - the same flat "line on an otherwise dark shape" convention this
pack already uses elsewhere. Added 2026-09-19 after a first pass gave these
two a head-nod instead of a blink, which wasn't what was asked for.
"""
import pathlib

from PIL import Image

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
ASSET_DIR = REPO_ROOT / "assets" / "1-bit dialogue portraits" / "x2 64x64px"
OUT_PATH = SCRIPT_DIR.parent / "include" / "portraits_data.h"

W = H = 64
ROW_BYTES = (W + 7) // 8  # 8, since W is byte-aligned

# Character index (position in _GROUPS below) -> eye box(es) in source-image
# (64x64) coordinates, (x0, y0, x1, y1), half-open like a Python slice. See
# the module docstring.
_SYNTHETIC_BLINKS = {
    44: [(16, 21, 29, 37), (35, 21, 48, 37)],  # portrait 112, two eyes
    66: [(9, 31, 16, 39)],  # portrait 156, one eye (3/4 profile)
}


def make_blink(im, boxes, line_height=2):
    """Returns a copy of grayscale image `im` with each eye box closed - see
    _SYNTHETIC_BLINKS above."""
    im = im.copy()
    px = im.load()
    for x0, y0, x1, y1 in boxes:
        for y in range(y0, y1):
            for x in range(x0, x1):
                px[x, y] = 0
        mid = (y0 + y1) // 2
        for y in range(mid, mid + line_height):
            for x in range(x0 + 2, x1 - 2):
                px[x, y] = 255
    return im


# See the module docstring for how this was derived.
_GROUPS = [
    [1, 2], [3, 4], [5, 6, 7, 8], [9, 10, 11], [12, 13, 14, 15], [16, 17, 18],
    [19, 20, 21], [22, 23], [24, 25, 26], [27, 28, 29, 30], [31, 32],
    [33, 34], [35, 36, 37], [38, 39, 40], [41, 42], [43, 44, 45, 46],
    [47, 48], [49, 50, 51], [52, 53, 54], [55, 56], [57, 58], [59, 60],
    [61, 62], [63, 64], [65, 66], [67, 68], [69, 70], [71, 72], [73, 74],
    [75, 76], [77, 78], [79, 80], [81, 82], [83, 84], [85, 86], [87, 88],
    [89, 90], [91, 92, 93, 94], [95, 96, 97], [98, 99, 100, 101],
    [102, 103], [104, 105, 106, 107], [108, 109], [110, 111], [112],
    [113, 114, 115], [116, 117], [118, 119], [120, 121], [122, 123],
    [124, 125], [126, 127], [128, 129], [130, 131], [132, 133], [134, 135],
    [136, 137], [138, 139], [140, 141], [142, 143], [144, 145], [146, 147],
    [148, 149], [150, 151], [152, 153], [154, 155], [156], [157, 158],
    [159, 160], [161, 162], [163, 164, 165], [166, 167], [168, 169],
    [170, 171, 172], [173, 174], [175, 176, 177], [178, 179], [180, 181],
    [182, 183], [184, 185], [186, 187, 188], [189, 190], [191, 192],
    [193, 194], [195, 196], [197, 198, 199], [200, 201], [202, 203],
    [204, 205], [206, 207], [208, 209], [210, 211], [212, 213], [214, 215],
    [216, 217], [218, 219], [220, 221], [222, 223], [224, 225],
]


def load_gray(portrait_num):
    """Loads one portrait PNG flattened onto white, as a grayscale image -
    the shared first step for pack_bits() and make_blink()."""
    im = Image.open(ASSET_DIR / f"portrait {portrait_num}.png").convert("RGBA")
    bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
    bg.paste(im, (0, 0), im)
    return bg.convert("L")


def pack_bits(im):
    """Packs one grayscale image into LGFX's drawBitmap() format: 1bpp,
    MSB-first per byte, rows padded to a byte boundary - verified against
    LGFXBase::draw_bitmap() in M5GFX, and round-tripped pixel-for-pixel
    against the source art. A pixel is "on" (bit set, drawn in the fg
    color) wherever the source art is dark ink, so it renders as lit
    (white) linework on the OLED's black background - see portrait_face.h.
    """
    px = im.load()
    out = []
    for y in range(H):
        for b in range(ROW_BYTES):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                on = 1 if (x < W and px[x, y] < 128) else 0
                byte = (byte << 1) | on
            out.append(byte)
    return out


def main():
    assert sum(len(g) for g in _GROUPS) == 225
    assert len(_GROUPS) == 99

    header = '''// Generated by firmware/tools/gen_portraits.py from the "1-bit dialogue
// portraits" pack (assets/1-bit dialogue portraits/x2 64x64px/*.png at the
// repo root; see that folder's license.txt for terms). Do not hand-edit -
// re-run the generator (see its module docstring for how the frame
// grouping below was derived) if the source pack changes. A couple of
// frames (marked below) are synthetic, not from the pack - see
// _SYNTHETIC_BLINKS in the generator.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace portraits {

constexpr int kWidth = 64;
constexpr int kHeight = 64;

'''
    lines = [header]

    synthetic_frames = 0
    char_frame_vars = []
    for ci, group in enumerate(_GROUPS):
        frame_vars = []
        last_im = None
        for fi, portrait_num in enumerate(group):
            last_im = load_gray(portrait_num)
            bits = pack_bits(last_im)
            varname = f"kFrame_{ci}_{fi}"
            hexes = ", ".join(f"0x{b:02x}" for b in bits)
            lines.append(f"static const uint8_t {varname}[] = {{{hexes}}};\n")
            frame_vars.append(varname)

        boxes = _SYNTHETIC_BLINKS.get(ci)
        if boxes is not None:
            assert len(group) == 1, (
                f"character {ci} has a synthetic blink box but already has "
                f"{len(group)} real frames - drop it from _SYNTHETIC_BLINKS"
            )
            bits = pack_bits(make_blink(last_im, boxes))
            varname = f"kFrame_{ci}_{len(frame_vars)}"
            hexes = ", ".join(f"0x{b:02x}" for b in bits)
            lines.append(
                f"static const uint8_t {varname}[] = {{{hexes}}};  "
                f"// synthetic blink, see _SYNTHETIC_BLINKS\n"
            )
            frame_vars.append(varname)
            synthetic_frames += 1

        char_frame_vars.append(frame_vars)
    lines.append("\n")

    for ci, frame_vars in enumerate(char_frame_vars):
        joined = ", ".join(frame_vars)
        lines.append(f"static const uint8_t *const kChar_{ci}_frames[] = {{{joined}}};\n")
    lines.append("\n")

    lines.append("struct Portrait {\n")
    lines.append("  const uint8_t *const *frames;\n")
    lines.append("  uint8_t frameCount;\n")
    lines.append("};\n\n")
    lines.append("static const Portrait kPortraits[] = {\n")
    for ci, frame_vars in enumerate(char_frame_vars):
        lines.append(f"    {{kChar_{ci}_frames, {len(frame_vars)}}},\n")
    lines.append("};\n\n")
    lines.append(
        "constexpr size_t kPortraitCount = sizeof(kPortraits) / sizeof(kPortraits[0]);\n\n"
    )
    lines.append("}  // namespace portraits\n")

    OUT_PATH.write_text("".join(lines))
    print(f"wrote {OUT_PATH}")
    print(
        f"characters: {len(_GROUPS)}, "
        f"total frames: {sum(len(g) for g in _GROUPS) + synthetic_frames} "
        f"({synthetic_frames} synthetic)"
    )


if __name__ == "__main__":
    main()
