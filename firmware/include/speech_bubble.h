// Renders spoken text as a word-wrapped, bordered "speech bubble" on a
// second panel, independent of the avatar's own M5.Display. See
// ssd1306_display.h for why the avatar needs M5.Display to itself; this
// class just draws directly to whatever M5GFX device it is given.
#pragma once

#include <Arduino.h>
#include <M5GFX.h>
#include <functional>
#include <utility>
#include <vector>

class SpeechBubble {
 public:
  // onChar, if given, fires once per revealed non-space character — the hook
  // for a per-character beep. Baked in at construction rather than passed to
  // show() so every caller (demo mode's canned phrases, MQTT mode's live
  // text) gets it for free.
  explicit SpeechBubble(M5GFX *display, std::function<void()> onChar = nullptr)
      : display_(display), onChar_(std::move(onChar)) {}

  // 1 (default) is the original small GLCD size this class was built around;
  // 2 doubles both glyph dimensions ("Large" in the display settings). Takes
  // effect on the next show() - textSize_ is only read there, so a message
  // already typing out finishes at whatever size it started at.
  void setTextSize(uint8_t size) { textSize_ = size; }

  // Reveals text one character at a time, typewriter-style. Lines are
  // wrapped up front (wrapping needs the whole word to measure it, so it
  // can't be decided mid-reveal) and then typed out line by line. Runs on
  // whatever task calls it — this blocks for roughly strlen(text) *
  // charDelayMs, which is the point: it paces itself to be readable rather
  // than dumping the whole bubble in one frame. 45ms lands around a slower,
  // more deliberate pace; drop it if a phrase is long enough to feel sluggish.
  //
  // abortRequested, if given, is polled once per character (i.e. about once
  // per charDelayMs — the same cadence callers normally tick an input device
  // at) and stops the reveal early when it returns true, leaving whatever's
  // been typed so far on screen. MqttLink uses this so a button click can
  // interrupt a long message instead of waiting for it to finish.
  void show(const char *text, uint32_t charDelayMs = 45,
            std::function<bool()> abortRequested = nullptr) {
    // Reasserted here, not just at construction: DigitalClock draws to this
    // same display between messages and leaves its own (much larger)
    // setTextSize() in place, which wrapLines()'s width math below would
    // otherwise inherit and wrap/print far too large to read.
    display_->setTextColor(TFT_WHITE, TFT_BLACK);
    display_->setTextSize(textSize_);
    // Word-wrap by hand below; the built-in wrap breaks mid-word.
    display_->setTextWrap(false, false);

    display_->startWrite();
    display_->fillScreen(TFT_BLACK);
    display_->drawRoundRect(0, 0, display_->width(), display_->height(), 6,
                             TFT_WHITE);

    // Scales with textSize_ - the base GLCD cell is 8px tall, so "Large"
    // (textSize_ == 2) needs twice the row pitch or lines would overlap.
    const int lineHeight = kBaseLineHeight * textSize_;
    // Lines beyond what fits inside the border scroll the content area
    // upward one row at a time instead of typing off the bottom of the
    // screen unseen — see scrollRect below for why this only affects text,
    // not the border. maxLines is rounded down, so lineHeight always
    // divides the scroll region evenly and a scroll never leaves a sliver
    // of the previous line's pixels behind.
    const int maxLines = (display_->height() - 2 * kMargin) / lineHeight;
    const int scrollHeight = maxLines * lineHeight;
    // setScrollRect only affects scroll() below, not normal drawing/clipping,
    // so this can be set once up front without touching fillScreen/
    // drawRoundRect above or the per-character prints below.
    display_->setScrollRect(kMargin, kMargin,
                             display_->width() - 2 * kMargin, scrollHeight);

    int cursorY = kMargin;
    std::vector<String> lines = wrapLines(text);
    for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++) {
      if (lineIndex > 0) {
        if (static_cast<int>(lineIndex) < maxLines) {
          cursorY += lineHeight;
        } else {
          // Screen's full: shift everything already shown up by one row
          // rather than growing cursorY past the border. scroll() runs its
          // own nested startWrite()/endWrite(), which — same as the
          // per-character prints below — won't auto-flush to the physical
          // OLED while show()'s outer transaction is still open, hence the
          // explicit display() call.
          display_->scroll(0, -lineHeight);
          display_->display();
        }
      }
      const String &line = lines[lineIndex];
      display_->setCursor(kMargin, cursorY);
      for (size_t i = 0; i < line.length(); i++) {
        display_->print(line[i]);
        // Panel_SSD1306 is a Panel_HasBuffer: draws land in a RAM shadow
        // buffer and only reach the physical OLED when the dirty rect is
        // flushed. That normally happens on the outermost endWrite(), which
        // here is the one at the end of the whole message — so without this,
        // every character lands in the buffer at the paced delay below but
        // the screen shows nothing until the last one, and the message
        // appears to pop in all at once. display() pushes just the changed
        // columns now instead of waiting.
        display_->display();
        if (onChar_ && line[i] != ' ') onChar_();
        vTaskDelay(pdMS_TO_TICKS(charDelayMs));
        if (abortRequested && abortRequested()) {
          display_->endWrite();
          return;
        }
      }
    }
    display_->endWrite();
  }

  void clear() { display_->fillScreen(TFT_BLACK); }

  // Draws up to kMaxQueueDots small filled dots in the bottom-left corner,
  // one per message still waiting behind the one just shown - a queued-up
  // burst of MQTT messages should be visible as "more coming" rather than
  // silently backed up. No-op when count is 0, so idle/single-message
  // display looks exactly as before. Meant to be called right after show():
  // it draws on top of the just-typed text rather than clearing the screen,
  // and sits one row above holdWithCountdown()'s bar so the two don't
  // overlap.
  void drawQueueIndicator(size_t count) {
    if (count == 0) return;
    size_t shown = count < kMaxQueueDots ? count : kMaxQueueDots;
    int y = display_->height() - kBarMargin - kBarHeight - kQueueDotSize - 2;
    display_->startWrite();
    for (size_t i = 0; i < shown; i++) {
      int x = kMargin + static_cast<int>(i) * (kQueueDotSize + kQueueDotGap) +
              kQueueDotSize / 2;
      display_->fillCircle(x, y, kQueueDotSize / 2, TFT_WHITE);
    }
    display_->display();
    display_->endWrite();
  }

  // Holds whatever show() just typed on screen for durationMs, animating a
  // thin bar along the bottom that starts full-width and shrinks toward the
  // left edge as time elapses — a visual countdown to when the caller will
  // clear/revert the panel. Runs on the caller's thread same as show():
  // blocks for durationMs.
  //
  // abortRequested, if given, is polled once per stepMs (same idea as
  // show()'s abortRequested) and ends the hold early when it returns true.
  void holdWithCountdown(uint32_t durationMs, uint32_t stepMs = 50,
                         std::function<bool()> abortRequested = nullptr) {
    const int barX = kMargin;
    const int barWidth = display_->width() - 2 * kMargin;
    const int barY = display_->height() - kBarMargin - kBarHeight;
    const uint32_t start = millis();

    display_->startWrite();
    for (;;) {
      uint32_t elapsed = millis() - start;
      if (elapsed >= durationMs) break;
      if (abortRequested && abortRequested()) break;
      float remaining = 1.0f - static_cast<float>(elapsed) / durationMs;
      int w = static_cast<int>(barWidth * remaining);
      // Redraw the whole bar track each tick rather than only the shrunk-away
      // sliver: simpler than tracking last width, and cheap at this size.
      display_->fillRect(barX, barY, barWidth, kBarHeight, TFT_BLACK);
      if (w > 0) display_->fillRect(barX, barY, w, kBarHeight, TFT_WHITE);
      display_->display();
      vTaskDelay(pdMS_TO_TICKS(stepMs));
    }
    display_->fillRect(barX, barY, barWidth, kBarHeight, TFT_BLACK);
    display_->display();
    display_->endWrite();
  }

 private:
  static constexpr int kMargin = 7;  // +2px over the old 5px inset from the border
  // Row pitch at textSize_ == 1; show() scales this by textSize_ for "Large".
  static constexpr int kBaseLineHeight = 10;
  static constexpr int kBarHeight = 2;
  static constexpr int kBarMargin = 2;  // gap above the bottom border stroke
  static constexpr int kQueueDotSize = 3;
  static constexpr int kQueueDotGap = 2;
  // Bounded rather than one dot per queued message: at kQueueDotSize+kGap=5px
  // per dot, 6 dots plus kMargin already spans half the 128px panel width -
  // more than that would run into the right edge or overlap the countdown
  // bar's full-width sweep once it starts shrinking from the right.
  static constexpr size_t kMaxQueueDots = 6;

  std::vector<String> wrapLines(const char *text) {
    const int maxWidth = display_->width() - 2 * kMargin;
    std::vector<String> lines;
    String line;
    String word;
    auto flush = [&]() {
      if (line.length()) {
        lines.push_back(line);
        line = "";
      }
    };
    for (const char *p = text;; p++) {
      if (*p == ' ' || *p == '\0') {
        String candidate = line.length() ? line + " " + word : word;
        if (line.length() && display_->textWidth(candidate.c_str()) > maxWidth) {
          flush();
          line = word;
        } else {
          line = candidate;
        }
        word = "";
        if (*p == '\0') break;
      } else {
        word += *p;
      }
    }
    flush();
    return lines;
  }

  M5GFX *display_;
  std::function<void()> onChar_;
  uint8_t textSize_ = 1;  // see setTextSize()
};
