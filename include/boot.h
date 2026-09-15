// Power-on self test, as a 1988 286 would have run it: a cold CRT striking,
// a memory count, then a column of dot-leader checks scrolling off the top of
// a 32x10 console, and finally a blinking cursor as the loader takes over.
//
// Runs once at startup, ahead of the splash. Nothing here touches the network
// or the clock — it is pure theatre, and pure function of elapsed time.
#pragma once
#include <U8g2lib.h>
#include <stdint.h>

namespace boot {

// Start (or restart) the sequence from its first frame.
void begin();

// True once the last line has printed and the hand-off cursor has blinked out.
bool done();

// Render one frame and send it.
void draw(U8G2& g);

// Total run length in ms, for logs and for tuning.
uint32_t durationMs();

}  // namespace boot
