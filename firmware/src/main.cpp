// Bare ESP32-WROOM + two SSD1306 128x64 OLEDs: one for the announcer's face
// (a random portrait, see portrait_face.h), one for the typed message.
//
// Correction, 2026-09-18: this used to run m5stack-avatar's procedural
// eyes/mouth on the face panel, which is why this file's core split existed
// - m5stack-avatar hardcodes both of its FreeRTOS tasks (rendering, and the
// blink/saccade/breath state machine) to APP_CPU_NUM in Avatar::start(), so
// Core 1 was "owned by the avatar" and everything else had to stay off it.
// PortraitFace replaced that outright: it draws synchronously, from
// whichever task calls it (appTask's own loop for idle breathing, a small
// helper task for the per-message frame cycle - see mqtt_link.h's
// lipSyncTask), so nothing hardcodes a core anymore. appTask is still
// pinned to core 0 below, but now only for the same reason the app logic
// was already there - WiFi/BT stacks live on PRO_CPU, and that's also
// where TTS/wake-word/LLM-client work would hang off if this grows one.
// Core 1 just runs Arduino's own (empty) loop() and is otherwise unused.

#include <Arduino.h>
#include <M5Unified.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <OneButton.h>

#include "ssd1306_display.h"
#include "portrait_face.h"
#include "speech_bubble.h"
#include "digital_clock.h"
#include "mqtt_link.h"
#include "device_settings.h"
#include "ble_config.h"
#include "secrets.h"
#include "boot.h"
#include "splash.h"
#include "rgb_led.h"
#include "jingle.h"

// SSD1306 on SDA=GPIO4, SCL=GPIO15, address 0x3C.
// SDA is on GPIO4 rather than the more usual GPIO2 because GPIO2 is a
// boot-strapping pin — the OLED's pull-up on it holds the ESP32 out of USB
// download mode ("Wrong boot mode detected (0x1b)") and flashing fails with
// the display attached. GPIO4 has no strapping role. Same wiring as the
// sibling `hello-world` panel in this folder.
#define OLED_SDA 4
#define OLED_SCL 15

// Second OLED, on its own bus rather than sharing the first one: GPIO32/33,
// driven as *software* (bit-banged) I2C, so it can sit at the default 0x3C
// address with no jumper/resistor mod needed. A negative i2c_port tells
// LGFX's Bus_I2C to use its bit-banged path (soft_i2c.inl) instead of one of
// the two hardware peripherals — see Bus_I2C.hpp's config_t comment ("e.g.
// ESP32 0=I2C_NUM_0 / 1=I2C_NUM_1 / negative=software"). GPIO32/33 avoid the
// pins already spoken for (4/15 here, boot-strapping pins 0/2/5/12) and
// aren't used elsewhere on this board. Clocked slower than the hardware bus
// (400kHz vs 800kHz) since bit-banged timing is CPU-cycle-bound and less
// forgiving at high speed.
#define OLED_TEXT_SDA 32
#define OLED_TEXT_SCL 33
#define OLED_TEXT_I2C_PORT (-1)

// Passive piezo for the per-character beep in the speech bubble (see
// SpeechBubble's onChar hook). GPIO27 avoids both OLEDs' pins above, the
// boot-strapping pins (0/2/5/12), and the flash pins (6-11).
#define PIEZO_PIN 27

// 4-pin RGB LED (common-cathode assumed: common leg to GND, each color leg
// through its own current-limiting resistor to the GPIO below). Board is a
// 30-pin ESP32 devkit with no GPIO16/17 broken out, so these three were
// picked instead — all LEDC-PWM-capable, clear of the OLED buses (4/15,
// 32/33), the piezo (27), the boot-strap pins (0/2/5/12), and M5Unified's
// reserved PortA pins (21/22). If the LED turns out to be common-anode
// instead (common leg to 3.3V), define RGB_COMMON_ANODE below to invert the
// PWM duty cycle rather than rewiring.
#define RGB_R_PIN 25
#define RGB_G_PIN 26
#define RGB_B_PIN 14
// #define RGB_COMMON_ANODE

// Multifunction push button. 4-leg tactile switches like this are a single
// SPST contact, not four independent ones, but which two legs are already
// shorted together inside the case (and so must be avoided as a pair) varies
// by part - don't assume a layout, check it with a multimeter.
//
// Correction, 2026-09-16: this used to claim the shorted pairs are always
// "diagonally opposite" corners, on the assumption that the two legs on a
// given side are permanently tied together and only the diagonal picks work
// as a switch. That's wrong for the switch actually on this board - a
// continuity check across the two same-side legs wired here (GPIO13 leg and
// GND leg) read open at rest and closed on press, i.e. a working switch
// contact, which the "same side is always shorted" assumption said should
// be impossible. Verified live 2026-09-16 by the wiring in place: pin reads
// HIGH released, LOW pressed, matching OneButton's activeLow=true below.
// OneButton (see platformio.ini) enables the internal pull-up, so the pin
// reads HIGH released and LOW pressed. GPIO13 avoids the OLED buses (4/15,
// 32/33), the piezo (27), the RGB LED (14/25/26), the boot-strapping pins
// (0/2/5/12), and the flash pins (6-11).
#define BUTTON_PIN 13

