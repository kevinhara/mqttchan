// Drives the 4-pin RGB status LED (see README's "RGB status LED" section and
// the RGB_R_PIN/RGB_G_PIN/RGB_B_PIN defines in main.cpp) from whatever a
// displayed message asks for. MqttLink calls start() right as a message
// starts showing on the bubble panel and stop() once it leaves the screen —
// see mqtt_link.h's display() — so the LED tracks the message's own on-screen
// lifetime rather than running on a fixed timer of its own.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Mirrors the "led" string a message payload can carry (see
// MqttLink::parseMessage()'s ledColorFromString()). None means the message
// didn't ask for a color, so the LED stays off for it.
enum class LedColor { None, Red, Green, Blue, Cycle };

class RgbLed {
 public:
  RgbLed(uint8_t rPin, uint8_t gPin, uint8_t bPin, bool commonAnode = false)
      : rPin_(rPin), gPin_(gPin), bPin_(bPin), commonAnode_(commonAnode) {}

  void begin() {
    ledcAttach(rPin_, kPwmFreq, kPwmRes);
    ledcAttach(gPin_, kPwmFreq, kPwmRes);
    ledcAttach(bPin_, kPwmFreq, kPwmRes);
    write(0, 0, 0);
  }

  // Lights the LED for as long as the caller's message stays on screen.
  // Solid colors are just set directly; cycle (and/or blink) need to vary
  // over time while the caller is off blocking on SpeechBubble::show()/
  // holdWithCountdown(), so those run on their own task instead — same
  // fire-and-forget pattern as MqttLink's lipSyncTask animating the mouth
  // across the same blocking calls. color == None is just stop().
  void start(LedColor color, bool blink) {
    if (color == LedColor::None) {
      stop();
      return;
    }
    color_ = color;
    blink_ = blink;
    active_ = true;
    if (color == LedColor::Cycle || blink) {
      xTaskCreatePinnedToCore(animateTaskFn, "ledAnimate", 2048, this, 1,
                               nullptr, PRO_CPU_NUM);
    } else {
      uint8_t r, g, b;
      solidToRgb(color, r, g, b);
      write(r, g, b);
    }
  }

  // Turns the LED off. If an animate task is running it notices active_
  // went false on its own next ~20ms tick and writes off itself before
  // exiting (again, fire-and-forget, like lipSyncActive_) — the extra
  // write(0,0,0) here just covers the solid-color case, where no task is
  // running to do it.
  void stop() {
    active_ = false;
    write(0, 0, 0);
  }

 private:
  static constexpr uint32_t kPwmFreq = 5000;
  static constexpr uint8_t kPwmRes = 8;  // 8-bit duty: 0-255
  static constexpr uint32_t kAnimateStepMs = 20;
  static constexpr float kCycleDegPerStep = 2.0f;    // ~3.6s per full sweep
  static constexpr uint32_t kBlinkIntervalMs = 400;  // on/off half-period

  static void solidToRgb(LedColor color, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = g = b = 0;
    switch (color) {
      case LedColor::Red: r = 255; break;
      case LedColor::Green: g = 255; break;
      case LedColor::Blue: b = 255; break;
      default: break;  // Cycle is handled by the animate task, not here
    }
  }

  // Standard HSV(h in [0,360), s=v=1) -> RGB, same formula as the
  // RGB_LED_TEST spectrum sweep in main.cpp (kept as a separate copy here
  // since that one only exists in a standalone bench-test build).
  static void hsvToRgb(float h, uint8_t &r, uint8_t &g, uint8_t &b) {
    float c = 255.0f;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float rp = 0, gp = 0, bp = 0;
    if (h < 60) {
      rp = c; gp = x;
    } else if (h < 120) {
      rp = x; gp = c;
    } else if (h < 180) {
      gp = c; bp = x;
    } else if (h < 240) {
      gp = x; bp = c;
    } else if (h < 300) {
      rp = x; bp = c;
    } else {
      rp = c; bp = x;
    }
    r = (uint8_t)rp;
    g = (uint8_t)gp;
    b = (uint8_t)bp;
  }

  static void animateTaskFn(void *arg) {
    static_cast<RgbLed *>(arg)->animateTask();
  }

  void animateTask() {
    float hue = 0.0f;
    bool on = true;
    uint32_t lastToggle = millis();
    while (active_) {
      uint8_t r, g, b;
      if (color_ == LedColor::Cycle) {
        hsvToRgb(hue, r, g, b);
        hue += kCycleDegPerStep;
        if (hue >= 360.0f) hue -= 360.0f;
      } else {
        solidToRgb(color_, r, g, b);
      }
      if (blink_) {
        uint32_t now = millis();
        if (now - lastToggle >= kBlinkIntervalMs) {
          on = !on;
          lastToggle = now;
        }
        if (!on) r = g = b = 0;
      }
      write(r, g, b);
      vTaskDelay(pdMS_TO_TICKS(kAnimateStepMs));
    }
    write(0, 0, 0);
    vTaskDelete(nullptr);
  }

  void write(uint8_t r, uint8_t g, uint8_t b) {
    if (commonAnode_) {
      r = 255 - r;
      g = 255 - g;
      b = 255 - b;
    }
    ledcWrite(rPin_, r);
    ledcWrite(gPin_, g);
    ledcWrite(bPin_, b);
  }

  uint8_t rPin_, gPin_, bPin_;
  bool commonAnode_;
  // Read by animateTask() (a separate FreeRTOS task, started from start());
  // written from whatever thread calls start()/stop() (appTask, via
  // MqttLink::display()). Not mutex-guarded, same tolerance as
  // lipSyncActive_ in mqtt_link.h - worst case is a cosmetic race across
  // back-to-back messages, never a crash (each is a single-word write/read).
  volatile bool active_ = false;
  LedColor color_ = LedColor::None;
  bool blink_ = false;
};
