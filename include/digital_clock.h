// Digital HH:MM clock, drawn on the same panel SpeechBubble uses for
// messages. This is that panel's default/idle face — MqttLink switches back
// to it once a message has finished displaying (see handleMessage()). Large
// text (setTextSize(4) is as big as "HH:MM" fits on a 128px-wide panel) with
// a colon that blinks once a second, both in service of cross-room
// readability — the previous HH:MM:SS at size 2 was legible only up close.
#pragma once

#include <Arduino.h>
#include <M5GFX.h>
#include <string.h>
#include <time.h>

class DigitalClock {
 public:
  // First-boot default if no timezone has been configured yet (see
  // DeviceSettings::load()) - Pacific/Auckland's full POSIX TZ string (with
  // US-style DST rule dates that happen to match NZ's own) rather than a bare
  // UTC offset, so the clock steps NZDT<->NZST across the year with no
  // firmware update. Public so main.cpp can hand it to DeviceSettings::load()
  // as the same single source of truth used here.
  static constexpr const char *kDefaultTz = "NZST-12NZDT,M9.5.0,M4.1.0/3";

  explicit DigitalClock(M5GFX *display) : display_(display) {}

  // Starts the SNTP sync using tz (a POSIX TZ string). Safe to call before
  // WiFi connects — the ESP32 core's SNTP client just waits for a network and
  // syncs once one shows up.
  void begin(const String &tz = kDefaultTz) { setTimezone(tz); }

  // (Re)applies a POSIX TZ string, e.g. after a BLE config write changes it —
  // takes effect on the next tick()/showNow() redraw, no reboot needed.
  void setTimezone(const String &tz) {
    configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
  }

  // Redraws only when the displayed second has changed, so appTask can call
  // this on every ~50ms tick without hammering the I2C bus.
  void tick() {
    time_t now = time(nullptr);
    if (now == lastDrawn_) return;
    lastDrawn_ = now;
    draw(now);
  }

  // Forces an immediate redraw, bypassing the "only on second change" guard
  // above — used right after a message clears so the clock reappears at once
  // instead of waiting up to a second for the next tick().
  void showNow() {
    lastDrawn_ = static_cast<time_t>(-1);
    tick();
  }

 private:
  // NZST-12NZDT,M9.5.0,M4.1.0/3: NZST is 12h ahead of UTC, NZDT (1h further)
  // runs from the last Sunday in September to the first Sunday in April.
  static constexpr const char *kTz = "NZST-12NZDT,M9.5.0,M4.1.0/3";

  void draw(time_t now) {
    struct tm local;
    char buf[8];
    // configTzTime() resets the clock to the epoch until SNTP's first sync
    // lands; localtime_r "succeeding" on that just yields 1970. Anything
    // before this file was written can only mean "not synced yet".
    if (localtime_r(&now, &local) != nullptr && local.tm_year >= (2024 - 1900)) {
      strftime(buf, sizeof(buf), "%H:%M", &local);
    } else {
      strncpy(buf, "--:--", sizeof(buf));
    }

    // Blink the colon at 1Hz rather than leaving it solid — draw() only
    // runs once a second (see tick()), so every call is one blink half-cycle.
    colonOn_ = !colonOn_;
    if (!colonOn_) buf[2] = ' ';

    display_->startWrite();
    display_->fillScreen(TFT_BLACK);
    display_->setTextColor(TFT_WHITE, TFT_BLACK);
    // Size 4 is as large as "HH:MM" fits on a 128px-wide panel (5 chars *
    // 6px * 4 = 120px) — any bigger and it clips off the sides.
    display_->setTextSize(4);
    display_->setTextDatum(textdatum_t::middle_center);
    display_->drawString(buf, display_->width() / 2, display_->height() / 2);
    display_->endWrite();
  }

  M5GFX *display_;
  time_t lastDrawn_ = -1;
  bool colonOn_ = true;
};
