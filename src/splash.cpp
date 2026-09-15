#include "splash.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "brand.h"
#include "ui.h"

namespace splash {

// ---------------------------------------------------------------- layout
// The outrun stack squeezed into 64 rows, measured off HORIZON: 原 high in the
// sky, a slit sun half-sunk on the horizon below it, a flat city skyline along
// the horizon, grid floor beneath, and the strap line on a lower-third plate at
// the bottom.
//
// The horizon sits low (44 of 64) to buy sky. The mark and the sun must not
// overlap at all, and that is a hard constraint rather than a taste call: the
// mark's dark halo carves a bite out of the disc, and what is left reads as a
// pedestal the mark is standing on. Two earlier attempts (mark over the sun with
// a 1px halo, then 2px) both failed this way. Sky 0..43, floor 44..63.
constexpr int HORIZON = 44;
constexpr int SUN_CX  = 64;
// Crown at y33. The mark's ink ends at y30 and its halo at y32, so the disc
// starts on the very next row — measured off the settled frame by sim_main.cpp,
// not estimated. Growing SUN_R is what breaks first if this scene is retouched.
constexpr int SUN_R   = 11;
constexpr int KICK_Y  = 62;  // baseline of the 4x6 strap line

// How far below the disc's top edge the slit bands start, so the crown stays a
// clean solid dome and the bands fall in the strip just above the horizon where
// they read as the sun sinking into it.
constexpr int SUN_SLIT_TOP = 6;

// The mark. 原 is drawn as a 32px bitmap (see hara() below) rather than set in a
// font: there is no CJK face small enough to carry on this part, and at 32px the
// strokes are hand-placed anyway. Settled origin is x48, y-1 — the top bar runs
// off the top edge by design, which is what makes it read as a stamp rather than
// a centred logo.
constexpr int MARK_X    = 48;
constexpr int MARK_Y    = -1;
constexpr int MARK_DROP = 10;  // px it travels on the way in
// 2px, not 1: the mark sits over the grid and the skyline once it settles, and
// 1px of dark is not enough separation for a 2-3px stroke on a lit background.
constexpr int MARK_HALO = 2;

// Grid. Rows are placed on a power curve rather than a true 1/z perspective,
// and that is a deliberate cheat: with only 29 rows of floor, honest 1/z puts
// every row past the fourth within a pixel of its neighbour, so they merge into
// a solid bar under the horizon instead of reading as a receding plane. (That
// is exactly what the first render did.) u = 0 at the horizon, 1 at the bottom
// edge, y = HORIZON + depth * u^GRID_POW: pow 2 gives separations of
// 2,2,3,4,5,6,7 px across 8 rows, which fills the floor and never merges.
constexpr int   GRID_ROWS  = 8;
constexpr float GRID_POW   = 2.0f;   // > 1 compresses toward the horizon
constexpr float GRID_SPEED = 0.85f;  // rows per second at a cruise
constexpr int   LANE_N      = 4;     // lanes either side of centre
constexpr int   LANE_SPREAD = 15;    // px per lane at the bottom edge

// ---------------------------------------------------------------- timeline
// Two movements. The world builds in the first second, then the approach runs
// at it for three — and that second beat is the whole point of the sequence
// being 4.8s instead of 1.8s. The build alone gave a finished postcard with
// nothing at stake; the run gives the slam something to arrive from.
constexpr uint32_t T_HORIZON  = 0,    D_HORIZON  = 180;   // wipe out from centre
constexpr uint32_t T_GRID     = 140,  D_GRID     = 560;   // floor grows forward
constexpr uint32_t T_SKY      = 380,  D_SKY      = 340;   // skyline rises
constexpr uint32_t T_SUN      = 520,  D_SUN      = 430;   // sun climbs, half-sinks
constexpr uint32_t T_APPROACH = 950,  D_APPROACH = 3000;  // the run at the city
constexpr uint32_t T_FLASH    = 3950, D_FLASH    = 70;    // full-frame XOR slam
constexpr uint32_t T_MARK     = 3990, D_MARK     = 300;   // 原 lands
constexpr uint32_t T_KICK     = 4300, D_KICK     = 420;   // strap line reveals
constexpr uint32_t INTRO_END  = 4780;

// ---------------------------------------------------------------- the approach
// How much bigger the skyline gets by the moment of the slam. Past ~1.7 the
// whole outer half of the tower table has left the frame and the run thins to
// five blocks with gaps you can see the sky through.
constexpr float SKY_NEAR = 1.6f;

// Extra rows of floor the surge covers, on top of what a cruise would have.
// This is an INTEGER on purpose. Scroll phase is what positions every grid row,
// so a whole number of rows means the attract loop settles onto exactly the
// scroll the reference sheet publishes instead of some arbitrary offset from it.
// Tune the feel with this, not with a peak speed — the peak falls out of it.
constexpr float GRID_SURGE = 4.0f;
constexpr float GRID_RUSH =  // rows/s at the moment of arrival, ~4x cruise
    GRID_SPEED + 2.0f * GRID_SURGE / (D_APPROACH / 1000.0f);

// Attract loop.
constexpr uint32_t KICK_HOLD  = 3600;  // ms per strap line
constexpr uint32_t KICK_HISS   = 130;  // ms of static as it changes
constexpr float    SCAN_CYCLE  = 3.5f; // s between scan sweeps
constexpr float    SCAN_TRAVEL = 1.2f; // s for one sweep top to bottom
// During the approach the bar whips instead of drifting, and comes round three
// times across the three seconds. It is doing work there that it does not do in
// the attract loop: the city grows smoothly and the floor is a repeating
// pattern, so without something crossing the tube on its own clock the run reads
// as a slow zoom on a still frame.
constexpr float    SCAN_RUSH_CYCLE  = 1.0f;
constexpr float    SCAN_RUSH_TRAVEL = 0.55f;

// `started` is an explicit flag rather than a startMs != 0 test: millis() is
// legitimately 0 for the first millisecond after reset, and a zero-sentinel
// wedges the whole sequence at frame one if begin() lands there.
static uint32_t startMs = 0;
static bool     started = false;

// ---------------------------------------------------------------- the theme
// playTheme()'s state (see the function itself, near the bottom of this
// file, for the monophonic synthwave arpeggio it drives on the piezo - the
// "retro cassette" half of the brief) - declared up here, not next to it,
// because begin() below needs to reset it and C++ has no forward
// declaration for file-scope variables the way it does for functions.
//
// Driven by an explicit stage machine rather than re-deriving "which beat is
// this" from elapsed time on every call: T_MARK (3990) falls inside
// [T_FLASH, T_FLASH+D_FLASH) = [3950, 4020), so matching stages by time
// window directly would fire the slam beat a second time instead of the
// mark beat. A stage that only ever advances forward, one beat at a time,
// can't make that mistake.
enum ThemeStage { STAGE_BUILD, STAGE_APPROACH, STAGE_SLAM, STAGE_MARK,
                   STAGE_KICK, STAGE_DONE };
static ThemeStage themeStage  = STAGE_DONE;
static uint32_t   themeNextMs = 0;
static int        themeBeat   = 0;

void begin() {
  startMs = millis();
  started = true;
  themeStage  = STAGE_BUILD;
  themeNextMs = 0;
  themeBeat   = 0;
}
uint32_t elapsed() { return started ? millis() - startMs : 0; }
bool intro() { return elapsed() < INTRO_END; }

static inline float ramp(uint32_t el, uint32_t at, uint32_t dur) {
  if (el <= at) return 0.0f;
  if (el >= at + dur) return 1.0f;
  return (float)(el - at) / dur;
}

// Round-to-nearest that also works below zero. The dolly pushes tower origins
// off the left edge, and (int)(v + 0.5f) truncates toward zero there — it turns
// -3.7 into -3, so the left half of the skyline drifts a pixel right of where
// the right half sits and the run stops being symmetric about the sun.
static inline int iround(float v) { return (int)floorf(v + 0.5f); }

// How much nearer the city is at `el`, as a scale factor about the vanishing
// point. True perspective — apparent size goes as 1/distance — rather than the
// grid's power-curve cheat below, because here it earns its keep: closing a
// fixed fraction of the gap per second makes the growth accelerate on its own,
// and that acceleration is the suspense. A linear ramp reads as a zoom effect;
// this reads as travel.
//
// Back to 1.0 on the slam. The 70ms full-frame XOR is a cut, and what it cuts to
// is the settled hero composition the sheet publishes — arriving *is* landing on
// the title card, so the run has to end rather than freeze at its widest.
static inline float skyZoom(uint32_t el) {
  if (el < T_APPROACH || el >= T_FLASH) return 1.0f;
  float p = ramp(el, T_APPROACH, D_APPROACH);
  return 1.0f / (1.0f - (1.0f - 1.0f / SKY_NEAR) * p);
}

// Rows the floor has scrolled by time t. Integrated piecewise rather than
// sampled, so it stays a pure function of elapsed ms *and* stays continuous
// across both speed changes — a jump in phase snaps every row on the floor at
// once, which is far more visible than the change in speed it was meant to
// express. Constant, then a linear ramp to GRID_RUSH, then back to a cruise.
static float gridPhase(float t) {
  const float tA = T_APPROACH / 1000.0f;
  const float tB = T_FLASH / 1000.0f;
  if (t <= tA) return t * GRID_SPEED;
  // The ramp contributes exactly GRID_SURGE rows over [tA,tB] by construction,
  // which is what leaves the tail below as plain cruise-plus-a-whole-number.
  if (t >= tB) return t * GRID_SPEED + GRID_SURGE;
  const float a = (GRID_RUSH - GRID_SPEED) / (tB - tA);
  const float d = t - tA;
  return t * GRID_SPEED + 0.5f * a * d * d;
}

// u8g2 coordinates are unsigned (uint16_t), so a negative x or y wraps to ~65k
// instead of clipping — the mark is drawn above the top edge and its halo goes
// further still, so every box it lays down has to be clipped here first.
static void clipBox(U8G2& g, int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > ui::W) w = ui::W - x;
  if (y + h > ui::H) h = ui::H - y;
  if (w > 0 && h > 0) g.drawBox(x, y, w, h);
}