#ifdef RGB_LED_TEST
// Bench-only wiring test, built with `-D RGB_LED_TEST` (see platformio.ini)
// instead of the real app — same pattern as AVATAR_DEMO_MODE/AVATAR_FB_DUMP
// below, but standalone at setup()/loop() rather than inside appTask, since
// this needs no OLEDs/WiFi/MQTT and is meant to run before any of that is
// wired up. Remove the flag and reflash to go back to the real app.
static constexpr uint32_t kRgbPwmFreq = 5000;
static constexpr uint8_t kRgbPwmRes = 8;  // 8-bit duty: 0-255

static void rgbSetColor(uint8_t r, uint8_t g, uint8_t b) {
#ifdef RGB_COMMON_ANODE
  r = 255 - r;
  g = 255 - g;
  b = 255 - b;
#endif
  ledcWrite(RGB_R_PIN, r);
  ledcWrite(RGB_G_PIN, g);
  ledcWrite(RGB_B_PIN, b);
}

// Standard HSV(h in [0,360), s=v=1) -> RGB, for the spectrum sweep below.
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
#endif  // RGB_LED_TEST

// Swapped 2026-09-15: the two physical OLED panels turned out to be mounted
// reversed relative to their wiring, so `oled` (the avatar/face panel) and
// `oledText` (bubble+clock) now each take the *other's* former pins/bus to
// put content back on the correct physical screen. GPIO4/15's hardware bus
// and GPIO32/33's bit-banged bus are unchanged as electrical facts (see the
// macro comments above) — only which content rides which bus moved. Net
// effect: the avatar now runs on the slower 400kHz bit-banged bus instead of
// the 800kHz hardware one, until the wiring itself gets corrected instead.
static SSD1306Display oled(OLED_TEXT_SDA, OLED_TEXT_SCL, 400000,
                            OLED_TEXT_I2C_PORT, 0x3C);
static SSD1306Display oledText(OLED_SDA, OLED_SCL);

// HLI boot POST + splash intro (boot.h/splash.h, ported verbatim from
// ~/Code/esp32_oled/HLI): drawn through U8g2's own I2C drivers, not through
// the LGFX `oled`/`oledText` objects above, since boot.cpp/splash.cpp draw
// directly to a U8G2& and porting them to LGFX wasn't worth it for a
// sequence that only plays once, before each panel gets handed to its real
// owner (the avatar / the clock+bubble). Named for physical position rather
// than content below, since which sequence draws where has already flipped
// once (2026-09-16: POST moved to the top panel, splash to the bottom one —
// see setup()).
//
// Correction, 2026-09-15: this used to say the two sequences "run on
// separate panels concurrently rather than one after another" — setup()
// drew both every frame in one shared while loop. That turned out to be why
// *both* were reported janky, not just the slower of the two: sharing one
// frame clock meant each sequence's own animation was throttled down to
// whatever pace the loop as a whole could sustain, which was however long
// the slower panel's send took, regardless of how fast the other one's own
// bus was. setup() below now runs them as two genuinely separate phases —
// POST alone first, then splash alone — so each phase's frame rate is
// bounded only by its own panel.
//
// U8G2_R0 on both, not U8G2_R2::R2 (180°) looked upside down on real
// hardware even though it matches M5.Display's/oledText's own
// setRotation(2) below — U8g2's rotation and LGFX's rotation(2) don't agree
// for this SSD1306 driver/panel combination. Verified live 2026-09-15.
//
// Pins/bus swapped 2026-09-15 along with `oled`/`oledText` above, for the
// same physical-panel reversal — u8g2Top must stay on whichever pins `oled`
// now uses (both drive the physically-top panel, one at boot and one at
// runtime), same for u8g2Bottom/oledText. That puts u8g2Top (POST) on
// GPIO32/33 and u8g2Bottom (splash) on GPIO4/15.
//
// u8g2Top claims the ESP32's second hardware I2C peripheral (Wire1) for
// GPIO32/33 instead of U8g2's own software/bit-banged I2C, even though that
// pair has no pre-existing hardware claim the way GPIO4/15 does — Wire1
// sits completely idle for the whole length of this phase regardless, and
// U8g2's software I2C bit-bangs through digitalWrite() per bit, which
// measured slow enough (tens of ms to send one 1024-byte frame) to be the
// actual cause of POST's own reported jank, on top of the shared-loop
// problem above. See Wire1.begin() in setup() and platformio.ini's
// U8X8_HAVE_2ND_HW_I2C flag — without it the _2ND_HW_I2C constructor below
// is a silent no-op. u8g2Bottom needs no equivalent treatment: it already
// sits on GPIO4/15's actual hardware peripheral (global Wire/I2C_NUM_0).
static U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C u8g2Top(U8G2_R0,
                                                        U8X8_PIN_NONE);
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2Bottom(U8G2_R0, U8X8_PIN_NONE,
                                                       OLED_SCL, OLED_SDA);
