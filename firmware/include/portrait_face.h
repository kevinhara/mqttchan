// Renders the "announcer" on the face panel as a random portrait from the
// bundled 1-bit dialogue portraits pack (see portraits_data.h) instead of
// m5stack-avatar's procedural eyes/mouth, which this replaces outright -
// see main.cpp/mqtt_link.h for the rest of the swap.
//
// Idle, it holds the character's first frame and sways it a couple of
// pixels in a slow "breathing" bob (tick()), punctuated every few seconds by
// a quick blink (startBlink()): a flash to portraits::Portrait::blink, a
// frame synthesized for every character by firmware/tools/gen_portraits.py
// (_SYNTHETIC_BLINKS) - not one of the pack's own frames. While a message is
// being typed, MqttLink drives it through the character's own frame set
// instead (advanceTalkFrame()) - most characters ship 2-4 frames (a mouth
// flap), so this is a real per-character animation, not the sine-noise
// mouth-ratio placeholder the m5avatar version faked. MqttLink also calls
// pickRandom() once an announcement is fully done, so the announcer looks
// like someone different next time - see revertToIdle().
//
// Correction, 2026-09-19: idle blinking used to flash to the character's own
// frame 1 - reusing the talk-cycle's own frames on the theory that they were
// close enough to a blink. Wrong on a live board: frame 1 is a mouth-open
// frame for a large fraction of characters, so idle was visibly opening its
// mouth rather than blinking. blink is now always the synthesized frame,
// kept separate from frames/frameCount so it's never talk-cycled either.
#pragma once

#include <M5GFX.h>
#include <math.h>

#include "portraits_data.h"

class PortraitFace {
 public:
  explicit PortraitFace(M5GFX *display) : display_(display) { pickRandom(); }

  // Idle "breathing" sway plus the occasional blink (see startBlink()) -
  // call every tick (~50ms is plenty) from appTask's own loop while nothing
  // else owns the panel. No-op while talking() (advanceTalkFrame() is
  // driving frames instead) or asleep (settled on the still resting frame -
  // see setSleeping()), and only actually redraws when something changed,
  // same throttling idea as MqttLink::applyRestingExpression() uses for the
  // old avatar_->setExpression() calls this replaces.
  void tick(uint32_t nowMs) {
    if (talking_ || sleeping_) return;
    if (blinking_) {
      // Hold whatever startBlink() drew until its hold time is up - the
      // breathing sway below is skipped for that beat so the blink frame
      // isn't immediately redrawn over with the resting one.
      if (nowMs - blinkStartMs_ < blinkHoldMs_) return;
      blinking_ = false;
      // Force the sway below to redraw, even if the bob phase lands back on
      // the same offset it was already showing.
      lastOffset_ = kNoOffset;
      scheduleNextBlink(nowMs);
    } else if (nowMs >= nextBlinkMs_) {
      startBlink(nowMs);
      return;
    }
    float phase =
        (nowMs % kBreathPeriodMs) / float(kBreathPeriodMs) * 2.0f * (float)M_PI;
    int offset = static_cast<int>(lroundf(sinf(phase) * kBreathAmplitudePx));
    if (offset == lastOffset_) return;
    lastOffset_ = offset;
    drawFrame(0, offset);
  }

  // Called by MqttLink right as a message starts typing.
  void startTalking() {
    talking_ = true;
    blinking_ = false;
    frame_ = 0;
    drawFrame(frame_, 0);
  }

  // Cycles to this character's next frame - called from MqttLink's lip-sync
  // task at a steady interval for as long as a message is typing. Frame
  // count varies per character (see portraits_data.h), so this just wraps
  // at whatever this one has.
  void advanceTalkFrame() {
    if (!talking_) return;
    frame_ = (frame_ + 1) % portraits::kPortraits[index_].frameCount;
    drawFrame(frame_, 0);
  }

  // Called once a message is done typing - settles back on frame 0. Idle
  // breathing resumes on tick()'s next call; lastOffset_ is reset so that
  // call redraws even if the bob phase happens to land back on the same
  // offset it was already showing.
  void stopTalking() {
    talking_ = false;
    frame_ = 0;
    lastOffset_ = kNoOffset;
    scheduleNextBlink(millis());
    drawFrame(0, 0);
  }