// ---------------------------------------------------------------- the sun
// Filled disc with horizontal slits knocked back out of it. The lit stripes
// stay 3px while the gaps widen going down, which is the only way 1 bit can
// suggest a gradient — then the whole lower half is cleared so the disc reads
// as half-sunk rather than floating.
static void drawSun(U8G2& g, int cy) {
  g.drawDisc(SUN_CX, cy, SUN_R);

  g.setDrawColor(0);
  int y = cy - SUN_R + SUN_SLIT_TOP;  // crown stays solid
  int gap = 1;
  while (y < cy + SUN_R) {
    g.drawBox(SUN_CX - SUN_R, y, SUN_R * 2 + 1, gap);
    y += gap + 3;
    if (gap < 4) gap++;
  }
  g.drawBox(0, HORIZON, ui::W, ui::H - HORIZON);  // clip to the sky
  g.setDrawColor(1);
}

// ---------------------------------------------------------------- skyline
// A flat city on the horizon, replacing the palms an earlier cut had here: palms
// put two tall silhouettes at the edges of a frame whose subject is already tall
// and central, and the eye had three things competing for the sky. A skyline is
// horizontal by nature, so it reads as ground rather than as another object.
//
// Table is {x, height}; each block runs to one pixel short of the next block's x,
// which leaves a 1px alley between neighbours so the outlines stay separate. The
// centre of the run (x 45..86) is deliberately kept low — that is where the sun
// sits, and anything over 5px tall there swallows the disc.
static const uint8_t TOWERS[][2] = {
    {0, 13},  {9, 9},   {16, 12}, {24, 7},  {31, 15}, {38, 6},
    {45, 3},  {52, 2},  {58, 4},  {65, 2},  {71, 3},  {78, 4},
    {85, 7},  {92, 14}, {100, 8}, {108, 12},{117, 9},
};
static const int TOWER_N = sizeof(TOWERS) / sizeof(TOWERS[0]);