// The face panel's announcer - a pointer, not a plain static object, since
// it needs `oled` already init()'d before it can draw its first frame; see
// its construction in setup(), right where avatar.setFace()/init() used to
// sit.
static PortraitFace *portraitFace = nullptr;
static SpeechBubble *bubble = nullptr;
// The bubble panel's idle/default face — MqttLink switches back to this once
// a message has finished displaying (see mqtt_link.h's handleMessage()).
// Named idleClock, not clock: that shadows <time.h>'s clock() at global scope
// and fails to compile.
static DigitalClock *idleClock = nullptr;
// Real (non-RGB_LED_TEST) app's RGB status LED, on the same three pins the
// bench test above uses. begin() is called from the real setup() below;
// MqttLink is handed a pointer to this so it can light it for as long as a
// message is on screen (see mqtt_link.h's display()/rgb_led.h's start()).
static RgbLed rgbLed(RGB_R_PIN, RGB_G_PIN, RGB_B_PIN);
// Plays a short notification tune, picked per-message the same way LED color
// is (see mqtt_link.h's display()/jingle.h's Jingle::play()). Shares
// PIEZO_PIN with beepChar() below and splash's boot theme - never contends
// with either, since it only ever plays synchronously from inside
// MqttLink::display(), before that message's own typing beeps start.
static Jingle jingle(PIEZO_PIN);

// Loaded in setup(), ahead of the boot POST screen, so its NVS-backed values
// (see device_settings.h) are available for boot::begin() to show - well
// before appTask (which used to be the one loading this) even starts. Global
// rather than local to appTask for that reason; appTask just reads it.
static constexpr const char *kDefaultDeviceName = "mqttchan";
static DeviceSettings settings;

// tone() is non-blocking (LEDC-driven), so this is safe to call from inside
// SpeechBubble::show()'s per-character reveal loop without slowing it down.
static void beepChar() { tone(PIEZO_PIN, 1800, 15); }

// Contract v2's "expression" vocabulary - kept only so MqttLink can keep
// validating/warning on an unrecognized value, same as always. Used to also
// select an m5avatar::Expression for the face; PortraitFace has no such
// per-mood variants, so nothing consumes the matched value anymore - see
// the constructor comment in mqtt_link.h.
static const char *const kExpressionNames[] = {"Happy",  "Angry",  "Sad",
                                               "Doubt",  "Sleepy", "Neutral"};
static constexpr size_t kExpressionCount =
    sizeof(kExpressionNames) / sizeof(kExpressionNames[0]);

#ifdef AVATAR_DEMO_MODE
// One line per expression above, in the same order — what the bubble shows
// while that expression is active. Placeholder for real TTS text; nihilistic
// one-liners for now because a demo bubble should at least be funny. Only
// used by the self-cycling demo loop below — real messages come from MQTT.
static const char *const kPhrases[] = {
    "Smile. The void won't notice.", "Scream away. Nothing's listening.",
    "Cheer up - nothing matters.",    "No one's driving. Never was.",
    "Rest easy. Heat death can wait.", "Just meat, doing meat things."};

