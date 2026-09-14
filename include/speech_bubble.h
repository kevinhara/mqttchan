// Renders spoken text as a word-wrapped, bordered "speech bubble" on a
// second panel, independent of the avatar's own M5.Display. See
// ssd1306_display.h for why the avatar needs M5.Display to itself; this
// class just draws directly to whatever M5GFX device it is given.
#pragma once

#include <Arduino.h>
#include <M5GFX.h>
#include <vector>

class SpeechBubble {
 public:
  explicit SpeechBubble(M5GFX *display) : display_(display) {
    display_->setTextColor(TFT_WHITE, TFT_BLACK);
    display_->setTextSize(1);
    // Word-wrap by hand below; the built-in wrap breaks mid-word.
    display_->setTextWrap(false, false);
  }

  // Reveals text one character at a time, typewriter-style. Lines are
  // wrapped up front (wrapping needs the whole word to measure it, so it
  // can't be decided mid-reveal) and then typed out line by line. Runs on
  // whatever task calls it — this blocks for roughly strlen(text) *
  // charDelayMs, which is the point: it paces itself to be readable rather
  // than dumping the whole bubble in one frame. 25ms lands around a fast
  // typist's pace; drop it if a phrase is long enough to feel sluggish.
  void show(const char *text, uint32_t charDelayMs = 25) {
    display_->startWrite();
    display_->fillScreen(TFT_BLACK);
    display_->drawRoundRect(0, 0, display_->width(), display_->height(), 6,
                             TFT_WHITE);
    int cursorY = kMargin;
    for (const auto &line : wrapLines(text)) {
      display_->setCursor(kMargin, cursorY);
      for (size_t i = 0; i < line.length(); i++) {
        display_->print(line[i]);
        vTaskDelay(pdMS_TO_TICKS(charDelayMs));
      }
      cursorY += kLineHeight;
    }
    display_->endWrite();
  }

  void clear() { display_->fillScreen(TFT_BLACK); }

 private:
  static constexpr int kMargin = 5;
  static constexpr int kLineHeight = 10;

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
};