// Blocks are outlines with the interior knocked out, not solid fills: at this
// size a solid block is just a dark stub, and the outline is what gives the
// skyline its neon-sign read. `grow` 0..1 raises them out of the horizon as the
// world builds, so they do not pop into an established scene.
//
// `z` is the dolly: everything scales about the vanishing point, so as the city
// closes in the towers spread outward and walk off both edges. Two things stay
// nailed down while they do, and both are correct rather than convenient — the
// sun and the horizon are effectively at infinity, so approaching does not move
// them. What is a cheat is the bases: a real approach drops the near blocks'
// feet below the horizon line, and there are only 20 rows of sky to lose them
// into, so they stay welded to it and only grow.
//
// The centre of the table clears out on its own as z rises, which is the reason
// the sun survives the run: x45..86 all scale *away* from x64.
static void drawSkyline(U8G2& g, float grow, float z) {
  for (int i = 0; i < TOWER_N; i++) {
    int bx = TOWERS[i][0];
    int nx = (i + 1 < TOWER_N) ? TOWERS[i + 1][0] : ui::W;
    int bw = nx - bx - 1;
    if (bw < 4) bw = 4;

    int x = SUN_CX + iround((bx - SUN_CX) * z);
    int w = iround(bw * z);
    if (w < 1) w = 1;
    if (x >= ui::W || x + w <= 0) continue;  // dollied off the edge entirely

    int h = iround(TOWERS[i][1] * grow * z);
    if (h < 1) h = 1;
    if (h > HORIZON) h = HORIZON;  // never draw above the top of the panel
    int top = HORIZON - h;

    g.setDrawColor(0);
    clipBox(g, x, top, w, h);  // knock a hole in the sun/sky behind it
    g.setDrawColor(1);
    clipBox(g, x, top, w, 1);           // roof
    clipBox(g, x, top, 1, h);           // left wall
    clipBox(g, x + w - 1, top, 1, h);   // right wall

    // Lit windows, only once a block is tall enough to hold a row of them clear
    // of its own roof and of the horizon line. The spacing scales with the dolly
    // too — held at 3px, a near tower fills with dots and reads as texture
    // rather than as windows getting closer.
    int step = iround(3 * z);
    if (step < 3) step = 3;
    if (h > iround(7 * z))
      for (int wy = top + step; wy < HORIZON - 2; wy += step)
        for (int wx = x + 2; wx < x + w - 2; wx += step)
          if (wx >= 0 && wx < ui::W && wy >= 0) g.drawPixel(wx, wy);
  }
}