// Cycles the announcer through its own frame set for `ms` - the same thing
// mqtt_link.h's lipSyncTask does for a real message, but driven by hand
// since this demo mode has no MQTT message triggering that path. Stand-in
// for what a real TTS engine on core 0 would drive instead.
static void speakFor(uint32_t ms) {
  if (portraitFace == nullptr) return;
  portraitFace->startTalking();
  const uint32_t until = millis() + ms;
  while (millis() < until) {
    portraitFace->advanceTalkFrame();
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  portraitFace->stopTalking();
}

// Ticks the announcer's idle breathing sway during a demo-loop pause -
// appTask's real for(;;) loop does this every ~50ms (see below); demoLoop()
// has no equivalent loop of its own during its plain vTaskDelay() gaps, so
// without this the face would just sit frozen between phrases.
static void idleDelay(uint32_t ms) {
  const uint32_t until = millis() + ms;
  while (millis() < until) {
    if (portraitFace != nullptr) portraitFace->tick(millis());
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
#endif

#ifdef AVATAR_FB_DUMP
// Debug aid for a headless bench: dump what is actually in the panel's
// framebuffer as ASCII, since Panel_HasBuffer keeps a readable RAM copy.
// Build with -DAVATAR_FB_DUMP to enable. Costs nothing when off. No
// suspend/resume needed around the read (unlike the old m5avatar version) -
// PortraitFace has no background draw task to race; it only ever draws
// synchronously from whichever caller invokes it.
static void dumpFramebuffer(const char *label) {
  Serial.printf("---- %s ----\n", label);
  for (int y = 0; y < 64; y += 2) {
    char row[129];
    for (int x = 0; x < 128; x++) {
      // Sample two rows per line so 64 rows fit a terminal at 1:1 aspect.
      bool on = M5.Display.readPixel(x, y) || M5.Display.readPixel(x, y + 1);
      row[x] = on ? '#' : '.';
    }
    row[128] = 0;
    Serial.println(row);
  }
}

// Same trick for the bubble panel. Nothing else writes to oledText besides
// appTask itself, which is what's calling this, so there's no concurrent
// writer to freeze.
static void dumpBubbleFramebuffer(const char *label) {
  if (bubble == nullptr) return;
  Serial.printf("---- bubble: %s ----\n", label);
  for (int y = 0; y < 64; y += 2) {
    char row[129];
    for (int x = 0; x < 128; x++) {
      bool on = oledText.readPixel(x, y) || oledText.readPixel(x, y + 1);
      row[x] = on ? '#' : '.';
    }
    row[128] = 0;
    Serial.println(row);
  }
}
#endif

#ifdef AVATAR_DEMO_MODE
// Self-cycling bench demo: no network needed, just the two OLEDs. Build with
// -DAVATAR_DEMO_MODE to get this instead of the real MQTT-driven behavior
// below — same idea as -DAVATAR_FB_DUMP, a flag-gated mode for bench testing.
static void demoLoop() {
  size_t i = 0;
  for (;;) {
    // Swaps the announcer to a new random face each pass - the same call
    // MqttLink::revertToIdle() makes after a real announcement.
    if (portraitFace != nullptr) portraitFace->pickRandom();
    if (bubble != nullptr) {
      bubble->show(kPhrases[i % kExpressionCount]);
    }

    Serial.printf("[core %d] phrase=%-8s heap=%6u draw=core %d\n",
                  xPortGetCoreID(), kExpressionNames[i % kExpressionCount],
                  (unsigned)ESP.getFreeHeap(), APP_CPU_NUM);

#ifdef AVATAR_FB_DUMP
    delay(400);
    dumpFramebuffer(kExpressionNames[i % kExpressionCount]);
    dumpBubbleFramebuffer(kExpressionNames[i % kExpressionCount]);
#endif

    // Every third phrase, babble for a bit so the portrait's own frames
    // cycle (see speakFor()).
    if (i % 3 == 2) {
      speakFor(1500);
      idleDelay(1000);
    } else {
      idleDelay(2500);
    }
    i++;
  }
}
#endif

// Pinned to core 0 - see the file-header comment for why (WiFi/BT locality,
// not a hard requirement from PortraitFace, which draws wherever it's called
// from).
static void appTask(void *) {
#ifdef AVATAR_DEMO_MODE
  demoLoop();
#else
  // `settings` is already loaded - see setup(), which needs it ahead of this
  // task even existing (for the boot POST screen's config lines).

  // Declared ahead of mqttLink below so it can be handed to MqttLink's
  // constructor - MqttLink polls it (via button.tick()) from inside its own
  // blocking show()/holdWithCountdown() calls, so a click can dismiss a
  // message that's still typing or counting down rather than only one
  // that's already finished. attachClick()/attachDoubleClick()/
  // attachLongPressStart() below still run after mqttLink exists, same as
  // before - only the declaration moved up.
  static OneButton button(BUTTON_PIN, /*activeLow=*/true,
                           /*pullupActive=*/true);

  static MqttLink mqttLink(portraitFace, bubble, kExpressionNames,
                            kExpressionCount, idleClock, &rgbLed, &button,
                            &jingle, &oled, &oledText);
  // Plays once, right as appTask starts up, before the bubble panel shows
  // anything - see jingle.h's playStartup() for why this is its own tune
  // rather than one of the message-notification jingles or splash.cpp's boot
  // theme. The 1s delay after it finishes (playStartup() itself blocks for
  // the tune's ~700ms) gives the jingle a clear moment on its own before the
  // "Connecting to <ssid>" bubble starts typing, rather than the two
  // happening on top of each other.
  jingle.playStartup();
  vTaskDelay(pdMS_TO_TICKS(1000));
  // Bubble panel's own boot sequence: "Connecting to <ssid>" while
  // connectWiFi() (inside begin() below) does its blocking wait, then
  // "Fetching data" once that attempt has settled and MQTT/SNTP take over -
  // digital_clock.h's draw() picks up that same "Fetching data" text once
  // idleClock starts ticking below, so the panel never goes blank for the
  // rest of the unsynced wait. Both are typed fast (20ms/char, vs. a real
  // message's 45ms) since these are status text, not something to savor.
  if (bubble != nullptr) {
    bubble->show(("Connecting to " + settings.ssid).c_str(), /*charDelayMs=*/20);
  }
  // Must precede begin(): connectWiFi() (called from begin()) reads this to
  // set the WiFi hostname, which ESP32 only honors if set before WiFi.begin().
  mqttLink.setDeviceName(settings.name);
  mqttLink.begin(settings.ssid, settings.pass, settings.host, settings.port,
                 settings.topic);
  if (bubble != nullptr) bubble->show("Fetching data", /*charDelayMs=*/20);
  mqttLink.setDisplayOptions(settings.messageHoldSeconds * 1000UL,
                              settings.keepLastMessage);
  if (idleClock != nullptr) idleClock->setTimezone(settings.tz);

  // BLE config: a phone (nRF Connect, LightBlue, ...) can connect, read/write
  // WiFi+MQTT+display settings, and push a one-off test message through the
  // same {"text":...,"expression":...} JSON path MQTT uses — see
  // ble_config.h. Started ahead of mqttLink.setBleIdentity() below so that
  // call can capture the BLE identity NimBLEDevice::init() assigns, alongside
  // the WiFi one, instead of just the MQTT one.
  //
  // onConfig_ below runs on NimBLE's own host task, not appTask (see
  // ble_config.h's file-header comment). mqttLink.reconfigure() touches
  // WiFi.disconnect()/mqtt_.disconnect() and idleClock->setTimezone() touches
  // configTzTime()/SNTP - both ultimately reach into lwIP, which is the same
  // subsystem appTask's own connectWiFi()/connectMqtt() (called from
  // mqttLink.loop() below) are using at essentially the same time. Calling
  // them directly from the BLE thread races appTask's WiFi/MQTT connect calls
  // on that shared lwIP state with no serialization between the two threads -
  // exactly the class of bug behind an intermittent
  // "assert failed: udp_new_ip_type ... Required to lock TCPIP core
  // functionality!" crash seen during testing. Rather than call any of that
  // here, just record that settings changed; bleConfigDirty is only ever set
  // here and only ever read/cleared in appTask's for(;;) loop below, so the
  // actual WiFi/MQTT/SNTP work stays entirely on appTask's own thread, same
  // as every other path that touches it.
  static volatile bool bleConfigDirty = false;
  static BleConfigService bleConfig;
  // settings.name is the BLE beacon name as well as the LAN hostname and
  // showStartupInfo()'s self-reference - see device_settings.h. A rename via
  // BLE (doc["name"] below) re-advertises live (BleConfigService::
  // renameDevice(), called from bleConfigDirty handling below) rather than
  // needing a reboot.
  bleConfig.begin(
      settings.name.c_str(),
      [](const JsonDocument &doc) {
        settings.applyAndSave(doc);
        bleConfigDirty = true;
        Serial.println("BLE: settings updated, reconnecting");
      },
      [](const uint8_t *data, size_t len) {
        mqttLink.injectMessage(data, len);
      },
      [](JsonDocument &doc) {
        // pass_ intentionally omitted from reads - write-only over BLE.
        doc["name"] = settings.name;
        doc["ssid"] = settings.ssid;
        doc["host"] = settings.host;
        doc["port"] = settings.port;
        doc["topic"] = settings.topic;
        doc["tz"] = settings.tz;
        doc["holdSeconds"] = settings.messageHoldSeconds;
        doc["keepLast"] = settings.keepLastMessage;
        doc["textSize"] = settings.messageTextSize;
      });

  // Records the BLE identity for the "what am I connected to" summary
  // (MqttLink::showStartupInfo()) rather than showing it here automatically -
  // see the correction above showStartupInfo() for why: it now only appears
  // on demand, from a button click while idle and before any message has
  // arrived (see MqttLink::onButtonClick()).
  mqttLink.setBleIdentity(settings.name,
                           NimBLEDevice::getAddress().toString().c_str());

  // Multifunction button (see BUTTON_PIN above, and `button`'s declaration
  // near the top of this function). A single click dismisses whatever
  // message is on screen, or replays the last one if nothing is - see
  // MqttLink::onButtonClick(). Double-click/long-press have no action wired
  // up yet, so those two just log for now, to confirm timing over serial
  // before deciding what they should do.
  button.attachClick([]() { mqttLink.onButtonClick(); });
  button.attachDoubleClick([]() { Serial.println("Button: double-click"); });
  button.attachLongPressStart(
      []() { Serial.println("Button: long-press start"); });

#ifdef AVATAR_FB_DUMP
  // Headless bench test: exercise the parse-and-display path with a
  // synthetic payload, no live broker required — same "read back what was
  // actually rasterised" technique as dumpFramebuffer/dumpBubbleFramebuffer.
  mqttLink.injectForTest(
      "{\"text\":\"Bench test, no broker needed.\",\"expression\":\"Happy\"}");
  dumpFramebuffer("MQTT bench test");
  dumpBubbleFramebuffer("MQTT bench test");
#endif

  uint32_t lastStatusPush = 0;
  for (;;) {
    // Apply a pending BLE config write here, on appTask's own thread, before
    // touching WiFi/MQTT below - see the comment above bleConfig.begin() for
    // why this can't happen directly on the BLE callback's thread.
    if (bleConfigDirty) {
      bleConfigDirty = false;
      // Renaming the BLE beacon is BLE-only work (see renameDevice()'s
      // comment), so it's fine to do this unconditionally here rather than
      // tracking whether "name" specifically was in the write - same as
      // reconfigure() below always re-applying ssid/host/etc. regardless of
      // which one changed. mqttLink.setDeviceName() must precede
      // reconfigure(): reconfigure() forces a WiFi reconnect, and
      // connectWiFi() reads the new hostname off deviceName_ right away.
      mqttLink.setDeviceName(settings.name);
      bleConfig.renameDevice(settings.name);
      mqttLink.reconfigure(settings.ssid, settings.pass, settings.host,
                            settings.port, settings.topic);
      mqttLink.setDisplayOptions(settings.messageHoldSeconds * 1000UL,
                                  settings.keepLastMessage);
      if (bubble != nullptr) bubble->setTextSize(settings.messageTextSize);
      if (idleClock != nullptr) idleClock->setTimezone(settings.tz);
    }
    mqttLink.loop();
    // display() only ever runs synchronously inside mqttLink.loop() above,
    // on this same appTask thread — MQTT messages and BLE test messages
    // (BleConfigService's test-message write lands on the NimBLE host task,
    // a different thread, but injectMessage() queues rather than displaying
    // straight away; see mqtt_link.h's enqueue()/popQueued()) both funnel
    // through here. So this never races a message reveal for the oledText
    // panel — by the time control gets here the bubble is either idle or
    // just went idle. Skipped while isHoldingMessage() - both draw to the
    // same oledText panel, so an untouched tick() would redraw the clock over
    // a keepLastMessage_ message the moment the wall-clock second changes,
    // undoing it within about a second of it finishing typing.
    if (idleClock != nullptr && !mqttLink.isHoldingMessage()) idleClock->tick();
    // Idle "breathing" sway for the announcer - no-op while a message is
    // typing (portraitFace_->talking_, set by lipSyncTask) or the face is
    // asleep for the night, both tracked inside PortraitFace itself; see
    // portrait_face.h's tick().
    if (portraitFace != nullptr) portraitFace->tick(millis());
    // OneButton needs frequent polling to time clicks/long-presses; the 50ms
    // period below is well inside its default click/press timing windows.
    button.tick();
    // Refresh the BLE status characteristic every couple seconds rather than
    // every 50ms tick - it's read-on-demand by the client, not notified.
    uint32_t now = millis();
    if (now - lastStatusPush > 2000) {
      bleConfig.setStatus(mqttLink.statusString());
      lastStatusPush = now;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
#endif
}

#ifdef RGB_LED_TEST
void setup() {
  Serial.begin(115200);
  ledcAttach(RGB_R_PIN, kRgbPwmFreq, kRgbPwmRes);
  ledcAttach(RGB_G_PIN, kRgbPwmFreq, kRgbPwmRes);
  ledcAttach(RGB_B_PIN, kRgbPwmFreq, kRgbPwmRes);
}

void loop() {
  // Red, green, blue, off - confirms each channel/resistor/leg is wired to
  // the color it's supposed to be, one at a time.
  Serial.println("red");
  rgbSetColor(255, 0, 0);
  delay(700);
  Serial.println("green");
  rgbSetColor(0, 255, 0);
  delay(700);
  Serial.println("blue");
  rgbSetColor(0, 0, 255);
  delay(700);
  Serial.println("off");
  rgbSetColor(0, 0, 0);
  delay(700);

  // Then sweep the full spectrum, to confirm PWM mixing works smoothly
  // across all three channels rather than just full-on/full-off.
  Serial.println("spectrum sweep");
  for (int deg = 0; deg < 360; deg++) {
    uint8_t r, g, b;
    hsvToRgb((float)deg, r, g, b);
    rgbSetColor(r, g, b);
    delay(15);
  }
  rgbSetColor(0, 0, 0);
  delay(700);
}
#else
void setup() {
  Serial.begin(115200);
  delay(200);

  // Needed ahead of the POST beep below; everything else that used to set
  // this pin up happens later, alongside the real `oledText` panel init.
  pinMode(PIEZO_PIN, OUTPUT);

  // Loaded here rather than in appTask (which used to do this): NVS/
  // Preferences needs no WiFi/BLE/OLED and is safe this early, and the boot
  // POST screen below wants these values on screen well before appTask
  // starts. secrets.h values are only the first-boot defaults; once BLE
  // config saves anything, NVS wins from then on (see device_settings.h).
  settings.load(kDefaultDeviceName, WIFI_SSID, WIFI_PASS, MQTT_HOST,
                MQTT_PORT, MQTT_TOPIC, DigitalClock::kDefaultTz,
                /*defaultMessageHoldSeconds=*/30,
                /*defaultKeepLastMessage=*/false,
                /*defaultMessageTextSize=*/1);

  // HLI boot POST + splash intro, played once before M5Unified/LGFX claims
  // either panel — POST (scrolling console text) first, alone, then splash
  // (the flashy full-frame graphic) alone once POST is done. (Swapped
  // 2026-09-16 from the original splash-on-top/POST-on-bottom pairing —
  // panel choice turned out to be a taste call either way, not something the
  // hand-off below depends on.)
  //
  // Correction, 2026-09-15: this used to run both sequences concurrently, in
  // one shared while loop — see the correction above u8g2Top/u8g2Bottom's
  // declarations for why that made both of them look janky, not just the
  // slower panel. The two phases below are now fully separate: whichever
  // panel isn't currently playing is blanked and left alone rather than
  // drawn to every frame for no reason.
  //
  // Since the 2026-09-15 physical-panel-reversal pin swap (see the comment
  // above `oled`/`oledText`'s declarations), u8g2Top (POST) is the one on
  // GPIO32/33 and u8g2Bottom (splash) is the one on GPIO4/15 — the opposite
  // of how this used to read. u8g2Top claims Wire1 (I2C_NUM_1) for its
  // phase instead of bit-banging (see the comment above its declaration);
  // u8g2Bottom uses the global Wire (I2C_NUM_0) it always has. Both get
  // released below, before M5.begin(), so the panels' real runtime owners
  // start from a clean bus: Wire.end() for oledText (GPIO4/15, LGFX
  // hardware I2C_NUM_1 — a different peripheral number, but released the
  // same way as good hygiene) and Wire1.end() for oled (GPIO32/33, LGFX's
  // own bit-banged path — this one matters, since a hardware peripheral
  // left attached to those pins would otherwise still be driving them
  // alongside oled's plain digitalWrite() bit-banging).
  Wire1.begin(OLED_TEXT_SDA, OLED_TEXT_SCL, 400000);
  u8g2Top.setBusClock(400000);  // must precede begin()
  u8g2Top.begin();
  u8g2Top.setContrast(255);
  u8g2Bottom.setBusClock(400000);
  u8g2Bottom.begin();
  u8g2Bottom.setContrast(255);

  // Blank both panels immediately after begin(), before either script draws
  // its first frame. begin() only sends the SSD1306 its init command
  // sequence - it does not touch GDDRAM - so a chip that stayed powered
  // through a software reset (upload, a soft reboot, anything short of an
  // actual power cycle) is still showing whatever was there last session
  // until something clears it. Corrected 2026-09-19: this used to only
  // cover u8g2Bottom, right below, on the reasoning that boot::draw()'s own
  // first-frame clearBuffer()+sendBuffer() (see boot.cpp) would blank
  // u8g2Top itself within its first ~16ms iteration anyway - true for the
  // *shadow buffer* M5GFX/U8g2 think they're showing, but this comment's
  // "restart from software" report is about the *physical* screen, which
  // this code has no way to read back and confirm empty. Blanking both here
  // removes the dependency on that first-frame timing entirely rather than
  // trusting it.
  u8g2Top.clearBuffer();
  u8g2Top.sendBuffer();
  u8g2Bottom.clearBuffer();
  u8g2Bottom.sendBuffer();

  // Beat to hold on each phase's settled last frame before handing off, so
  // POST's cursor and splash's landed title card are actually seen rather
  // than the app cutting away the instant each script ends.
  constexpr uint32_t PHASE_HOLD_MS = 2000;

  // Phase 1: POST alone, on u8g2Top.
  boot::begin(boot::Info{settings.name.c_str(), settings.ssid.c_str(),
                         settings.host.c_str(), settings.port,
                         settings.topic.c_str()});
  // Correction, 2026-09-16: this beep used to fire when boot::done() went
  // true, i.e. right as the self-test *finished* - modeled on the "self test
  // passed, handing off to the bootloader" chime AMI/Award BIOSes give at the
  // end of POST. Moved to the very start instead: the beep these old boot
  // screens are actually remembered for is the one that opens the self test,
  // not the one that closes it, so it now fires the instant boot::begin()
  // returns, before boot::draw() has run even once.
  //
  // Pitched up 2026-09-16 from 2000Hz - flat and dull for the one beep meant
  // to announce the machine switching on.
  tone(PIEZO_PIN, 3000, 150);
  bool bootDone = false;
  uint32_t bootDoneAt = 0;
  for (;;) {
    boot::draw(u8g2Top);
    if (!bootDone && boot::done()) {
      bootDone = true;
      bootDoneAt = millis();
    }
    if (bootDone && millis() - bootDoneAt >= PHASE_HOLD_MS) break;
    delay(16);
  }

  // Hand off: blank u8g2Top before splash claims the other panel, so POST's
  // frozen last frame isn't still sitting there through the whole next phase.
  u8g2Top.clearBuffer();
  u8g2Top.sendBuffer();

  // Phase 2: splash alone, on u8g2Bottom, with its synthwave riff on the
  // piezo (splash::playTheme() — see splash.cpp for the "retro cassette"
  // arpeggio this plays). Stops at the end of its one-shot intro plus the
  // same hold beat; the attract loop (kicker lines cycling, scan bar
  // sweeping) never gets to run here; there's nothing left to attract
  // anyone to before the app boots into real mode.
  splash::begin();
  bool splashDone = false;
  uint32_t splashDoneAt = 0;
  for (;;) {
    splash::draw(u8g2Bottom);
    splash::playTheme(PIEZO_PIN);
    if (!splashDone && !splash::intro()) {
      splashDone = true;
      splashDoneAt = millis();
    }
    if (splashDone && millis() - splashDoneAt >= PHASE_HOLD_MS) break;
    delay(16);
  }
  u8g2Bottom.clearBuffer();
  u8g2Bottom.sendBuffer();

  Wire.end();
  Wire1.end();

  auto cfg = M5.config();
  // Nothing else is on this board, so skip the probes. external_display_value
  // in particular defaults to 0xFFFF, which scans PortA I2C for M5 display
  // units we do not have.
  cfg.external_display_value = 0;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;  // flip to true if an I2S speaker is added for TTS
  M5.begin(cfg);

  // Order matters: M5.begin() runs M5GFX board autodetect, which probes SPI
  // pins (GPIO15 among them) looking for M5Stack panels. Initialising a panel
  // afterwards re-owns whatever pins it uses and sends the SSD1306 its full
  // reset sequence, so whatever autodetect left behind is discarded. Doing it
  // in the other order leaves the panel blank. Since the 2026-09-15 pin swap
  // above, GPIO15 is oledText's SCL, not oled's (oled is now on GPIO33,
  // which autodetect doesn't probe) — the concern this comment describes now
  // applies to oledText.init() below, not this call, but both already run
  // after M5.begin() so the ordering requirement is satisfied either way.
  if (!oled.init()) {
    Serial.println("SSD1306 init failed - check wiring on SDA=32 SCL=33 @0x3C");
    while (true) delay(1000);
  }

  // Make the OLED M5.Display/M5.Lcd, which is what PortraitFace draws to
  // below. addDisplay() makes it primary automatically when it is the first
  // display, but autodetect may have registered one, so be explicit.
  const size_t idx = M5.addDisplay(oled);
  M5.setPrimaryDisplay(idx);
  // Rotation 2 = 180 degrees. Panel is mounted upside down relative to the
  // SSD1306's native origin, so the face renders inverted at rotation 0.
  M5.Display.setRotation(2);
  M5.Display.fillScreen(TFT_BLACK);

  // Second panel: the speech bubble. Not made primary, so the face panel
  // never touches it — accessed only through `bubble` below. If it's
  // missing or at the wrong address this just logs and carries on without
  // it; the face half of the demo doesn't depend on it. (PIEZO_PIN's
  // pinMode() already happened at the top of setup(), ahead of the
  // boot-POST beep.)
  if (oledText.init()) {
    M5.addDisplay(oledText);
    oledText.setRotation(2);
    // Explicit, same as M5.Display.fillScreen() above for `oled` - init()'s
    // own use_clear default should already cover this, but `oled` gets the
    // belt-and-suspenders redraw and this panel didn't, which was the one
    // asymmetry found while chasing the "screen still shows artifacts from
    // the previous session after a software restart" report (2026-09-19).
    oledText.fillScreen(TFT_BLACK);
    bubble = new SpeechBubble(&oledText, beepChar);
    bubble->setTextSize(settings.messageTextSize);
    idleClock = new DigitalClock(&oledText);
    // Not idleClock->begin() here - see digital_clock.h's begin() comment
    // for why starting SNTP this early (well before WiFi.begin() ever runs)
    // is implicated in the intermittent MQTT-connect lwIP crash. No
    // showNow() call either - draw() now shows nothing at all while unsynced
    // (see digital_clock.h's 2026-09-16 correction), and the fillScreen()
    // just above already leaves the panel blank, so there's nothing an
    // early call would put on screen that isn't already there. appTask's
    // real idleClock->setTimezone() call, after WiFi/MQTT connect, is what
    // actually starts SNTP.
  } else {
    Serial.println(
        "Second SSD1306 (text) init failed - check wiring on SDA=4 "
        "SCL=15 @0x3C");
  }

  // Picks the first random announcer and draws it immediately - see
  // portrait_face.h.
  portraitFace = new PortraitFace(&oled);

  // RGB status LED: attaches its three LEDC channels and leaves it off until
  // a message asks for a color (see mqtt_link.h's display()). Independent of
  // both OLEDs above, so it's fine to bring up regardless of whether either
  // panel's init() succeeded.
  rgbLed.begin();

  // Pin app logic to core 0 - see the file-header comment. 8192 rather than
  // the demo's old 4096: WiFi, PubSubClient and ArduinoJson buffers now
  // share this stack, and the old size overflows silently once networking
  // is in the mix.
  xTaskCreatePinnedToCore(appTask, "appTask", 8192, nullptr, 1, nullptr,
                          PRO_CPU_NUM);
}

void loop() {
  // Intentionally empty. Arduino's loopTask runs on core 1; app work goes in
  // appTask on core 0 instead - see the file-header comment.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
#endif  // RGB_LED_TEST
