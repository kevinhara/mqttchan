#include "boot.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "brand.h"
#include "ui.h"

namespace boot {

// The sequence is a *pure function of elapsed ms*: every frame rebuilds the
// whole console from SCRIPT instead of appending to a persistent buffer. That
// costs a dozen small string builds a frame (nothing, on a 240MHz part) and
// buys two things worth having — the scroll and the typing are seekable, so a
// LOOP_BOOT build can be replayed and retimed without power-cycling, and there
// is no state to get out of step if a frame is dropped.

constexpr uint32_t CRT_END = 420;  // cold tube strikes, then the BIOS blinks
constexpr uint32_t TYPE_MS = 14;   // ms per character on a typed label
constexpr uint32_t HOLD_MS = 700;  // loader cursor blinks before hand-off
constexpr int      MEM_K   = 640;  // the number, obviously

enum Kind : uint8_t {
  BANNER,  // instant, inverse video across the full row (vendor line)
  PLAIN,   // instant, plain text
  TYPE,    // label types out; dot leader + result snap in at resAt
  MEM,     // memory counter, result appended at resAt
  BLANK,   // spacer
};

struct Step {
  uint16_t    at;     // ms this line starts printing
  uint16_t    resAt;  // ms the right-hand result lands (0 = no result)
  uint8_t     kind;
  const char* label;
  const char* res;
};

// MUST stay sorted by `at` — the renderer stops at the first step that has not
// started yet, which is what keeps the per-frame rebuild cheap.
//
// The header block lands whole rather than typing, because on real hardware the
// video BIOS is already up by then and prints it in one go; only the self test
// below has the character-by-character cadence. Two of these are brand jokes
// rather than 286 hardware ("Cassette Bus", "Photon Array") — a light
// industries company would test its own bus.
static const Step SCRIPT[] = {
    { 420,    0, BANNER, brand::NAME,               nullptr},
    { 420,    0, PLAIN,  brand::BIOS,               nullptr},
    { 420,    0, PLAIN,  brand::COPY,               nullptr},
    { 620,    0, BLANK,  nullptr,                   nullptr},
    { 700, 2350, MEM,    "Memory Test",             "OK"    },
    {2500, 2700, TYPE,   "80286-12 CPU",            "12 MHz"},
    {2720, 2920, TYPE,   "Coprocessor",             "NONE"  },
    {2940, 3140, TYPE,   "Video Adapter",           "EGA"   },
    {3160, 3360, TYPE,   "Keyboard",                "OK"    },
    {3380, 3620, TYPE,   "Fixed Disk 0",            "20M"   },
    {3640, 3880, TYPE,   "Cassette Bus",            "READY" },
    {3900, 4160, TYPE,   "Photon Array",            "OK"    },
    {4180, 4380, TYPE,   "Serial 1,2",              "OK"    },
    {4400, 4600, TYPE,   "Real Time Clock",          "OK"   },
    {4620, 4820, TYPE,   "HLI Sync Bus",            "OK"    },
    {4980,    0, BLANK,  nullptr,                   nullptr},
    {5060,    0, TYPE,   "Booting from Fixed Disk 0", nullptr},
    {5460,    0, TYPE,   brand::DOS,                nullptr},
};
static const int N = sizeof(SCRIPT) / sizeof(SCRIPT[0]);

// `started` is an explicit flag rather than a startMs != 0 test: millis() is
// legitimately 0 for the first millisecond after reset, and a zero-sentinel
// would leave done() false forever if begin() landed there.
static uint32_t startMs = 0;
static bool     started = false;

// Console window, rebuilt each frame. File-static rather than on the stack so
// a 10x33 buffer plus flags is not paid for out of the loop task's stack.
static char sLines[ui::ROWS][ui::COLS + 1];
static bool sInv[ui::ROWS];
static int  sN;        // lines currently in the window (<= ui::ROWS)
static int  sCursorCol;

static void push(const char* txt, bool invert) {
  if (sN < ui::ROWS) {
    strlcpy(sLines[sN], txt, ui::COLS + 1);
    sInv[sN] = invert;
    sN++;
    return;
  }
  for (int i = 1; i < ui::ROWS; i++) {  // scroll
    memcpy(sLines[i - 1], sLines[i], ui::COLS + 1);
    sInv[i - 1] = sInv[i];
  }
  strlcpy(sLines[ui::ROWS - 1], txt, ui::COLS + 1);
  sInv[ui::ROWS - 1] = invert;
}

// When the last line has finished printing.
static uint32_t scriptEnd() {
  uint32_t end = 0;
  for (int i = 0; i < N; i++) {
    uint32_t e = SCRIPT[i].at;
    if (SCRIPT[i].label) e += strlen(SCRIPT[i].label) * TYPE_MS;
    if (SCRIPT[i].resAt > e) e = SCRIPT[i].resAt;
    if (e > end) end = e;
  }
  return end;
}

void begin() {
  startMs = millis();
  started = true;
}

uint32_t durationMs() { return scriptEnd() + HOLD_MS; }

bool done() { return started && (millis() - startMs) >= durationMs(); }

// A cold CRT striking: the beam lights as a thin band across the middle, blooms
// to fill the tube, then collapses as the video signal is acquired. This is the
// half-second before any BIOS gets a word in, and it is what sells "old screen"
// more than any amount of text does.
static void drawCrt(U8G2& g, uint32_t el) {
  if (el < 60) return;  // tube still dark
  if (el < 210) {
    float p = (el < 150) ? (el - 60) / 90.0f            // bloom out
                         : 1.0f - (el - 150) / 60.0f;   // collapse back
    int h = 1 + (int)(p * (ui::H - 1));
    if (h > ui::H) h = ui::H;
    g.drawBox(0, (ui::H - h) / 2, ui::W, h);
    return;
  }
  ui::cursor(g, 0, 0, el - 210);  // BIOS has the screen
}

void draw(U8G2& g) {
  uint32_t el = millis() - startMs;
  g.clearBuffer();

  if (el < CRT_END) {
    drawCrt(g, el);
    g.sendBuffer();
    return;
  }

  sN = 0;
  sCursorCol = 0;
  for (int i = 0; i < N; i++) {
    const Step& s = SCRIPT[i];
    if (el < s.at) break;  // sorted by `at`, so nothing later has started

    char buf[ui::COLS + 1];
    switch (s.kind) {
      case BLANK:
        buf[0] = 0;
        break;

      case BANNER:
      case PLAIN:
        strlcpy(buf, s.label, sizeof(buf));
        break;

      case MEM: {
        // Counts 0 -> 640K in 8K chunks across its window, the way a 286 walked
        // memory visibly. Snapped to 8 so the digits churn instead of flickering
        // through every value the frame rate happens to land on.
        uint32_t span = s.resAt > s.at ? s.resAt - s.at : 1;
        float    p    = (float)(el - s.at) / span;
        if (p > 1.0f) p = 1.0f;
        int k = (int)(MEM_K * p) & ~7;
        if (p >= 1.0f) k = MEM_K;
        snprintf(buf, sizeof(buf), "Memory Test : %4dK%s", k,
                 (s.resAt && el >= s.resAt) ? " OK" : "");
        break;
      }

      case TYPE: {
        size_t   lab   = strlen(s.label);
        uint32_t shown = (el - s.at) / TYPE_MS;
        if (shown < lab) {  // still typing the label
          memcpy(buf, s.label, shown);
          buf[shown] = 0;
        } else {
          ui::leader(buf, sizeof(buf), s.label,
                     (s.resAt && el >= s.resAt) ? s.res : nullptr);
        }
        break;
      }
    }
    push(buf, s.kind == BANNER);
    sCursorCol = (int)strlen(buf);
  }

  // Past the last line the loader gets a fresh line of its own to blink on —
  // the beat between "Starting HLI-DOS" and the screen clearing to the splash.
  bool onNewLine = el >= scriptEnd();
  if (onNewLine) {
    push("", false);
    sCursorCol = 0;
  }

  g.setFont(u8g2_font_5x8_tf);  // matches the message screen's font/size
  for (int r = 0; r < sN; r++) {
    int y = r * ui::CH + ui::CH - 1;  // 5x8 baseline within its row
    if (sInv[r]) {
      g.drawBox(0, r * ui::CH, ui::W, ui::CH);
      g.setDrawColor(0);
      g.drawStr(1, y, sLines[r]);
      g.setDrawColor(1);
    } else {
      g.drawStr(0, y, sLines[r]);
    }
  }
  ui::cursor(g, sCursorCol, sN - 1, el);
  g.sendBuffer();
}

}  // namespace boot
