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

  // Starts the SNTP sync using tz (a POSIX TZ string).
  //
  // Correction, 2026-09-16: this used to say "safe to call before WiFi
  // connects - the ESP32 core's SNTP client just waits for a network and
  // syncs once one shows up," and main.cpp's setup() called this early on
  // that basis. It's syntactically safe (doesn't crash or hang on its own)
  // but not consequence-free: an intermittent
  // "assert failed: udp_new_ip_type ... Required to lock TCPIP core
  // functionality!" crash during MQTT's hostname DNS lookup was traced (via
  // xtensa-esp32-elf-addr2line against a live crash) to SNTP's own pending
  // DNS retry for its NTP server firing reentrantly, mid-lookup, off the
  // *other* DNS lookup MQTT's PubSubClient::connect() does for its hostname
  // - lwIP's DNS cache management can invoke a pending callback (here,
  // SNTP's) as a side effect of resolving an unrelated name, and that
  // nested call lands outside the locking the outer call expected. This
  // only happens if an earlier SNTP attempt is still pending/retrying when
  // that second DNS lookup runs - which an early, pre-WiFi begin() call all
  // but guarantees, since that first attempt has nothing to resolve against
  // yet. main.cpp's setup() no longer calls this before WiFi is up; the
  // first real call now happens in appTask, after MQTT is connected. The
  // old comment was believable because "safe" was true in the narrow sense
  // of "won't itself crash" - it just didn't account for what calling it
  // early does to code elsewhere that also does DNS.
  //
  // Verified live 2026-09-16: the crash reproduced 1 time out of 3
  // DTR/RTS-reset trials right after this fix was written (before the
  // ordering fix below was in place, i.e. the baseline), consistent with
  // the ~2/7 rate seen earlier the same session. After moving the first
  // real call to appTask (main.cpp, after mqttLink.begin() rather than
  // before WiFi is even up), 0 crashes across 16 trials in two back-to-back
  // batches, 15/16 reaching "MQTT: connected" cleanly (1 inconclusive,
  // timed out waiting rather than crashing). Not a mathematical proof for
  // an intermittent bug, but a strong result against that baseline.
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

  // Burn-in mitigation: cycles the clock's drawn position through this table
  // once per minute (see minutesSinceMidnight below) rather than pinning it
  // dead-centre for as long as the device stays powered on. Kept tight on X:
  // "HH:MM" at size 4 is 120px wide on a 128px panel (see the size-4 comment
  // below), leaving only 4px of slack per side, so a ±3 swing is as far as
  // it can move without clipping. Y has far more room — size-4 text is 32px
  // tall on a 64px panel, 16px slack per side — so it does most of the work.
  static constexpr int8_t kOffsets[][2] = {
      {0, 0},  {3, -10}, {-3, 10}, {3, 10}, {-3, -10},
      {0, -10}, {0, 10}, {3, 0},  {-3, 0},
  };
  static constexpr size_t kOffsetCount =
      sizeof(kOffsets) / sizeof(kOffsets[0]);

  void draw(time_t now) {
    struct tm local;
    // configTzTime() resets the clock to the epoch until SNTP's first sync
    // lands; localtime_r "succeeding" on that just yields 1970. Anything
    // before this file was written can only mean "not synced yet".
    bool synced =
        localtime_r(&now, &local) != nullptr && local.tm_year >= (2024 - 1900);

    if (!synced) {
      // Correction, 2026-09-16: this used to fall back to a "--:--"
      // placeholder while unsynced (see git history), which read as a
      // stuck/broken clock rather than a device still booting. Then changed
      // to a blank screen, then to redrawing "Fetching data" itself here so
      // the panel never goes blank for the rest of a long unsynced wait
      // (MQTT retries, a slow broker, NTP taking its time). That redraw was
      // its own bug: main.cpp's appTask already types that same "Fetching
      // data" onto the bubble, in its bordered, word-wrapped box, right
      // before idleClock starts ticking - this fillScreen+drawString ran on
      // idleClock's very next tick (within ~50ms) and stomped that box with
      // an unboxed, dead-centered copy, which read as a jarring flash into a
      // larger, cut-off-looking font even though the point size hadn't
      // actually changed. Since the text is identical either way, simplest
      // fix is to just leave the panel alone while unsynced - the bubble's
      // own text is already correct and stays up untouched for as long as
      // sync takes.
      return;
    }

    // A pure function of the current hour/minute, not separately tracked
    // state — it only changes when the minute does, since local.tm_min only
    // changes once a minute, and tick() already guards against redrawing
    // more than once a second.
    int minutesSinceMidnight = local.tm_hour * 60 + local.tm_min;
    int8_t dx = kOffsets[minutesSinceMidnight % kOffsetCount][0];
    int8_t dy = kOffsets[minutesSinceMidnight % kOffsetCount][1];

    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &local);
    // Blink the colon at 1Hz rather than leaving it solid — draw() only
    // runs once a second (see tick()), so every call is one blink
    // half-cycle.
    colonOn_ = !colonOn_;
    if (!colonOn_) buf[2] = ' ';

    display_->startWrite();
    display_->fillScreen(TFT_BLACK);
    display_->setTextColor(TFT_WHITE, TFT_BLACK);
    // Size 4 is as large as "HH:MM" fits on a 128px-wide panel (5 chars *
    // 6px * 4 = 120px) — any bigger and it clips off the sides.
    display_->setTextSize(4);
    display_->setTextDatum(textdatum_t::middle_center);
    display_->drawString(buf, display_->width() / 2 + dx,
                          display_->height() / 2 + dy);
    display_->endWrite();
  }

  M5GFX *display_;
  time_t lastDrawn_ = -1;
  bool colonOn_ = true;
};