// ---------------------------------------------------------------- the floor
// `reveal` wipes the grid forward from the horizon during the intro. `phase` is
// how many rows the floor has scrolled — see gridPhase(), which is what makes
// the surge possible without keeping frame-to-frame state.
static void drawGrid(U8G2& g, float phase, float reveal) {
  const int far  = HORIZON + 1;
  const int near = ui::H - 1;
  int maxY = HORIZON + (int)(reveal * (near - HORIZON));
  if (maxY < far) return;

  const int depth = near - HORIZON;

  // Lanes converge on the vanishing point. A ground-plane vertical projects to
  // a straight line, so linear interpolation in screen space is exact — but it
  // is stepped per row rather than handed to drawLine, because the outer lanes
  // run off-panel and u8g2 coordinates are unsigned (a negative x wraps to
  // ~65k). The outer lanes move more than a pixel per row, so consecutive
  // samples are joined with a short run; plotting single pixels left them
  // visibly dotted.
  for (int k = -LANE_N; k <= LANE_N; k++) {
    int prevX = SUN_CX;
    for (int y = far; y <= maxY; y++) {
      int x = SUN_CX + (k * LANE_SPREAD * (y - HORIZON)) / depth;
      int a = prevX < x ? prevX : x;
      int b = prevX < x ? x : prevX;
      if (a < 0) a = 0;
      if (b > ui::W - 1) b = ui::W - 1;
      if (a <= b) g.drawHLine(a, y, b - a + 1);
      prevX = x;
    }
  }

  // Rows scroll toward the viewer: as `frac` grows so does every u, sliding
  // each row down and off the bottom edge. When frac wraps, row i+1 inherits
  // row i's position exactly, which is what makes the loop seamless.
  // Walked near-to-far (i descending), because both guards below are "and every
  // remaining row is worse" tests — run the other way they trip on the first
  // row at the horizon and drop the whole floor.
  float frac  = fmodf(phase, 1.0f);
  int   prevY = ui::H + 8;
  for (int i = GRID_ROWS; i >= 1; i--) {
    float u = (i - 1 + frac) / GRID_ROWS;
    int   y = HORIZON + (int)(depth * powf(u, GRID_POW) + 0.5f);
    if (y > maxY) continue;       // not revealed yet, or off the bottom edge
    if (y <= HORIZON + 1) break;  // hugging the horizon line; nothing legible
    if (prevY - y < 2) break;     // would merge into fill — see GRID_POW above
    prevY = y;
    g.drawHLine(0, y, ui::W);
  }
}