  // Swaps in a new random character, distinct from whoever's on screen now.
  // MqttLink calls this from revertToIdle(), once an announcement (typing +
  // hold, or a dismiss mid-typing) is fully done.
  void pickRandom() {
    if (portraits::kPortraitCount > 1) {
      size_t next;
      do {
        next = static_cast<size_t>(random(0, (long)portraits::kPortraitCount));
      } while (next == index_);
      index_ = next;
    }
    frame_ = 0;
    lastOffset_ = kNoOffset;
    blinking_ = false;
    scheduleNextBlink(millis());
    drawFrame(0, 0);
  }

  // Replaces the old Sleepy/Neutral m5avatar expression toggle
  // (applyRestingExpression() in mqtt_link.h): this asset pack has no
  // separate sleeping frame, so overnight just means "hold still" - freeze
  // the breathing sway rather than draw anything different.
  void setSleeping(bool sleeping) {
    if (sleeping == sleeping_) return;
    sleeping_ = sleeping;
    if (sleeping_) {
      blinking_ = false;
      drawFrame(0, 0);
    } else {
      scheduleNextBlink(millis());
    }
  }

 private:
  // Slow enough to read as breathing, not twitching; ±2px is enough motion
  // to notice on a 64px-tall panel without the sway looking like jitter.
  static constexpr uint32_t kBreathPeriodMs = 2600;
  static constexpr int kBreathAmplitudePx = 2;
  // Off the range tick()'s offset can ever produce, so stopTalking()/
  // pickRandom() always force a redraw on the next tick() rather than
  // silently matching whatever offset was last drawn.
  static constexpr int kNoOffset = 1000;

  // How long a blink holds before tick() lets the breathing sway resume.
  static constexpr uint32_t kBlinkHoldMs = 150;
  // Randomized per blink (see scheduleNextBlink()) so every character on
  // screen doesn't blink in lockstep on a fixed metronome.
  static constexpr uint32_t kBlinkIntervalMinMs = 2500;
  static constexpr uint32_t kBlinkIntervalJitterMs = 3000;

  // Picks the next blink at a randomized interval from nowMs - called
  // whenever idle sway resumes (stopTalking(), pickRandom(), waking from
  // setSleeping(true), and the ctor via pickRandom()) and again right after
  // each blink ends, so the cadence keeps going for as long as the character
  // sits idle.
  void scheduleNextBlink(uint32_t nowMs) {
    nextBlinkMs_ = nowMs + kBlinkIntervalMinMs +
                   static_cast<uint32_t>(random(0, (long)kBlinkIntervalJitterMs));
  }

  // Flashes to this character's synthesized blink frame and back - see the
  // file header for why this is never one of the pack's own talk-cycle
  // frames. tick() holds it for kBlinkHoldMs before resuming the breathing
  // sway.
  void startBlink(uint32_t nowMs) {
    blinking_ = true;
    blinkStartMs_ = nowMs;
    blinkHoldMs_ = kBlinkHoldMs;
    draw(portraits::kPortraits[index_].blink,
         (lastOffset_ == kNoOffset) ? 0 : lastOffset_);
  }

  // Draws this character's talk-cycle frame `frameIndex` (portraits_data.h's
  // frames/frameCount) - the resting frame (0) for idle/breathing, or
  // whichever one advanceTalkFrame() is currently on.
  void drawFrame(uint8_t frameIndex, int yOffset) {
    draw(portraits::kPortraits[index_].frames[frameIndex], yOffset);
  }

  void draw(const uint8_t *bitmap, int yOffset) {
    const int x = (128 - portraits::kWidth) / 2;
    display_->startWrite();
    display_->fillScreen(TFT_BLACK);
    display_->drawBitmap(x, yOffset, bitmap, portraits::kWidth,
                          portraits::kHeight, TFT_WHITE);
    display_->display();
    display_->endWrite();
  }

  M5GFX *display_;
  size_t index_ = 0;
  uint8_t frame_ = 0;
  int lastOffset_ = kNoOffset;
  volatile bool talking_ = false;
  bool sleeping_ = false;
  bool blinking_ = false;
  uint32_t blinkStartMs_ = 0;
  uint32_t blinkHoldMs_ = 0;
  uint32_t nextBlinkMs_ = 0;
};
