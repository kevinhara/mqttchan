// Subscribes to a single MQTT topic carrying {"text":..., "led":..., ...}
// JSON and drives the announcer face + speech bubble from whatever arrives.
// Owns WiFi and MQTT connect/reconnect; call begin() once from setup-time
// code and loop() repeatedly from appTask.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <M5GFX.h>
#include <OneButton.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <ctype.h>
#include <time.h>
#include <deque>
#include <utility>

#include "portrait_face.h"
#include "speech_bubble.h"
#include "digital_clock.h"
#include "rgb_led.h"
#include "jingle.h"

class MqttLink {
 public:
  // clock, if given, is what the bubble panel reverts to once a message has
  // finished displaying (see display()) — nullptr just leaves the last
  // message on screen, the old behavior. led, if given, is lit for as long
  // as a message stays on screen (see display()) — nullptr just skips the
  // LED entirely, same "optional peripheral" treatment as clock. jingle, if
  // given, plays once as a message starts showing (see display()) — nullptr
  // just skips the jingle entirely, same treatment again. button, if
  // given, is polled (via its own tick(), so its usual debounce/click timing
  // still applies) from inside display()'s blocking show()/holdWithCountdown()
  // calls — see onButtonClick() — so a click can dismiss a message that is
  // still typing or counting down, not just one already finished.
  // faceDisplay/textDisplay, if given, are the two raw M5GFX panels
  // (announcer/face and bubble/clock respectively) — not otherwise reachable
  // through portraitFace_/bubble_, which only expose drawing calls, not
  // panel power state. Used solely by applyScreenPower() to send the
  // SSD1306 sleep/wake command overnight — see there. nullptr just skips
  // that entirely, same "optional peripheral" treatment as clock/led/jingle.
  //
  // Contract v2's "expression" field (happy/angry/sad/doubt/sleepy/neutral)
  // is gone as of 2026-09-19: it stopped driving anything the moment the
  // portrait pack replaced m5avatar's procedural eyes/mouth (see
  // portrait_face.h), and this removes the vestigial parse/validate-only
  // path it was left with since. Night/day still affects the face - see
  // setSleeping() below - but that's driven by the clock, not by a field in
  // the message.
  MqttLink(PortraitFace *portraitFace, SpeechBubble *bubble,
           DigitalClock *clock = nullptr, RgbLed *led = nullptr,
           OneButton *button = nullptr, Jingle *jingle = nullptr,
           M5GFX *faceDisplay = nullptr, M5GFX *textDisplay = nullptr)
      : portraitFace_(portraitFace),
        bubble_(bubble),
        clock_(clock),
        led_(led),
        button_(button),
        jingle_(jingle),
        faceDisplay_(faceDisplay),
        textDisplay_(textDisplay),
        mqtt_(wifiClient_) {
    self_ = this;
  }

  // Must be called before begin() - connectWiFi() reads deviceName_ to set
  // the WiFi hostname, and that has to happen ahead of WiFi.begin() to take
  // effect (see connectWiFi()). Also callable later (e.g. after a BLE config
  // write renames the device) - the new name takes effect on the next
  // reconnect, same as reconfigure()'s ssid/host/etc. changes.
  void setDeviceName(const String &name) { deviceName_ = name; }

  void begin(const String &ssid, const String &pass, const String &host,
             uint16_t port, const String &topic) {
    ssid_ = ssid;
    pass_ = pass;
    host_ = host;
    port_ = port;
    topic_ = topic;
    mqtt_.setServer(host_.c_str(), port_);
    // PubSubClient's default 256-byte buffer silently drops any larger
    // payload — no error, nothing in the callback, message just never
    // arrives. Sized deliberately instead; doubles as free payload-size
    // validation since oversized/malformed messages never reach display code.
    mqtt_.setBufferSize(512);
    // Default keepalive (15s) is tight once a long phrase's per-character
    // bubble reveal (~25ms/char, see speech_bubble.h) delays how often
    // loop() gets re-entered from appTask.
    mqtt_.setKeepAlive(60);
    mqtt_.setCallback(onMessage);
    connectWiFi();
    connectMqtt();
  }

  // Swaps in new credentials (e.g. from BleConfigService after a BLE config
  // write) and drops both links so the next loop() call reconnects with
  // them — loop() already does connect-if-down, so this just needs to make
  // "down" true. Doesn't block: appTask's next ~50ms tick does the work.
  // Callable only from appTask's own thread: it touches WiFi.disconnect()/
  // mqtt_.disconnect() (lwIP) and the ssid_/pass_/host_/port_/topic_ fields
  // connectWiFi()/connectMqtt() read, same as loop() itself - see main.cpp's
  // bleConfigDirty flag, which is how a NimBLE-thread BLE config write gets
  // this called on appTask instead of directly on NimBLE's host task.
  void reconfigure(const String &ssid, const String &pass, const String &host,
                    uint16_t port, const String &topic) {
    ssid_ = ssid;
    pass_ = pass;
    host_ = host;
    port_ = port;
    topic_ = topic;
    mqtt_.setServer(host_.c_str(), port_);
    mqtt_.disconnect();
    WiFi.disconnect();
  }

  // For BleConfigService's status characteristic. Not const: PubSubClient's
  // connected() isn't const-qualified.
  String statusString() {
    return String("wifi=") + (WiFi.status() == WL_CONNECTED ? "up" : "down") +
           " mqtt=" + (mqtt_.connected() ? "up" : "down");
  }

