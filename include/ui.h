// Shared drawing helpers. Nothing here knows about the boot sequence or the
// splash; both just need the same handful of primitives that a 1-bit panel
// makes awkward — tracking, haloed text, XOR bars, dot leaders.
#pragma once
#include <U8g2lib.h>
#include <stddef.h>
#include <stdint.h>

namespace ui {

// The panel in its native landscape orientation (U8G2_R0). Every layout
// constant in this project is written against these two numbers.
constexpr int W = 128;
constexpr int H = 64;

// The BIOS console grid: u8g2_font_5x8_tf cells, 25 columns x 8 rows. The
// 5x8 baseline sits at CH-1 within its row, leaving the descender row spare.
//
// Changed 2026-09-16 from u8g2_font_4x6_tf's 32x10 grid, to match the size
// of mqttchan's own "message screen" (SpeechBubble's default LGFX font —
// the classic 5x7-glyph/6x8-cell GLCD font; see speech_bubble.h) rather
// than the original HLI source project's tighter font. Only boot.cpp's
// console reads off CW/CH/COLS/ROWS — splash.cpp's kicker line kept the
// original 4x6 font and sizes its one CH-shaped call with a literal `6`
// instead, so it didn't move when this did.
constexpr int CW   = 5;
constexpr int CH   = 8;
constexpr int COLS = W / CW;   // 25
constexpr int ROWS = H / CH;   // 8

// Centre `s` on the panel at the given baseline, in the current font.
void centerStr(U8G2& g, int y, const char* s);

// Centred text with `track` extra pixels between glyphs, revealing only the
// first `n` characters (n < 0 = all). u8g2 has no letter-spacing, so glyphs go
// down one at a time — which is also what makes a per-character reveal free.
void trackedStr(U8G2& g, int y, const char* s, int track, int n = -1);
int  trackedWidth(U8G2& g, const char* s, int track);

// Bright glyph with an `r`-pixel dark halo all round. The splash mark sits
// directly on top of the sun, and bright-on-bright is unreadable in 1 bit — the
// halo is the gap that makes the letterform legible. r=1 is not enough against
// a fully lit disc; the mark uses r=2. Costs (2r+1)^2-1 glyph blits, so keep it
// for the handful of characters that need it.
//
// Caller must keep x,y >= r: the halo draws at x-r, y-r and u8g2 coordinates
// are unsigned (uint16_t), so a negative wraps to ~65k instead of clipping.
void haloStr(U8G2& g, int x, int y, const char* s, int r = 1);

// BIOS dot leader: "Video Adapter ............. EGA", padded to COLS so every
// result lands in the same right-hand column. `res` may be null (label only).
void leader(char* dst, size_t cap, const char* label, const char* res);

// Blinking block cursor in console cell (col, row). `el` drives the blink.
void cursor(U8G2& g, int col, int row, uint32_t el);

// XOR a bright bar `h` tall across the frame — inverts whatever it crosses,
// which is the cheapest convincing CRT scan artifact on a 1-bit panel.
// Clips vertically and restores draw colour 1.
void scanBar(U8G2& g, int y, int h);

// Fill a band with pixel noise: tape static, used for the splash's strap-line
// changes. `seed` should advance a few times a second, not every frame, or the
// noise averages out to grey mush.
void staticBand(U8G2& g, int y, int h, uint32_t seed);

// 22px seven-segment digits — the face the reference sheet's clock panel
// publishes, ported from design/.../tools/render.js. Only '0'..'9' and ':';
// this is a counter face, not a font. Digits are 14 wide on a 17px pitch, the
// colon 4 wide on 7, so "20:47" is 72px and centres at x=28.
//
// The colon is two solid blocks. `colonOn` toggles whether they're drawn —
// callers doing a clock face blink it at 1Hz — but the colon's column is
// always reserved at COLON_ADV regardless, so digits never shift when it's off.
void bigTime(U8G2& g, int x, int y, const char* s, bool colonOn = true);
int  bigTimeWidth(const char* s);

// The 7px inverse title bar every content screen wears: title flush left, tag
// flush right, content from y8. It is what makes the screens read as one
// machine rather than a pile of layouts. Expects the 4x6 font to be set, and
// leaves draw colour 1.
void chrome(U8G2& g, const char* title, const char* tag);

// Framed fill bar, `pct` clamped to 0..100. Track progress and volume.
void bar(U8G2& g, int x, int y, int w, int h, int pct);

// Text clipped to `w` px, panning back and forth if it overflows. A string
// that fits is drawn static at x. `now` drives the pan; there is a 1.2s dwell
// at each end so a title is readable rather than permanently sliding.
void marquee(U8G2& g, int x, int y, int w, const char* s, uint32_t now);

// Solid triangle `h` tall with its apex at (x, y), pointing up (dir < 0) or
// down (dir > 0). The menu's "there is more this way" affordance.
void chevron(U8G2& g, int x, int y, int h, int dir);

// Corner ticks inset `m` from the panel edge — the cheapest thing that makes a
// screen look framed without spending 4 rows on a border.
void corners(U8G2& g, int m, int len);

// Knuth multiplicative hash — deterministic "random" without a PRNG.
inline uint32_t hash(uint32_t n) { return n * 2654435761u; }

}  // namespace ui
