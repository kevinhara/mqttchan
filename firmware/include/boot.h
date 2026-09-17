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

// Live device/config values shown alongside the fake hardware self-test
// lines, so the screen someone actually watches at power-on says something
// about *this* device instead of only a period bit. Copied into internal
// buffers at begin() time, so callers don't need to keep the sources (e.g. a
// DeviceSettings) alive past that call. A null or empty field prints as
// "(not set)" rather than being skipped, so the row count - and the timing
// script beneath it - never changes with configuration state.
struct Info {
  const char* name;   // device name; DeviceSettings always has one
  const char* ssid;   // WiFi SSID
  const char* host;   // MQTT broker host
  uint16_t    port;   // MQTT broker port; ignored if host is empty
  const char* topic;  // MQTT topic
};

// Start (or restart) the sequence from its first frame.
void begin(const Info& info);

// True once the last line has printed and the hand-off cursor has blinked out.
bool done();

// Render one frame and send it.
void draw(U8G2& g);

// Total run length in ms, for logs and for tuning.
uint32_t durationMs();

}  // namespace boot