// ---------------------------------------------------------------- the mark
// 原 — "hara", the first character of the name — as a 32x32 bitmap in the
// current draw colour. Drawn against a reference glyph rather than from memory,
// and the 厂 radical is the tell that it was: its downstroke is not vertical, it
// sweeps left as it falls and its foot ends BELOW everything else in the
// character. Get that wrong and it reads as a box with a lid.
//
// Stroke weights are what survive 1 bit at this size: 3px on 厂, a 2px box for
// 白, 2px on the three strokes of 小. Below 24px those all merge into a blob, so
// 32px is the floor and there is no smaller version of this scene.
static void hara(U8G2& g, int x, int y) {
  clipBox(g, x + 3, y + 1, 28, 3);  // 厂 top bar, running past the box below it

  // The downstroke's leftward sweep. Cubic, not linear: it has to leave the top
  // bar vertically and only fall away near the foot, which is what reads as a
  // brush finishing its stroke. Over 31 rows that is only two pixels of drift —
  // 19 rows at dx3, 9 at dx2, 3 at dx1 — but a straight line here looks printed.
  //
  // Integer round of 2*(i/30)^3, which matches the reference render exactly for
  // all 31 rows (checked, not assumed). Written out rather than tabled because
  // the curve is the point and 31 bytes of literals would hide it.
  for (int i = 0; i < 31; i++) {
    int dx = 3 - (2 * i * i * i + 13500) / 27000;  // 27000 = 30^3
    clipBox(g, x + dx, y + 1 + i, 3, 1);
  }

  clipBox(g, x + 15, y + 5, 4, 2);   // 丿 nub on top of 白
  clipBox(g, x + 10, y + 7, 18, 2);  // 白 top
  clipBox(g, x + 10, y + 12, 18, 2); // 白 middle
  clipBox(g, x + 10, y + 18, 18, 2); // 白 bottom
  clipBox(g, x + 10, y + 7, 2, 13);  // 白 left
  clipBox(g, x + 26, y + 7, 2, 13);  // 白 right
  clipBox(g, x + 18, y + 23, 3, 8);  // 小 centre post
  clipBox(g, x + 16, y + 29, 3, 2);  // ...and its hook to the left

  // 小's flanking strokes. Never clipped — y+23 is the highest row they touch
  // and the mark never rises far enough for that to go negative.
  g.drawLine(x + 12, y + 23, x + 9, y + 31);
  g.drawLine(x + 13, y + 23, x + 10, y + 31);
  g.drawLine(x + 25, y + 23, x + 28, y + 31);
  g.drawLine(x + 26, y + 23, x + 29, y + 31);
}

// The halo is a dilation: every pixel within MARK_HALO of an inked one goes
// dark, then the mark is laid over the top. Done as 24 offset passes in colour 0
// rather than a scratch bitmap, which is the same trick ui::haloStr uses for
// text — it costs ~1100 small draws a frame and no RAM.
static void drawMark(U8G2& g, float p) {
  float e = 1.0f - (1.0f - p) * (1.0f - p);  // ease out
  int   y = MARK_Y + (int)((1.0f - e) * MARK_DROP + 0.5f);

  g.setDrawColor(0);
  for (int dy = -MARK_HALO; dy <= MARK_HALO; dy++)
    for (int dx = -MARK_HALO; dx <= MARK_HALO; dx++)
      if (dx || dy) hara(g, MARK_X + dx, y + dy);
  g.setDrawColor(1);
  hara(g, MARK_X, y);
}

