// Brand splash: late-80s synthwave, rendered in 1 bit.
//
// A slit sun half-sunk on the horizon, a flat city skyline along it, a
// perspective grid scrolling toward the viewer, and 原 landing over the top of
// it. The intro plays once (~4.8s) in two movements: the world builds in the
// first second, then three seconds of running at the city — towers dollying
// outward, floor surging, scan bars whipping — before the slam. After that it
// settles into an attract loop: grid scrolling, a CRT scan bar sweeping through,
// and the strap line cycling in through a burst of tape static.
//
// Palms and an "HLI" wordmark stood where the skyline and 原 are now, until the
// panel reference sheet replaced both (design/, sheet 01). Nothing about the
// interface changed with them.
#pragma once
#include <U8g2lib.h>
#include <stdint.h>

namespace splash {

// Start (or restart) from the first frame of the intro.
void begin();

// True while the one-shot intro is still playing (false = attract loop).
bool intro();

// ms since begin(), for retiming rigs and logs.
uint32_t elapsed();

// Render one frame and send it.
void draw(U8G2& g);

// Fires whatever notes of the intro's synthwave arpeggio have come due since
// the last call - the "retro cassette" half of the brief, on a passive piezo
// (tone() is non-blocking/LEDC-driven, same as SpeechBubble's per-character
// beep - see speech_bubble.h). Call every frame alongside draw(), from
// begin() through the end of the intro; a no-op before begin() and after the
// intro finishes.
void playTheme(uint8_t piezoPin);

}  // namespace splash