  // Call frequently (every ~50ms) from appTask. Services PubSubClient and
  // reconnects whichever of WiFi/MQTT has dropped — checks WiFi first so an
  // MQTT retry never fires against a dead link, the common hobby-AP failure
  // mode.
  void loop() {
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
      return;
    }
    if (!mqtt_.connected()) {
      connectMqtt();
      return;
    }
    mqtt_.loop();
    drainQueue();
    applyRestingExpression();
    applyScreenPower();
    if (replayRequested_) {
      // Set by onButtonClick() rather than calling display() directly from
      // there - see the correction in onButtonClick()'s comment for why that
      // has to be deferred to here, appTask's own top-level loop, instead.
      replayRequested_ = false;
      display(lastMessage_, 0);
      return;
    }
    if (configInfoRequested_) {
      // Same deferral as replayRequested_ just above, and for the same
      // reason - see onButtonClick().
      configInfoRequested_ = false;
      showStartupInfo(bleName_, bleAddress_);
      return;
    }
    if (petRequested_) {
      // Same deferral as replayRequested_ above - see onButtonLongPress().
      petRequested_ = false;
      playPetReaction();
      return;
    }
    if (statusRequested_) {
      // Same deferral as configInfoRequested_ above - see
      // onButtonDoubleClick(). Reuses the same screen configInfoRequested_
      // shows; the two flags exist separately only because they're set from
      // different button gestures with different idle-state guards.
      statusRequested_ = false;
      showStartupInfo(bleName_, bleAddress_);
      return;
    }
    PendingMessage msg;
    size_t remaining;
    if (!popQueued(&msg, &remaining)) return;
    display(msg, remaining);
  }

  // Headless bench test (see AVATAR_FB_DUMP in main.cpp): exercises the
  // parse-and-display path with a synthetic payload, no live broker needed.
  // Only ever called from appTask before the loop() above starts running
  // (see main.cpp), so there is no other task to race with — safe to
  // display immediately rather than going through the queue.
  void injectForTest(const char *json) {
    handleMessage(reinterpret_cast<const uint8_t *>(json), strlen(json));
  }

  // Used by BleConfigService's test-message characteristic. That write
  // callback runs on the NimBLE host task, not appTask - a different thread
  // from the one loop() above runs on. display() is not safe to call from
  // two threads at once (it drives the shared, non-reentrant oledText
  // display/I2C bus that idleClock->tick() also draws to from appTask every
  // ~50ms), so this must not call display() directly the way it used to.
  // Enqueuing instead means the message waits its turn and is always shown
  // from loop() on appTask, same as an MQTT message - see enqueue().
  void injectMessage(const uint8_t *payload, size_t length) {
    enqueue(payload, length);
  }

  // Updates how a finished message behaves on the bubble panel: holdMs is how
  // long it stays up before reverting to the idle clock, and keepLastMessage
  // (when true) skips that revert entirely - the last message just stays on
  // screen until the next one arrives. Takes effect on the next display()
  // call; doesn't cut short a hold already in progress. Called from appTask
  // at startup and again from BleConfigService's onConfig callback whenever
  // these settings change (see device_settings.h).
  void setDisplayOptions(uint32_t holdMs, bool keepLastMessage) {
    holdMs_ = holdMs;
    keepLastMessage_ = keepLastMessage;
  }

  // Updates the panels' brightness and overnight schedule - brightnessPercent
  // (0-100) is applied to both OLEDs immediately via applyBrightness();
  // offStartMinutes/offEndMinutes (each minutes-since-midnight, local time)
  // replace the window isScreenOffTime() checks, wrapping across midnight
  // when offStartMinutes > offEndMinutes the same way a plain "22:00-07:00"
  // reads; equal values disable the schedule (screens never power off on
  // their own). wakeForMessage controls whether an incoming message wakes
  // sleeping panels (see wakeForMessage() below) or is drawn silently into
  // GDDRAM for the panels to show whenever they next wake on their own.
  // Called from appTask at startup and again from BleConfigService's
  // onConfig callback, same as setDisplayOptions() above.
  void setScreenOptions(uint8_t brightnessPercent, uint16_t offStartMinutes,
                        uint16_t offEndMinutes, bool wakeForMessage) {
    brightnessPercent_ = brightnessPercent;
    screenOffStartMin_ = offStartMinutes;
    screenOffEndMin_ = offEndMinutes;
    wakeForMessageEnabled_ = wakeForMessage;
    applyBrightness();
  }

  // Records the user's configured message text size (SpeechBubble::
  // setTextSize() - 1 Small, 2 Large) so showStartupInfo() can force its own
  // dense status line back down to Small and then restore this afterward.
  // Called from the same two appTask spots that already push
  // settings.messageTextSize straight to the bubble - see there. Doesn't
  // touch the bubble itself: a real message's size is still applied by
  // those callers, this is only read back by showStartupInfo().
  void setMessageTextSize(uint8_t size) { messageTextSize_ = size; }

  // True once display() has left a message up under keepLastMessage_ instead
  // of reverting to the idle clock. main.cpp's loop must skip idleClock->tick()
  // while this holds - tick() redraws the same oledText panel the bubble used,
  // so an untouched idle clock would otherwise overwrite the "kept" message
  // the moment the wall-clock second changes, undoing keepLastMessage_ within
  // about a second of it finishing typing.
  bool isHoldingMessage() const { return messageHeld_; }

  // Wired up as the push button's click handler (see main.cpp) — a single
  // click dismisses whatever message is currently on screen (whether it's
  // still typing, counting down its hold, or sitting there indefinitely
  // under keepLastMessage_), or, when nothing is on screen, replays the last
  // message that was shown.
  //
  // Called from two different situations that both land on appTask's
  // thread, never concurrently: (1) main.cpp's plain button.tick() in its
  // idle loop, when no message is active or one is being held indefinitely
  // — display() isn't on the call stack, so any work needed happens right
  // here; (2) the button_->tick() call display() itself makes from inside
  // show()/holdWithCountdown() (see the constructor comment) while a message
  // is actively typing/counting down — display() is already polling
  // dismissRequested_ in that case, so this just sets the flag rather than
  // acting directly, and lets display() unwind itself once it notices.
  //
  // Correction, 2026-09-16: the idle branch used to call display(lastMessage_,
  // 0) directly, right here. Verified live that this makes the replayed
  // message flash up for about one character and then vanish immediately -
  // traced to OneButton::_fsm()'s OCS_COUNT case (OneButton.cpp): it invokes
  // this very callback *before* calling reset(), with _state still OCS_COUNT
  // and waitTime still past the click threshold. Calling display() inline
  // here reaches show()'s abortRequested lambda, which polls button_->tick()
  // again - reentrantly, on the same OneButton object, before the outer
  // tick() call has reset anything - and that nested tick() re-evaluates the
  // still-unreset OCS_COUNT state, decides a second click just happened, and
  // fires this callback again immediately. That second call lands with
  // messageActive_ already true (display() had just set it), so it takes the
  // dismiss branch above and cuts the reveal off after ~1 character. Setting
  // replayRequested_ instead defers the actual display() call to loop()'s
  // next pass, which runs from appTask's own top-level for(;;), never nested
  // inside a button_->tick() call - so its own button_->tick() polling is
  // never reentrant. Verified live 2026-09-16: replay now holds for the full
  // holdMs_ instead of vanishing.
  void onButtonClick() {
    if (messageActive_) {
      if (messageHeld_) {
        revertToIdle();
      } else {
        dismissRequested_ = true;
      }
      return;
    }
    // Correction, 2026-09-16: the idle-and-nothing-yet case used to do
    // nothing at all. It now shows the "what am I connected to" summary
    // instead (see showStartupInfo()) - deferred to loop() via
    // configInfoRequested_, the same indirection replayRequested_ above
    // uses and for the same reason (see the correction in this function's
    // header comment): calling straight into a blocking bubble_->show()
    // from inside this callback re-enters OneButton::tick() and misfires.
    if (haveLastMessage_) {
      replayRequested_ = true;
    } else {
      configInfoRequested_ = true;
    }
  }

  // Wired up as the push button's double-click handler (see main.cpp) - the
  // "what am I connected to" status screen (showStartupInfo()) on demand,
  // any time the device is idle, not just before the first message has ever
  // arrived (that's still the single-click idle behavior above). Ignored
  // while a message is active - same "don't interrupt what's on screen"
  // rule onButtonClick() follows for the dismiss/replay case - and deferred
  // to loop() via statusRequested_ for the same reentrancy reason (see the
  // correction in onButtonClick()'s header comment: calling straight into a
  // blocking bubble_->show() from inside this callback re-enters
  // OneButton::tick() and misfires).
  void onButtonDoubleClick() {
    if (messageActive_) return;
    statusRequested_ = true;
  }

  // Wired up as the push button's long-press handler (see main.cpp) - a
  // wordless "pet" gesture: a fresh random portrait, a soft jingle and a
  // brief warm LED glow (see playPetReaction()). Purely a toy flourish, so
  // it's ignored while a message is active rather than competing with it -
  // same guard as onButtonDoubleClick() above - and deferred to loop() via
  // petRequested_ for the same reentrancy reason.
  void onButtonLongPress() {
    if (messageActive_) return;
    petRequested_ = true;
  }

  // "What am I connected to" summary - reports the broker, link state,
  // network identity and BLE identity so a glance at the bubble answers that
  // without needing a laptop. Always holds for a fixed 10s regardless of
  // setDisplayOptions()'s holdMs_/keepLastMessage_ - those govern real MQTT
  // messages, not this one - then reverts to the idle clock the same way
  // display() does.
  //
  // Correction, 2026-09-16: this used to be shown automatically, once, right
  // after appTask's connect attempts settled - added 2026-09-15 so the info
  // was readable off the panel without a laptop. Changed so it no longer
  // appears on boot at all: a freshly-flashed device sitting on a desk has no
  // reason to broadcast its diagnostics before anyone's asked for them. It
  // now only shows on demand - see onButtonClick()'s configInfoRequested_
  // branch above, which fires on a click while idle and only for as long as
  // no MQTT/BLE message has ever arrived (haveLastMessage_ false); once any
  // message has been shown, that same idle click replays it instead (see
  // replayRequested_) and this screen is unreachable again until the next
  // reboot. bleName_/bleAddress_ are captured once via setBleIdentity() below
  // instead of being passed in here, since this is no longer called from a
  // context that has them to hand.
  void showStartupInfo(const String &bleName, const String &bleAddress) {
    if (bubble_ == nullptr) return;
    String text = String("MQTT ") + host_ + ":" + port_ + " - " +
                  (mqtt_.connected() ? "online" : "offline") + " | " +
                  deviceName_ + " " + WiFi.localIP().toString() + " | BLE " +
                  bleName + " " + bleAddress;
    // This packs far more into one screen than a real message ever does, so
    // it's always shown Small regardless of the user's configured message
    // size (messageTextSize_) - at Large it wraps into more lines than the
    // panel can hold before it even reaches the BLE identity. Restored to
    // the configured size below so the next real message isn't left small.
    bubble_->setTextSize(1);
    // button_->tick() pumped through both blocking calls below, same as
    // display() does for a real message - without it OneButton misses
    // however many polls this 10s+ round trip skips, which can desync its
    // click-timing state for whatever the next real press is. Always
    // returning false: nothing here supports dismissing this screen early,
    // just keeping OneButton's own bookkeeping current while it plays out.
    bubble_->show(text.c_str(), 45, [this] {
      if (button_ != nullptr) button_->tick();
      return false;
    });
    bubble_->holdWithCountdown(10000, 50, [this] {
      if (button_ != nullptr) button_->tick();
      return false;
    });
    bubble_->setTextSize(messageTextSize_);
    if (clock_ != nullptr) clock_->showNow();
  }

  // Records the BLE identity for showStartupInfo() above, called once from
  // main.cpp's appTask after BleConfigService::begin() - the same point that
  // used to call showStartupInfo() directly (see the correction there).
  // Doesn't draw anything itself. deviceName_ (WiFi hostname and self-
  // reference text) is set separately, via setDeviceName().
  void setBleIdentity(const String &bleName, const String &bleAddress) {
    bleName_ = bleName;
    bleAddress_ = bleAddress;
  }

 private:
  static MqttLink *self_;
  static constexpr const char *kClientId = "avatar-demo";
  // First-boot default for holdMs_ below - see DeviceSettings::load().
  static constexpr uint32_t kDefaultMessageHoldMs = 30000;
  // Silence held after a jingle finishes and before typing's own beeps
  // start - see display()'s use of this below.
  static constexpr uint32_t kPostJingleGapMs = 300;
  // How long playPetReaction() holds its LED glow before switching it off -
  // long enough to register as a deliberate flourish, short enough that a
  // repeated long-press doesn't feel laggy.
  static constexpr uint32_t kPetLedHoldMs = 600;
  // How often lipSyncTask advances the announcer to its next frame while a
  // message is typing - a steady talking pace rather than the old random
  // 60-140ms jitter, since real per-character frames (a blink, a mouth
  // flap) read better on a fixed beat than they did as noise.
  static constexpr uint32_t kTalkFrameIntervalMs = 150;

  void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("WiFi: connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(deviceName_.c_str());  // must precede begin() to take effect
    WiFi.begin(ssid_, pass_);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi: connected, ip=%s\n",
                    WiFi.localIP().toString().c_str());
    } else {
      Serial.println("WiFi: still not connected, will retry");
    }
  }

  void connectMqtt() {
    if (WiFi.status() != WL_CONNECTED) return;
    Serial.println("MQTT: connecting...");
    // LWT: broker marks this device offline (retained) if it disappears
    // without a clean disconnect, so late subscribers see current status.
    if (mqtt_.connect(kClientId, "avatar/status", 0, true, "offline")) {
      mqtt_.publish("avatar/status", "online", true);
      mqtt_.subscribe(topic_.c_str());
      Serial.println("MQTT: connected and subscribed");
    } else {
      Serial.printf("MQTT: connect failed, rc=%d\n", mqtt_.state());
      vTaskDelay(pdMS_TO_TICKS(2000 + random(0, 1000)));  // jitter
    }
  }

  // Parsed but not-yet-displayed message. Queued rather than shown
  // immediately so a burst of MQTT publishes doesn't get lost or garbled
  // while a previous message is still typing/holding on the bubble panel.
  struct PendingMessage {
    String text;
    LedSpec ledSpec;
    bool ledBlink = false;
    JingleTune jingle = JingleTune::None;
  };

  static void onMessage(char *topic, uint8_t *payload, unsigned int length) {
    if (self_ != nullptr) self_->enqueue(payload, length);
  }

  static LedSpec solidLed(uint8_t r, uint8_t g, uint8_t b) {
    LedSpec spec;
    spec.mode = LedSpec::Mode::Solid;
    spec.r = r;
    spec.g = g;
    spec.b = b;
    return spec;
  }

  static bool allHexDigits(const char *s) {
    for (; *s != '\0'; s++) {
      if (!isxdigit(static_cast<unsigned char>(*s))) return false;
    }
    return true;
  }

  // "led" is optional. Accepted forms, in the order tried:
  //   ""        - absent or empty, the LED stays off for this message
  //   "cycle"   - the HSV rainbow sweep (a mode, not a color)
  //   "#RRGGBB" - any color, with or without the leading '#'
  //   "red"/"green"/"blue" - aliases for the full-scale primaries, kept so the
  //               control page and older publishers keep working unchanged
  // Anything else logs a warning and leaves the LED off for this message -
  // the same "unrecognized falls back rather than fails" treatment as the
  // jingle field below. Matching is case-insensitive.
  //
  // Only the exact 6-digit hex form is accepted: a short "#fff" or a stray
  // trailing character is a typo worth warning about rather than something to
  // guess the intent of.
  static LedSpec ledSpecFromString(const String &raw) {
    LedSpec spec;  // defaults to Mode::Off
    String name = raw;
    name.trim();
    if (name.length() == 0) return spec;

    if (strcasecmp(name.c_str(), "cycle") == 0) {
      spec.mode = LedSpec::Mode::Cycle;
      return spec;
    }
    if (strcasecmp(name.c_str(), "red") == 0) return solidLed(255, 0, 0);
    if (strcasecmp(name.c_str(), "green") == 0) return solidLed(0, 255, 0);
    if (strcasecmp(name.c_str(), "blue") == 0) return solidLed(0, 0, 255);

    const char *hex = name.c_str();
    if (*hex == '#') hex++;
    if (strlen(hex) == 6 && allHexDigits(hex)) {
      uint32_t v = strtoul(hex, nullptr, 16);
      return solidLed((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }

    Serial.printf("MQTT: unrecognized led color '%s', leaving LED off\n",
                  raw.c_str());
    return spec;
  }

  // "jingle" is optional and, when present, must be one of these strings
  // (six as of "beep", 2026-09-19 - this comment previously said "four" and
  // was already stale by one at "boarding"; corrected while touching this
  // line rather than left for the next miscount), matched case-insensitively
  // for the same reason "led" is (contract v2, 2026-09-17) - being the one
  // field that still cared about case would be a trap, not a convention.
  // Anything else logs a warning and plays nothing
  // for this message, same "unrecognized falls back rather than fails"
  // treatment as the other two.
  static JingleTune jingleFromString(const String &name) {
    if (name.length() == 0) return JingleTune::None;
    if (strcasecmp(name.c_str(), "chime") == 0) return JingleTune::Chime;
    if (strcasecmp(name.c_str(), "alert") == 0) return JingleTune::Alert;
    if (strcasecmp(name.c_str(), "fanfare") == 0) return JingleTune::Fanfare;
    if (strcasecmp(name.c_str(), "gentle") == 0) return JingleTune::Gentle;
    if (strcasecmp(name.c_str(), "boarding") == 0) return JingleTune::Boarding;
    if (strcasecmp(name.c_str(), "beep") == 0) return JingleTune::Beep;
    if (strcasecmp(name.c_str(), "coin") == 0) return JingleTune::Coin;
    if (strcasecmp(name.c_str(), "oneup") == 0) return JingleTune::OneUp;
    if (strcasecmp(name.c_str(), "stageclear") == 0) return JingleTune::StageClear;
    if (strcasecmp(name.c_str(), "descend") == 0) return JingleTune::Descend;
    if (strcasecmp(name.c_str(), "trill") == 0) return JingleTune::Trill;
    Serial.printf("MQTT: unrecognized jingle '%s', playing nothing\n",
                  name.c_str());
    return JingleTune::None;
  }

  bool parseMessage(const uint8_t *payload, unsigned int length,
                     PendingMessage *out) {
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, reinterpret_cast<const char *>(payload), length);
    if (err) {
      Serial.printf("MQTT: bad JSON payload (%s)\n", err.c_str());
      return false;
    }
    out->text = doc["text"] | "";
    out->ledSpec = ledSpecFromString(doc["led"] | "");
    out->ledBlink = doc["blink"] | false;
    out->jingle = jingleFromString(doc["jingle"] | "");
    return true;
  }

  // Pushes a parsed message onto queue_ for loop() (appTask) to pick up and
  // display. Called from two different threads - PubSubClient's callback
  // (onMessage above), which runs inside mqtt_.loop() on appTask, and
  // injectMessage(), which runs on the NimBLE host task when a BLE client
  // writes the test-message characteristic - so queue_ access is guarded by
  // queueMutex_ rather than assuming a single caller thread. Deliberately
  // just parses and pushes rather than displaying inline - it must return
  // fast so mqtt_.loop() gets back to servicing the socket instead of
  // blocking on display(), which is what lets drainQueue() build up a real
  // backlog to count and display one at a time instead of only ever seeing
  // the single most recent message.
  void enqueue(const uint8_t *payload, unsigned int length) {
    PendingMessage msg;
    if (!parseMessage(payload, length, &msg)) return;
    xSemaphoreTake(queueMutex_, portMAX_DELAY);
    queue_.push_back(std::move(msg));
    xSemaphoreGive(queueMutex_);
  }

  // Used only by injectForTest, which runs during setup on appTask before
  // loop() below starts, so there's no other thread it can race with -
  // display() is safe to call inline here. Do not reuse this for anything
  // that might be called from another thread (e.g. a BLE callback) - see
  // injectMessage()/enqueue() for why that must go through the queue.
  void handleMessage(const uint8_t *payload, unsigned int length) {
    PendingMessage msg;
    if (!parseMessage(payload, length, &msg)) return;
    display(msg, queuedCount());
  }

  size_t queuedCount() {
    xSemaphoreTake(queueMutex_, portMAX_DELAY);
    size_t n = queue_.size();
    xSemaphoreGive(queueMutex_);
    return n;
  }

  // Pops the next message for loop() to display, or returns false if queue_
  // is empty. Guarded the same as enqueue() - see there for why.
  bool popQueued(PendingMessage *out, size_t *remaining) {
    xSemaphoreTake(queueMutex_, portMAX_DELAY);
    bool has = !queue_.empty();
    if (has) {
      *out = std::move(queue_.front());
      queue_.pop_front();
      *remaining = queue_.size();
    }
    xSemaphoreGive(queueMutex_);
    return has;
  }

  // mqtt_.loop() (called just before this in loop()) only reads one packet
  // per call, but display() blocks this thread for the length of a whole
  // message (typing + kMessageHoldMs) - tens of seconds. Whatever the broker
  // sends during that time just accumulates in the OS socket buffer, unread.
  // Draining right before picking the next message to show catches up on
  // that backlog as fast as enqueue() can absorb it, so queuedCount() below
  // reflects an actual count instead of always being 0 or 1. Capped so a
  // sustained flood can't stall WiFi/BLE servicing indefinitely.
  void drainQueue() {
    static constexpr size_t kMaxDrainIterations = 32;
    for (size_t i = 0; i < kMaxDrainIterations && mqtt_.connected(); i++) {
      size_t before = queuedCount();
      mqtt_.loop();
      if (queuedCount() == before) break;
    }
  }

  // remainingQueued is how many messages are still waiting behind this one -
  // drawn as a row of dots in the bubble panel's bottom-left corner so a
  // message flood is visible as "more coming" rather than silently queued.
  void display(const PendingMessage &msg, size_t remainingQueued) {
    // A new message is about to take over the panel, so any previously held
    // message is no longer what's on screen - reset before the keepLastMessage_
    // branch below decides whether this one ends up held too.
    messageHeld_ = false;
    if (bubble_ != nullptr && msg.text.length() > 0) {
      // Remembered so onButtonClick() can replay this exact message (text +
      // LED) once nothing is on screen - see there.
      lastMessage_ = msg;
      haveLastMessage_ = true;
      // "On screen" from here until revertToIdle() below - covers typing,
      // counting down, and (via keepLastMessage_) sitting indefinitely.
      // onButtonClick() checks this to decide dismiss-vs-replay.
      messageActive_ = true;
      dismissRequested_ = false;

      // Overnight the panels are powered off (see applyScreenPower()), but
      // that's meant to stop an unread idle clock/face lighting the room up
      // all night, not to swallow a real notification - wake both panels for
      // an actual message, same as a button press would. revertToIdle()
      // below puts them back to sleep once this message is done, if it's
      // still within the screen-off window.
      wakeForMessage();

      // Lit before typing starts and left running through the hold below -
      // "on screen" covers the whole reveal-plus-hold, not just the hold
      // - see rgb_led.h's start()/stop().
      if (led_ != nullptr) led_->start(msg.ledSpec, msg.ledBlink);
      // Played before typing starts, not alongside it - Jingle::play() blocks
      // this thread for the tune's duration, so it finishes before
      // bubble_->show() below starts firing its own per-character tone()
      // calls on the same piezo pin (see jingle.h). kPostJingleGapMs holds a
      // beat of silence afterward so the jingle's last note doesn't run
      // straight into the first typing beep - only when a tune actually
      // played (msg.jingle != None); skipped otherwise so a plain message
      // isn't delayed for a jingle it never had.
      if (jingle_ != nullptr) {
        jingle_->play(msg.jingle);
        if (msg.jingle != JingleTune::None) {
          vTaskDelay(pdMS_TO_TICKS(kPostJingleGapMs));
        }
      }
      lipSyncActive_ = true;
      xTaskCreatePinnedToCore(lipSyncTask, "lipSync", 2048, this, 1, nullptr,
                              PRO_CPU_NUM);
      // button_->tick() runs here, on this thread, roughly every charDelayMs
      // - see the constructor comment for why display() has to poll the
      // button itself rather than relying on main.cpp's own tick() loop,
      // which doesn't get a turn again until show()/holdWithCountdown()
      // below return.
      bubble_->show(msg.text.c_str(), 45, [this] {
        if (button_ != nullptr) button_->tick();
        return dismissRequested_;
      });
      lipSyncActive_ = false;

      if (dismissRequested_) {
        // Clicked mid-typing - go straight to idle rather than drawing the
        // queue indicator/countdown for a message that's being dismissed.
        revertToIdle();
        return;
      }
      bubble_->drawQueueIndicator(remainingQueued);

      if (keepLastMessage_) {
        // Configured to leave the last message up indefinitely (see
        // setDisplayOptions()) - skip the hold/countdown and the revert to
        // the idle clock below; the face and bubble stay exactly as set
        // until the next message arrives, or onButtonClick() dismisses it
        // directly (no blocking loop here to poll dismissRequested_ against
        // - see onButtonClick()). messageHeld_ tells main.cpp's loop to stop
        // calling idleClock->tick(), which would otherwise draw over this
        // same panel the moment the wall-clock second changes. The LED stays
        // lit too - it comes back to start()/stop() the next time a message
        // arrives, same as the bubble text it tracks.
        messageHeld_ = true;
        return;
      }

      // Hold the message on screen for a bit (with a shrinking countdown bar
      // along the bottom) before handing the face and bubble panel back to
      // their idle look, rather than reverting the instant typing finishes -
      // cut short by a click the same way show() above is.
      bubble_->holdWithCountdown(holdMs_, 50, [this] {
        if (button_ != nullptr) button_->tick();
        return dismissRequested_;
      });
      revertToIdle();
    }
  }

  // Shared "message is done being shown, go back to idle" cleanup - used
  // both when a hold finishes on its own and when a click cuts one short
  // (see onButtonClick()).
  void revertToIdle() {
    if (bubble_ != nullptr) bubble_->clear();
    messageActive_ = false;
    messageHeld_ = false;
    dismissRequested_ = false;
    // An announcement just finished (typed out and held, or dismissed
    // mid-typing) - swap in a new random face for next time, per the
    // "announcer" brief (see portrait_face.h's pickRandom()).
    if (portraitFace_ != nullptr) portraitFace_->pickRandom();
    // messageActive_ must already be false here - applyRestingExpression()
    // is a no-op while it's true - and restingIsNight_ is forced so the face
    // actually updates even if the last resting state (set before this
    // message arrived) already matched today's night/day state.
    restingIsNight_ = -1;
    applyRestingExpression();
    if (clock_ != nullptr) clock_->showNow();
    if (led_ != nullptr) led_->stop();
    // Undoes wakeForMessage() above, if it woke the panels for this message -
    // see there and resleepIfNight().
    resleepIfNight();
  }

  // The long-press "pet" reaction (see onButtonLongPress()) - a fresh random
  // portrait, the same jingle a gentle/ambient message would use, and a
  // brief warm-pink LED glow. No bubble text: this is a wordless flourish,
  // not a message, so it doesn't touch lastMessage_/haveLastMessage_ or
  // anything display() tracks. Blocking is fine here - the jingle plus the
  // LED hold below is well under a second, same tolerance loop()'s other
  // deferred branches already have.
  void playPetReaction() {
    if (portraitFace_ != nullptr) portraitFace_->pickRandom();
    if (jingle_ != nullptr) jingle_->play(JingleTune::Gentle);
    if (led_ != nullptr) {
      LedSpec spec;
      spec.mode = LedSpec::Mode::Solid;
      // Warm pink - deliberately not a color any message payload sends
      // (LedSpec's "led" field takes arbitrary hex, but triage.ts's actual
      // palette runs status colors, not this), so a glance tells "petted"
      // apart from "a message is showing".
      spec.r = 255;
      spec.g = 105;
      spec.b = 180;
      led_->start(spec, /*blink=*/false);
      vTaskDelay(pdMS_TO_TICKS(kPetLedHoldMs));
      led_->stop();
    }
  }

  // True between 23:00 and 07:00 local time - the window the resting face
  // should look asleep rather than its usual awake sway. Unsynced time (see
  // digital_clock.h's own 1970 check) reads as false: a device that hasn't
  // finished NTP sync yet has no reliable local time to judge night from,
  // and the boot sequence (see main.cpp's jingle/bubble text) already covers
  // that window on its own.
  bool isNighttime() const {
    time_t now = time(nullptr);
    struct tm local;
    if (localtime_r(&now, &local) == nullptr || local.tm_year < (2024 - 1900))
      return false;
    return local.tm_hour >= 23 || local.tm_hour < 7;
  }

  // Keeps the face's resting (no message on screen) sleep state in sync
  // with time of day - frozen overnight, breathing otherwise (see
  // portrait_face.h's setSleeping()) - without calling it every ~50ms tick
  // for no reason. Only actually calls it when the night/day state has
  // changed since the last call, tracked via restingIsNight_ (-1 = not yet
  // applied, e.g. right after boot or a revertToIdle() reset). No-op while
  // a message is on screen - that's portraitFace_'s startTalking()/
  // advanceTalkFrame() to control, not this.
  void applyRestingExpression() {
    if (messageActive_) return;
    bool night = isNighttime();
    int8_t state = night ? 1 : 0;
    if (state == restingIsNight_) return;
    restingIsNight_ = state;
    if (portraitFace_ != nullptr) portraitFace_->setSleeping(night);
  }

  // True while local time falls inside [screenOffStartMin_, screenOffEndMin_)
  // (minutes since midnight) - the window both OLED panels should be powered
  // off in, rather than just showing an unread idle clock/Sleepy face all
  // night. Defaults to midnight-07:00 (see the member initializers below),
  // matching this method's old hardcoded "hour < 7" behavior, but is now
  // configurable via setScreenOptions() - see device_settings.h's
  // screenOffStart/screenOffEnd. Deliberately independent of isNighttime()'s
  // fixed 23:00 threshold: that one is only about the resting face's
  // expression, not the panels' power state, and the two don't have to (and
  // don't by default) share a boundary. Wraps across midnight when start >
  // end (e.g. 22:00-07:00); start == end disables the schedule entirely, so
  // the panels never power off on their own. Same unsynced-time guard as
  // isNighttime(), and for the same reason.
  bool isScreenOffTime() const {
    if (screenOffStartMin_ == screenOffEndMin_) return false;
    time_t now = time(nullptr);
    struct tm local;
    if (localtime_r(&now, &local) == nullptr || local.tm_year < (2024 - 1900))
      return false;
    uint16_t nowMin = static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
    if (screenOffStartMin_ < screenOffEndMin_) {
      return nowMin >= screenOffStartMin_ && nowMin < screenOffEndMin_;
    }
    return nowMin >= screenOffStartMin_ || nowMin < screenOffEndMin_;
  }

  // Sends the SSD1306 sleep/wake command to both panels on an actual
  // midnight/morning transition - screensAsleep_ makes this a once-per-
  // transition call rather than one every ~50ms tick, same pattern as
  // applyRestingExpression()'s restingIsNight_. Unlike applyRestingExpression(),
  // this runs unconditionally rather than early-returning while a message is
  // active: the whole point is that the panels default to off overnight
  // regardless of what display() is doing to them at any given moment - an
  // idle clock/resting face never gets to light the room up unread. A real
  // message still wakes the panels for as long as it's on screen - see
  // wakeForMessage()/resleepIfNight(), called from display()/revertToIdle()
  // - so this function's job is specifically the *resting* state, not an
  // absolute "off" for the whole window.
  void applyScreenPower() {
    bool shouldSleep = isScreenOffTime();
    if (shouldSleep == screensAsleep_) return;
    screensAsleep_ = shouldSleep;
    if (faceDisplay_ != nullptr) {
      shouldSleep ? faceDisplay_->sleep() : faceDisplay_->wakeup();
    }
    if (textDisplay_ != nullptr) {
      shouldSleep ? textDisplay_->sleep() : textDisplay_->wakeup();
    }
  }

  // Called from display() right as a real message starts, before typing -
  // wakes both panels if applyScreenPower() had put them to sleep, the same
  // way a button press would, so an actual notification is never silently
  // swallowed overnight. Deliberately leaves screensAsleep_ set to true:
  // that flag tracks what the *resting* state should be, which hasn't
  // changed just because one message is being shown on top of it - see
  // resleepIfNight(), which reads it back once the message is done.
  //
  // Gated on wakeForMessageEnabled_ (see setScreenOptions()) - when the user
  // has turned that off, a message during the screen-off window is still
  // typed and held as normal, just onto panels that stay powered down; it
  // shows once they next wake on their own (schedule end, or a button
  // press), same as anything else that was drawn while asleep.
  void wakeForMessage() {
    if (!screensAsleep_ || !wakeForMessageEnabled_) return;
    if (faceDisplay_ != nullptr) faceDisplay_->wakeup();
    if (textDisplay_ != nullptr) textDisplay_->wakeup();
  }

  // Maps brightnessPercent_ (0-100) onto the SSD1306 contrast register's
  // 0-255 range and applies it to both panels - called from
  // setScreenOptions() whenever the setting changes. Doesn't interact with
  // portrait_face.h's pickRandom() cross-fade: that reads back whatever
  // level is current via getBrightness() as its "resting" brightness before
  // fading, so it always fades to/from whatever this last set.
  void applyBrightness() {
    uint8_t level = static_cast<uint8_t>(
        (static_cast<uint16_t>(brightnessPercent_) * 255 + 50) / 100);
    if (faceDisplay_ != nullptr) faceDisplay_->setBrightness(level);
    if (textDisplay_ != nullptr) textDisplay_->setBrightness(level);
  }

  // Undoes wakeForMessage() once a message finishes - called from
  // revertToIdle(). Recomputes isScreenOffTime() fresh rather than trusting
  // screensAsleep_ as-is: a message that started right at the midnight/07:00
  // boundary and held for a while (holdMs_, or indefinitely under
  // keepLastMessage_) could have crossed it in either direction while
  // loop() was blocked inside display(), so this is what keeps screensAsleep_
  // and the panels' actual state from drifting apart across that edge.
  void resleepIfNight() {
    bool night = isScreenOffTime();
    screensAsleep_ = night;
    if (!night) return;  // already awake from wakeForMessage() - leave it
    if (faceDisplay_ != nullptr) faceDisplay_->sleep();
    if (textDisplay_ != nullptr) textDisplay_->sleep();
  }

  // Cycles the announcer's portrait through its own frame set while
  // bubble_->show() reveals the text on the caller's thread - a real
  // per-character animation now (see portrait_face.h), not the noisy
  // mouth-ratio placeholder this replaced. Runs as its own task since
  // show() blocks the thread that would otherwise drive this loop.
  // advanceTalkFrame()/stopTalking() just flip a couple of ints and issue
  // one I2C blit, so calling them from here rather than appTask is safe the
  // same way the old setMouthOpenRatio() call was. Exits once display()
  // clears lipSyncActive_ after show() returns.
  static void lipSyncTask(void *arg) {
    auto *self = static_cast<MqttLink *>(arg);
    if (self->portraitFace_ != nullptr) self->portraitFace_->startTalking();
    while (self->lipSyncActive_) {
      if (self->portraitFace_ != nullptr) self->portraitFace_->advanceTalkFrame();
      vTaskDelay(pdMS_TO_TICKS(kTalkFrameIntervalMs));
    }
    if (self->portraitFace_ != nullptr) self->portraitFace_->stopTalking();
    vTaskDelete(nullptr);
  }

  volatile bool lipSyncActive_ = false;

  // Set by setDisplayOptions(), read by display() - see there for behavior.
  uint32_t holdMs_ = kDefaultMessageHoldMs;
  bool keepLastMessage_ = false;
  // Set by setMessageTextSize(), read by showStartupInfo() to restore the
  // bubble's size after forcing it to Small - see both.
  uint8_t messageTextSize_ = 1;
  // Set by display() when keepLastMessage_ leaves a message up instead of
  // reverting to the idle clock; read by isHoldingMessage(). See there.
  bool messageHeld_ = false;
  // True for the whole time a message is visibly on screen (typing,
  // counting down, or held indefinitely) - set at the top of display()'s
  // bubble branch, cleared by revertToIdle(). onButtonClick() reads this to
  // decide whether a click means "dismiss" or "replay the last message".
  bool messageActive_ = false;
  // -1/0/1 tri-state ("not yet applied"/day/night) rather than a plain bool,
  // so applyRestingExpression() can tell "never applied" (boot, or just
  // after revertToIdle() forces a re-check) from "already applied, and it
  // was day" - a plain bool defaulting to false would look identical to
  // "day" and skip the very first portraitFace_->setSleeping() call.
  // Read/written only from applyRestingExpression()/revertToIdle(), both on
  // appTask.
  int8_t restingIsNight_ = -1;
  // Plain bool, unlike restingIsNight_ above: applyScreenPower() runs
  // unconditionally (see there), so there's no "not yet applied" case to
  // distinguish from day - defaulting to false (awake) just means boot
  // doesn't send a redundant wakeup() to panels that are already on.
  bool screensAsleep_ = false;
  // Set by setScreenOptions(), read by applyBrightness()/isScreenOffTime()/
  // wakeForMessage(). Defaults reproduce this class's old hardcoded
  // behavior (full brightness, midnight-07:00 screen-off, always wake for a
  // message) for the brief window before main.cpp's first real call - see
  // setScreenOptions()'s comment.
  uint8_t brightnessPercent_ = 100;
  uint16_t screenOffStartMin_ = 0;
  uint16_t screenOffEndMin_ = 7 * 60;
  bool wakeForMessageEnabled_ = true;
  // Set by onButtonClick() while display() is blocked inside show()/
  // holdWithCountdown(); polled by the lambdas passed to those calls (see
  // display()) so a click can cut a message short instead of waiting for it
  // to finish on its own.
  volatile bool dismissRequested_ = false;
  // The last message actually shown (text/LED), so onButtonClick()
  // can redisplay it when nothing is currently on screen. Only ever set from
  // display(), on appTask's own thread - see onButtonClick().
  PendingMessage lastMessage_;
  bool haveLastMessage_ = false;
  // Set by onButtonClick() when a click arrives with nothing on screen;
  // consumed by loop() on its next pass, which calls display(lastMessage_,
  // 0) from there instead of onButtonClick() calling it directly - see the
  // correction in onButtonClick()'s comment for why that indirection is
  // required (calling display() straight from the click callback re-enters
  // OneButton::tick() and double-fires the click).
  volatile bool replayRequested_ = false;
  // Set by onButtonClick() when a click arrives idle and no message has ever
  // been shown (haveLastMessage_ false); consumed by loop() the same way
  // replayRequested_ is, for the same re-entrancy reason - see there.
  volatile bool configInfoRequested_ = false;
  // Set by onButtonLongPress() when idle; consumed by loop() the same way
  // replayRequested_ is, for the same re-entrancy reason - see there and
  // playPetReaction().
  volatile bool petRequested_ = false;
  // Set by onButtonDoubleClick() when idle; consumed by loop() the same way
  // configInfoRequested_ is, for the same re-entrancy reason - see there.
  volatile bool statusRequested_ = false;
  // Captured once via setBleIdentity(), read by showStartupInfo() when
  // configInfoRequested_/statusRequested_ fires - see both.
  String bleName_;
  String bleAddress_;

  // Messages waiting to be displayed - pushed by enqueue() (from either the
  // MQTT callback on appTask or a BLE test-message write on the NimBLE host
  // task) and popped by loop() on appTask - see popQueued(). Not touched by
  // injectForTest, which displays straight away (see handleMessage()).
  std::deque<PendingMessage> queue_;
  // Guards queue_ since enqueue()/popQueued()/queuedCount() can be called
  // from two different FreeRTOS tasks (appTask and the NimBLE host task) -
  // see enqueue()'s comment.
  SemaphoreHandle_t queueMutex_ = xSemaphoreCreateMutex();

  PortraitFace *portraitFace_;
  SpeechBubble *bubble_;
  DigitalClock *clock_;
  RgbLed *led_;
  OneButton *button_;
  Jingle *jingle_;
  M5GFX *faceDisplay_;
  M5GFX *textDisplay_;

  // WiFi hostname and self-reference text (showStartupInfo()) - set via
  // setDeviceName() before begin(), and again whenever a BLE config write
  // renames the device (see main.cpp's bleConfigDirty handling).
  String deviceName_;

  // String, not const char*: values can now come from BLE config writes
  // (see reconfigure()), which don't outlive a caller-owned buffer the way
  // secrets.h's compile-time constants do.
  String ssid_;
  String pass_;
  String host_;
  uint16_t port_ = 0;
  String topic_;

  WiFiClient wifiClient_;
  PubSubClient mqtt_;
};

MqttLink *MqttLink::self_ = nullptr;