// ---------------------------------------------------------------- strap line
// A lower-third plate: the grid is knocked out behind the text so it reads
// cleanly, but only as wide as the text — the lanes still run to the bottom edge
// either side, which keeps the near field (the most dramatic part of the floor)
// intact and makes the plate look like it is in front of the scene.
static void drawKickerPlate(U8G2& g, int w) {
  int x0 = (ui::W - w) / 2 - 3;
  if (x0 < 0) x0 = 0;
  int bw = w + 6;
  if (x0 + bw > ui::W) bw = ui::W - x0;
  g.setDrawColor(0);
  g.drawBox(x0, KICK_Y - 7, bw, 9);
  g.setDrawColor(1);
}

static void drawKicker(U8G2& g, uint32_t el) {
  g.setFont(u8g2_font_4x6_tf);

  if (el < INTRO_END) {
    if (el < T_KICK) return;
    const char* s = brand::KICKERS[0];
    int reveal = (int)(ramp(el, T_KICK, D_KICK) * strlen(s) + 0.5f);
    int tr = ui::trackedWidth(g, s, 1) <= ui::W ? 1 : 0;
    // Plate is sized to the finished string, so it does not grow with the
    // reveal — the text wipes on inside a plate that is already there.
    drawKickerPlate(g, ui::trackedWidth(g, s, tr));
    ui::trackedStr(g, KICK_Y, s, tr, reveal);
    return;
  }

  // Attract loop: the strap lines cycle, each arriving through a burst of tape
  // static. That hiss is the cassette half of the brief, and it costs one band
  // of noise — the seed only advances ~30 times a second on purpose, because
  // reseeding every frame averages out to flat grey.
  // The `since >= KICK_HOLD` guard matters: without it the very first attract
  // frame hisses over the strap line the intro just finished revealing, which
  // reads as a glitch rather than a transition.
  uint32_t since = el - INTRO_END;
  uint32_t into  = since % KICK_HOLD;
  const char* s  = brand::KICKERS[(since / KICK_HOLD) % brand::KICKER_N];
  int tr = ui::trackedWidth(g, s, 1) <= ui::W ? 1 : 0;
  drawKickerPlate(g, ui::trackedWidth(g, s, tr));

  if (since >= KICK_HOLD && into < KICK_HISS) {
    // Literal 6, not ui::CH: this band sizes to the kicker's own 4x6 font
    // row height. ui::CH moved to 8 on 2026-09-16 when boot.cpp's console
    // grid switched fonts to match the message screen — the kicker here
    // kept its original font, so this didn't move with it.
    ui::staticBand(g, KICK_Y - 6, 6, el / 33);
    return;
  }
  ui::trackedStr(g, KICK_Y, s, tr);
}

// ---------------------------------------------------------------- the theme
// A monophonic synthwave arpeggio on the piezo, timed to the same beats the
// visuals hit — the "retro cassette" half of the brief. The visuals alone
// were already outrun/synthwave (the sun, the grid, the neon skyline);
// brand.h's "Cassette Bus" joke and KICK_HISS's tape-static burst above
// already lean the same way, and this is what makes the intro read as a
// tape playing, not just a screen lighting up.
//
// One passive piezo holds one note at a time, so this is a riff, not a
// chord progression: a slow rising phrase while the world assembles, a
// driving arpeggio through the approach that quickens toward the slam (the
// same acceleration gridPhase() gives the floor, just heard instead of
// seen), a hit on the slam, and two resolving notes as the mark and kicker
// land. ThemeStage/themeStage/themeNextMs/themeBeat are declared up near
// startMs/started, not here - see the comment there.

// A minor pentatonic — the classic driving-synthwave set. Nothing in it
// clashes regardless of what order it plays in, so the pattern below can
// just walk it without needing real harmony.
static const uint16_t SCALE[]  = {220, 262, 294, 330, 392, 440, 523, 587};
static const int      SCALE_N  = sizeof(SCALE) / sizeof(SCALE[0]);

// ms until the next approach beat: ramps from a loping 220ms down to a
// driving 90ms by the slam, so the ear speeds up together with gridPhase()'s
// own acceleration instead of just the eye.
static uint32_t approachBeatMs(uint32_t at) {
  float p = ramp(at, T_APPROACH, D_APPROACH);
  return 220 - (uint32_t)(p * 130);
}

