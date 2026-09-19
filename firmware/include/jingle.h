// Plays a short notification tune on the piezo (see main.cpp's PIEZO_PIN)
// when a message arrives, the same "optional peripheral, picked per-message"
// treatment as rgb_led.h's LedSpec - MqttLink calls play() once as a message
// starts showing (see mqtt_link.h's display()).
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Mirrors the "jingle" string a message payload can carry (see
// MqttLink::parseMessage()'s jingleFromString()). None means the message
// didn't ask for a jingle, so nothing plays.
enum class JingleTune {
  None,
  Chime,
  Alert,
  Fanfare,
  Gentle,
  Boarding,
  Beep,
  Coin,
  OneUp,
  StageClear,
  Descend,
  Trill,
};

class Jingle {
 public:
  explicit Jingle(uint8_t piezoPin) : piezoPin_(piezoPin) {}

  // Blocks the calling thread for the tune's duration (well under a second in
  // every case below), same as SpeechBubble::show()'s per-character reveal -
  // called from MqttLink::display() before typing starts, so this never
  // overlaps SpeechBubble's own per-character tone() calls (beepChar() in
  // main.cpp) on the same piezo pin. tone()'s duration argument is
  // non-blocking/LEDC-driven (see splash.h), so each note still needs an
  // explicit delay to keep the next one from cutting it off early.
  void play(JingleTune tune) {
    const Note *notes;
    size_t count;
    notesFor(tune, notes, count);
    playNotes(notes, count);
  }

  // Plays once as appTask starts connecting to the network (see main.cpp) -
  // in place of the "--:--" placeholder the bubble panel's idle clock used to
  // show for that whole wait (digital_clock.h's draw() now shows nothing at
  // all while unsynced, so there's no screen content to compete with this).
  // The app's own startup flourish: a bright, upbeat ascending C-major
  // arpeggio into a held top note. Deliberately not one of the JingleTune
  // notification tunes above and not selectable via a message's "jingle"
  // field - this always plays exactly once, at boot, never per-message. Also
  // deliberately not splash.cpp's Hirajoshi scale: that theme nods at the 原
  // mark landing, which has nothing to do with the product just being glad
  // to be alive.
  void playStartup() {
    static const Note kStartup[] = {
        {523, 90, 30},    // C5
        {659, 90, 30},    // E5
        {784, 90, 30},    // G5
        {1047, 90, 40},   // C6
        {1319, 220, 0},   // E6 - bright, held landing note
    };
    playNotes(kStartup, sizeof(kStartup) / sizeof(kStartup[0]));
  }

 private:
  struct Note {
    uint16_t freq;
    uint16_t durationMs;
    uint16_t gapMs;  // silence after this note, before the next one starts
  };

  void playNotes(const Note *notes, size_t count) {
    for (size_t i = 0; i < count; i++) {
      tone(piezoPin_, notes[i].freq, notes[i].durationMs);
      vTaskDelay(pdMS_TO_TICKS(notes[i].durationMs + notes[i].gapMs));
    }
  }

  static void notesFor(JingleTune tune, const Note *&notes, size_t &count) {
    // Notes named for the equal-tempered pitch they're closest to (C5=523Hz
    // etc.), purely so the tables below read as a tune rather than a wall of
    // numbers - no relation to splash.cpp's Hirajoshi SCALE, which is a
    // deliberate callback to the boot mark and has no reason to match a
    // notification jingle.
    // Lengthened 2026-09-16 (each was under 600ms, felt clipped) - Chime and
    // Gentle each gained a third note, Alert a fourth beep, Fanfare just
    // stretched its existing four - roughly 1.7-2x the original duration
    // apiece rather than only slower notes, so they still read as a tune and
    // not just the same phrase in slow motion.
    static const Note kChime[] = {
        {784, 200, 60},   // G5
        {659, 200, 60},   // E5
        {523, 450, 0},    // C5 - three-note "ding-dong-dong"
    };
    static const Note kAlert[] = {
        {1500, 100, 100}, {1500, 100, 100}, {1500, 100, 100}, {1500, 100, 0},  // four sharp beeps
    };
    static const Note kFanfare[] = {
        {523, 130, 30}, {659, 130, 30}, {784, 130, 30}, {1047, 260, 0},  // C5-E5-G5-C6
    };
    static const Note kGentle[] = {
        {440, 260, 80},   // A4
        {554, 260, 80},   // C#5
        {659, 380, 0},    // E5 - soft, unhurried three-note rise
    };
    // Added 2026-09-18 for the flights feed's arrivals/departures - a
    // discreet two-tone "bing-bong" played twice, the same shape as a real
    // airport PA chime, so a movement announcement reads as "attention"
    // rather than "urgent" the way Alert's sharp beeps would. Not yet
    // verified live on the board - see firmware/README.md's jingle table.
    static const Note kBoarding[] = {
        {784, 160, 50},   // G5 - "bing"
        {523, 160, 220},  // C5 - "bong"
        {784, 160, 50},   // G5 - "bing"
        {523, 320, 0},    // C5 - "bong", held for the landing
    };
    // Added 2026-09-19 for `sensor.reading` (see the API's triage.ts) - a
    // single short blip, deliberately smaller than every tune above. Those
    // all read as "come look at this"; a temperature reading just needs to
    // register as "noted" without competing for attention the way even
    // Gentle's three-note rise would.
    static const Note kBeep[] = {
        {1200, 60, 0},  // one short blip, no second note
    };
    // Added 2026-09-19, not yet wired to any kind in triage.ts - a bigger
    // catalog to pick from next time a feed needs a distinct voice, not a
    // change to what plays today. Not yet verified live on the board - see
    // firmware/README.md's jingle table.
    //
    // The three below are Mario Bros SFX, transcribed by ear rather than from
    // a reference - close enough to read as "that game" on a piezo buzzer,
    // not a faithful reproduction.
    static const Note kCoin[] = {
        {988, 70, 30},    // B5
        {1319, 200, 0},   // E6 - the coin "ding"
    };
    static const Note kOneUp[] = {
        {659, 100, 20},   // E5
        {784, 100, 20},   // G5
        {1319, 100, 20},  // E6
        {1047, 100, 20},  // C6
        {1175, 100, 20},  // D6
        {1568, 260, 0},   // G6 - the 1-up run, held on top
    };
    static const Note kStageClear[] = {
        {659, 90, 20},    // E5
        {784, 90, 20},    // G5
        {988, 90, 20},    // B5
        {1319, 90, 20},   // E6
        {1568, 280, 0},   // G6 - victory run, held on top
    };
    // A deliberate mirror of Gentle's rise, for the opposite occasion - a
    // soft three-note fall for "resolved/cleared" rather than "here's
    // something new."
    static const Note kDescend[] = {
        {659, 260, 80},   // E5
        {554, 260, 80},   // C#5
        {440, 380, 0},    // A4 - soft, unhurried three-note fall
    };
    // Two notes alternated twice, for "worth a second look" - busier than
    // Chime's single descending phrase but not sharp like Alert's beeps.
    static const Note kTrill[] = {
        {784, 90, 40},    // G5
        {1047, 90, 40},   // C6
        {784, 90, 40},    // G5
        {1047, 220, 0},   // C6 - held on the landing
    };
    switch (tune) {
      case JingleTune::Chime: notes = kChime; count = sizeof(kChime) / sizeof(kChime[0]); return;
      case JingleTune::Alert: notes = kAlert; count = sizeof(kAlert) / sizeof(kAlert[0]); return;
      case JingleTune::Fanfare: notes = kFanfare; count = sizeof(kFanfare) / sizeof(kFanfare[0]); return;
      case JingleTune::Gentle: notes = kGentle; count = sizeof(kGentle) / sizeof(kGentle[0]); return;
      case JingleTune::Boarding: notes = kBoarding; count = sizeof(kBoarding) / sizeof(kBoarding[0]); return;
      case JingleTune::Beep: notes = kBeep; count = sizeof(kBeep) / sizeof(kBeep[0]); return;
      case JingleTune::Coin: notes = kCoin; count = sizeof(kCoin) / sizeof(kCoin[0]); return;
      case JingleTune::OneUp: notes = kOneUp; count = sizeof(kOneUp) / sizeof(kOneUp[0]); return;
      case JingleTune::StageClear: notes = kStageClear; count = sizeof(kStageClear) / sizeof(kStageClear[0]); return;
      case JingleTune::Descend: notes = kDescend; count = sizeof(kDescend) / sizeof(kDescend[0]); return;
      case JingleTune::Trill: notes = kTrill; count = sizeof(kTrill) / sizeof(kTrill[0]); return;
      default: notes = nullptr; count = 0; return;
    }
  }

  uint8_t piezoPin_;
};
