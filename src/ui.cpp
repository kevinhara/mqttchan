#include "ui.h"
#include <string.h>

namespace ui {

void centerStr(U8G2& g, int y, const char* s) {
  g.drawStr((W - g.getStrWidth(s)) / 2, y, s);
}

int trackedWidth(U8G2& g, const char* s, int track) {
  int w = 0, n = 0;
  for (const char* p = s; *p; p++) {
    char c[2] = {*p, 0};
    w += g.getStrWidth(c);
    n++;
  }
  if (n > 1) w += track * (n - 1);
  return w;
}

void trackedStr(U8G2& g, int y, const char* s, int track, int n) {
  int x = (W - trackedWidth(g, s, track)) / 2;
  if (x < 0) x = 0;
  int len = (int)strlen(s);
  if (n < 0 || n > len) n = len;
  for (int i = 0; i < n; i++) {
    char c[2] = {s[i], 0};
    g.drawStr(x, y, c);
    x += g.getStrWidth(c) + track;
  }
}

void haloStr(U8G2& g, int x, int y, const char* s, int r) {
  if (r < 0) r = 0;
  // Transparent font mode is load-bearing, not a tidy-up. u8g2 defaults to
  // *solid*, where a glyph drawn with colour 0 paints its bounding box in the
  // background colour (1) and the glyph itself in 0 — it inverts a rectangle
  // instead of erasing a letter shape. In solid mode the eight halo passes
  // fringe every glyph with stray bright box edges; on the splash mark that
  // fringe closed the L into a rectangle and it read as "H O I".
  g.setFontMode(1);
  g.setDrawColor(0);
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx || dy) g.drawStr(x + dx, y + dy, s);
  g.setDrawColor(1);
  g.drawStr(x, y, s);
  g.setFontMode(0);
}

void leader(char* dst, size_t cap, const char* label, const char* res) {
  if (!res || !*res) {
    strlcpy(dst, label, cap);
    return;
  }
  int ln = (int)strlen(label), rn = (int)strlen(res);
  int dots = COLS - ln - rn - 2;  // two spaces flank the leader
  if (dots < 1) dots = 1;

  size_t i = 0;
  for (int k = 0; label[k] && i + 1 < cap; k++) dst[i++] = label[k];
  if (i + 1 < cap) dst[i++] = ' ';
  for (int k = 0; k < dots && i + 1 < cap; k++) dst[i++] = '.';
  if (i + 1 < cap) dst[i++] = ' ';
  for (int k = 0; res[k] && i + 1 < cap; k++) dst[i++] = res[k];
  dst[i] = 0;
}

void cursor(U8G2& g, int col, int row, uint32_t el) {
  if ((el / 400) & 1) return;  // ~1.25Hz, the DOS blink rate
  if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
  g.drawBox(col * CW, row * CH + 1, CW - 1, CH - 2);
}

// Seven-segment geometry, straight off the reference sheet. Segment order is
// t, tl, tr, m, bl, br, b and DIGSEG packs it one bit per segment, LSB first —
// render.js spells the same thing out as strings, which does not survive being
// a lookup table in 2KB of .rodata.
namespace {
struct Seg {
  int8_t x, y, w, h;
};
constexpr Seg SEG[7] = {
    {0, 0, 14, 4},   // t
    {0, 0, 4, 13},   // tl
    {10, 0, 4, 13},  // tr
    {0, 9, 14, 4},   // m
    {0, 9, 4, 13},   // bl
    {10, 9, 4, 13},  // br
    {0, 18, 14, 4},  // b
};
constexpr uint8_t DIGSEG[10] = {119, 36, 93, 109, 46, 107, 123, 37, 127, 111};
constexpr int DIG_ADV   = 17;  // 14 wide + 3 gap
constexpr int COLON_ADV = 7;   // 4 wide + 3 gap
}  // namespace

int bigTimeWidth(const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) w += (*p == ':') ? COLON_ADV : DIG_ADV;
  return w - 3;  // no trailing gap
}