void playTheme(uint8_t piezoPin) {
  uint32_t el = elapsed();
  while (themeStage != STAGE_DONE && themeNextMs <= el) {
    switch (themeStage) {
      case STAGE_BUILD:  // the world assembling: a slow rising phrase
        tone(piezoPin, SCALE[themeBeat % 4], 90);
        themeBeat++;
        themeNextMs += 220;
        if (themeNextMs >= T_APPROACH) themeStage = STAGE_APPROACH;
        break;

      case STAGE_APPROACH: {  // the run: quickens toward the slam
        tone(piezoPin, SCALE[themeBeat % SCALE_N], 60);
        themeBeat++;
        uint32_t at = themeNextMs;
        themeNextMs = at + approachBeatMs(at);
        if (themeNextMs >= T_FLASH) {
          themeNextMs = T_FLASH;
          themeStage  = STAGE_SLAM;
        }
        break;
      }

      case STAGE_SLAM:  // the full-frame XOR hit
        tone(piezoPin, SCALE[SCALE_N - 1], 90);
        themeNextMs = T_MARK;
        themeStage  = STAGE_MARK;
        break;

      case STAGE_MARK:  // 原 lands
        tone(piezoPin, SCALE[2], 200);
        themeNextMs = T_KICK;
        themeStage  = STAGE_KICK;
        break;

      case STAGE_KICK:  // the strap line reveals — resolve and stop
        tone(piezoPin, SCALE[0], 250);
        themeStage = STAGE_DONE;
        break;

      case STAGE_DONE:
        break;
    }
  }
}

// ---------------------------------------------------------------- frame
void draw(U8G2& g) {
  uint32_t el = elapsed();
  float    t  = el / 1000.0f;
  g.clearBuffer();

  // Sun first: it clears everything below the horizon on its way out, and the
  // skyline then knocks its own blocks out of the disc — so both the grid and
  // the horizon line have to be drawn after the pair of them.
  float sunP = ramp(el, T_SUN, D_SUN);
  if (sunP > 0.0f) drawSun(g, HORIZON + (int)((1.0f - sunP) * SUN_R));

  float skyP = ramp(el, T_SKY, D_SKY);
  if (skyP > 0.0f) drawSkyline(g, skyP, skyZoom(el));

  // Horizon wipes out from the centre.
  float hp = ramp(el, T_HORIZON, D_HORIZON);
  int   half = (int)(hp * (ui::W / 2));
  if (half > 0) g.drawHLine(SUN_CX - half, HORIZON, half * 2);

  drawGrid(g, gridPhase(t), ramp(el, T_GRID, D_GRID));

  if (el >= T_MARK) drawMark(g, ramp(el, T_MARK, D_MARK));
  drawKicker(g, el);

  // The slam: one full-frame XOR as the mark lands.
  if (el >= T_FLASH && el < T_FLASH + D_FLASH) ui::scanBar(g, 0, ui::H);

  // Scan sweeps, on two different clocks and deliberately absent in between.
  //
  //   approach  fast bar, anchored on T_APPROACH so the run opens on a sweep
  //   arrival   nothing, from the slam to the end of the intro — the mark
  //             landing and the strap wiping on are the only events that get
  //             the tube to themselves, and a bar crossing either of them
  //             reads as a glitch rather than as a CRT
  //   attract   the original 3.5s drift, on absolute time
  if (el >= T_APPROACH && el < T_FLASH) {
    float cyc = fmodf((el - T_APPROACH) / 1000.0f, SCAN_RUSH_CYCLE);
    if (cyc < SCAN_RUSH_TRAVEL)
      ui::scanBar(g, (int)(cyc / SCAN_RUSH_TRAVEL * (ui::H + 6)) - 6, 3);
  } else if (el >= INTRO_END) {
    float cyc = fmodf(t, SCAN_CYCLE);
    if (cyc < SCAN_TRAVEL)
      ui::scanBar(g, (int)(cyc / SCAN_TRAVEL * (ui::H + 6)) - 6, 3);
  }

  g.sendBuffer();
}

}  // namespace splash