void bigTime(U8G2& g, int x, int y, const char* s, bool colonOn) {
  for (const char* p = s; *p; p++) {
    if (*p == ':') {
      if (colonOn) {
        g.drawBox(x, y + 4, 4, 4);
        g.drawBox(x, y + 14, 4, 4);
      }
      x += COLON_ADV;
      continue;
    }
    // '-' is the middle segment on its own, for "--:--" before NTP has
    // answered. render.js has no such character because the reference panel
    // was drawn with a time already in it; a clock has to be able to say it
    // does not know one.
    if (*p == '-') {
      g.drawBox(x + SEG[3].x, y + SEG[3].y, SEG[3].w, SEG[3].h);
      x += DIG_ADV;
      continue;
    }
    if (*p < '0' || *p > '9') continue;
    // A seven-segment '1' is only the two right-hand bars, so at a 17px pitch
    // it sits hard against the *next* digit and leaves a 10px hole before it.
    // On a passkey typed blind off this panel "284519" came out reading as
    // "2845 19". Nudging '1' to the middle of its own cell costs the strict
    // fidelity to render.js that everything else here keeps, and buys a number
    // you can read back. Deliberate divergence, noted in the README.
    int dx = (*p == '1') ? -5 : 0;
    uint8_t m = DIGSEG[*p - '0'];
    for (int i = 0; i < 7; i++)
      if (m & (1 << i))
        g.drawBox(x + dx + SEG[i].x, y + SEG[i].y, SEG[i].w, SEG[i].h);
    x += DIG_ADV;
  }
}

void chrome(U8G2& g, const char* title, const char* tag) {
  g.drawBox(0, 0, W, 7);
  g.setDrawColor(0);
  g.drawStr(2, 5, title);
  if (tag && *tag) g.drawStr(W - 2 - g.getStrWidth(tag), 5, tag);
  g.setDrawColor(1);
}

void bar(U8G2& g, int x, int y, int w, int h, int pct) {
  if (w < 2 || h < 2) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  g.drawFrame(x, y, w, h);
  int fill = ((w - 2) * pct + 50) / 100;
  if (fill > 0) g.drawBox(x + 1, y + 1, fill, h - 2);
}

void marquee(U8G2& g, int x, int y, int w, const char* s, uint32_t now) {
  int sw = g.getStrWidth(s);
  if (sw <= w) {
    g.drawStr(x, y, s);
    return;
  }
  // Pan the overflow at 18px/s with a 1.2s dwell at each end. A continuous
  // wrap-around scroll never lets the eye settle on the start of the title,
  // which is the part that identifies the track.
  const int over = sw - w;
  const uint32_t travel = (uint32_t)over * 1000 / 18;
  const uint32_t dwell = 1200;
  uint32_t period = 2 * (travel + dwell);
  uint32_t t = period ? now % period : 0;
  int off;
  if (t < dwell)
    off = 0;
  else if (t < dwell + travel)
    off = (int)((t - dwell) * over / (travel ? travel : 1));
  else if (t < 2 * dwell + travel)
    off = over;
  else
    off = over - (int)((t - 2 * dwell - travel) * over / (travel ? travel : 1));

  // Generous vertically: the window only has to bound the string horizontally,
  // and a tight top edge shaves the ascenders off 7x13B. Clamped at 0 because
  // u8g2's clip coordinates are unsigned and a negative wraps to ~65k.
  int top = y - 13;
  if (top < 0) top = 0;
  g.setClipWindow(x, top, x + w, y + 5);
  g.drawStr(x - off, y, s);
  g.setMaxClipWindow();
}

void chevron(U8G2& g, int x, int y, int h, int dir) {
  for (int i = 0; i < h; i++) {
    int row = dir > 0 ? y + i : y - i;
    int half = h - 1 - i;
    g.drawHLine(x - half, row, half * 2 + 1);
  }
}

void corners(U8G2& g, int m, int len) {
  const int x0 = m, y0 = m, x1 = W - 1 - m, y1 = H - 1 - m;
  g.drawHLine(x0, y0, len);
  g.drawVLine(x0, y0, len);
  g.drawHLine(x1 - len + 1, y0, len);
  g.drawVLine(x1, y0, len);
  g.drawHLine(x0, y1, len);
  g.drawVLine(x0, y1 - len + 1, len);
  g.drawHLine(x1 - len + 1, y1, len);
  g.drawVLine(x1, y1 - len + 1, len);
}

void scanBar(U8G2& g, int y, int h) {
  if (h <= 0 || y >= H || y + h <= 0) return;
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > H) h = H - y;
  g.setDrawColor(2);  // XOR
  g.drawBox(0, y, W, h);
  g.setDrawColor(1);
}

void staticBand(U8G2& g, int y, int h, uint32_t seed) {
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (h <= 0 || y >= H) return;
  if (y + h > H) h = H - y;
  for (int py = y; py < y + h; py++)
    for (int px = 0; px < W; px++)
      if (hash(px + py * W + seed * 7919u) & 0x10000) g.drawPixel(px, py);
}

}  // namespace ui
