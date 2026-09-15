// stack-chan/m5stack-avatar on a bare ESP32-WROOM + SSD1306 128x64 OLED.
//
// Core split (the point of this demo):
//   Core 1 (APP_CPU) - owned entirely by the avatar. m5stack-avatar hardcodes
//                      both of its FreeRTOS tasks to APP_CPU_NUM in
//                      Avatar::start() (Avatar.cpp), so rendering and the
//                      blink/saccade/breath state machine are already pinned
//                      there. Arduino's loop() also lands on core 1, so it is
//                      deliberately left empty below.
//   Core 0 (PRO_CPU) - free for application work. Everything this demo does
//                      to the avatar happens in appTask, pinned to core 0.
//                      This is the core to hang TTS, wake-word, an LLM client
//                      or network I/O off; WiFi/BT stacks also live here.
//
// Talking to the avatar across cores is safe for the calls used here:
// Avatar::setExpression() suspends the draw task before mutating, and the
// float setters are single-word writes the draw task only reads.

#include <Arduino.h>
#include <M5Unified.h>
#include <Avatar.h>

#include "ssd1306_display.h"
#include "small_oled_face.h"
#include "speech_bubble.h"
#include "digital_clock.h"
#include "mqtt_link.h"
#include "device_settings.h"
#include "ble_config.h"
#include "secrets.h"

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

static SSD1306Display oled(OLED_SDA, OLED_SCL);
static SSD1306Display oledText(OLED_TEXT_SDA, OLED_TEXT_SCL, 400000,
                                OLED_TEXT_I2C_PORT, 0x3C);
static m5avatar::Avatar avatar;
static SpeechBubble *bubble = nullptr;
// The bubble panel's idle/default face — MqttLink switches back to this once
// a message has finished displaying (see mqtt_link.h's handleMessage()).
// Named idleClock, not clock: that shadows <time.h>'s clock() at global scope
// and fails to compile.
static DigitalClock *idleClock = nullptr;

// tone() is non-blocking (LEDC-driven), so this is safe to call from inside
// SpeechBubble::show()'s per-character reveal loop without slowing it down.
static void beepChar() { tone(PIEZO_PIN, 1800, 15); }

static const char *const kExpressionNames[] = {"Happy",  "Angry",  "Sad",
                                               "Doubt",  "Sleepy", "Neutral"};
static const m5avatar::Expression kExpressions[] = {
    m5avatar::Expression::Happy,  m5avatar::Expression::Angry,
    m5avatar::Expression::Sad,    m5avatar::Expression::Doubt,
    m5avatar::Expression::Sleepy, m5avatar::Expression::Neutral};
static constexpr size_t kExpressionCount =
    sizeof(kExpressions) / sizeof(kExpressions[0]);

#ifdef AVATAR_DEMO_MODE
// One line per expression above, in the same order — what the bubble shows
// while that expression is active. Placeholder for real TTS text; nihilistic
// one-liners for now because a demo bubble should at least be funny. Only
// used by the self-cycling demo loop below — real messages come from MQTT.
static const char *const kPhrases[] = {
    "Smile. The void won't notice.", "Scream away. Nothing's listening.",
    "Cheer up - nothing matters.",    "No one's driving. Never was.",
    "Rest easy. Heat death can wait.", "Just meat, doing meat things."};

// Fake lip-sync: drives mouthOpenRatio from a noisy envelope for `ms`.
// Stand-in for a real TTS/mic amplitude source — this is the hook a
// text-to-speech engine on core 0 would drive instead.
static void speakFor(uint32_t ms) {
  const uint32_t until = millis() + ms;
  while (millis() < until) {
    float openRatio = (random(0, 100) / 100.0f);
    avatar.setMouthOpenRatio(openRatio);
    vTaskDelay(pdMS_TO_TICKS(random(60, 140)));
  }
  avatar.setMouthOpenRatio(0.0f);
}
#endif

#ifdef AVATAR_FB_DUMP
// Debug aid for a headless bench: dump what is actually in the panel's
// framebuffer as ASCII, since Panel_HasBuffer keeps a readable RAM copy.
// Build with -DAVATAR_FB_DUMP to enable. Costs nothing when off.
static void dumpFramebuffer(const char *label) {
  avatar.suspend();  // freeze the draw task so we read a whole frame
  delay(120);
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
  avatar.resume();
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
    const m5avatar::Expression exp = kExpressions[i % kExpressionCount];
    avatar.setExpression(exp);
    if (bubble != nullptr) {
      bubble->show(kPhrases[i % kExpressionCount]);
    }

    Serial.printf("[core %d] expression=%-8s heap=%6u draw=core %d\n",
                  xPortGetCoreID(), kExpressionNames[i % kExpressionCount],
                  (unsigned)ESP.getFreeHeap(), APP_CPU_NUM);

#ifdef AVATAR_FB_DUMP
    delay(400);
    dumpFramebuffer(kExpressionNames[i % kExpressionCount]);
    dumpBubbleFramebuffer(kExpressionNames[i % kExpressionCount]);
#endif

    // Every third expression, babble for a bit so the mouth animates.
    if (i % 3 == 2) {
      speakFor(1500);
      vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(2500));
    }
    i++;
  }
}
#endif

// Everything here runs on core 0 while the avatar renders on core 1 (see the
// core-split comment at the top of this file).
static void appTask(void *) {
#ifdef AVATAR_DEMO_MODE
  demoLoop();
#else
  // secrets.h values are only the first-boot defaults; once BLE config saves
  // anything, NVS wins from then on (see device_settings.h). The tz/hold/
  // keepLast defaults come from DigitalClock/this file rather than secrets.h
  // since they aren't secrets, just first-boot behavior.
  static DeviceSettings settings;
  settings.load(WIFI_SSID, WIFI_PASS, MQTT_HOST, MQTT_PORT, MQTT_TOPIC,
                DigitalClock::kDefaultTz, /*defaultMessageHoldSeconds=*/30,
                /*defaultKeepLastMessage=*/false);

  static MqttLink mqttLink(&avatar, bubble, kExpressionNames, kExpressions,
                            kExpressionCount, idleClock);
  mqttLink.begin(settings.ssid, settings.pass, settings.host, settings.port,
                 settings.topic);
  mqttLink.setDisplayOptions(settings.messageHoldSeconds * 1000UL,
                              settings.keepLastMessage);
  if (idleClock != nullptr) idleClock->setTimezone(settings.tz);

  // BLE config: a phone (nRF Connect, LightBlue, ...) can connect, read/write
  // WiFi+MQTT+display settings, and push a one-off test message through the
  // same {"text":...,"expression":...} JSON path MQTT uses — see
  // ble_config.h.
  static BleConfigService bleConfig;
  bleConfig.begin(
      "MqttChan-Config",
      [](const JsonDocument &doc) {
        settings.applyAndSave(doc);
        mqttLink.reconfigure(settings.ssid, settings.pass, settings.host,
                              settings.port, settings.topic);
        mqttLink.setDisplayOptions(settings.messageHoldSeconds * 1000UL,
                                    settings.keepLastMessage);
        if (idleClock != nullptr) idleClock->setTimezone(settings.tz);
        Serial.println("BLE: settings updated, reconnecting");
      },
      [](const uint8_t *data, size_t len) {
        mqttLink.injectMessage(data, len);
      },
      [](JsonDocument &doc) {
        // pass_ intentionally omitted from reads - write-only over BLE.
        doc["ssid"] = settings.ssid;
        doc["host"] = settings.host;
        doc["port"] = settings.port;
        doc["topic"] = settings.topic;
        doc["tz"] = settings.tz;
        doc["holdSeconds"] = settings.messageHoldSeconds;
        doc["keepLast"] = settings.keepLastMessage;
      });

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
    mqttLink.loop();
    // handleMessage() runs synchronously inside mqttLink.loop() above, so
    // this never races a message reveal for the oledText panel — by the
    // time control gets here the bubble is either idle or just went idle.
    if (idleClock != nullptr) idleClock->tick();
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

void setup() {
  Serial.begin(115200);
  delay(200);

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
  // pins (GPIO15 among them, which is our SCL) looking for M5Stack panels.
  // Initialising the OLED afterwards re-owns those pins and sends the SSD1306
  // its full reset sequence, so whatever autodetect left behind is discarded.
  // Doing it in the other order leaves the panel blank.
  if (!oled.init()) {
    Serial.println("SSD1306 init failed - check wiring on SDA=4 SCL=15 @0x3C");
    while (true) delay(1000);
  }

  // Make the OLED M5.Display/M5.Lcd, which is the surface m5stack-avatar
  // draws to. addDisplay() makes it primary automatically when it is the
  // first display, but autodetect may have registered one, so be explicit.
  const size_t idx = M5.addDisplay(oled);
  M5.setPrimaryDisplay(idx);
  // Rotation 2 = 180 degrees. Panel is mounted upside down relative to the
  // SSD1306's native origin, so the avatar renders inverted at rotation 0.
  M5.Display.setRotation(2);
  M5.Display.fillScreen(TFT_BLACK);

  // Second panel: the speech bubble. Not made primary, so the avatar never
  // touches it — accessed only through `bubble` below. If it's missing or at
  // the wrong address this just logs and carries on without it; the avatar
  // half of the demo doesn't depend on it.
  pinMode(PIEZO_PIN, OUTPUT);

  if (oledText.init()) {
    M5.addDisplay(oledText);
    oledText.setRotation(2);
    bubble = new SpeechBubble(&oledText, beepChar);
    idleClock = new DigitalClock(&oledText);
    idleClock->begin();
    idleClock->showNow();
  } else {
    Serial.println(
        "Second SSD1306 (text) init failed - check wiring on SDA=32 "
        "SCL=33 @0x3C");
  }

  avatar.setFace(new m5avatar::SmallOledFace());
  // colorDepth 1: render into a 1-bit sprite. Anything else wastes RAM and
  // gets flattened by the panel anyway.
  avatar.init(1);

  // Pin app logic to core 0, leaving core 1 to the avatar's own tasks. 8192
  // rather than the demo's old 4096: WiFi, PubSubClient and ArduinoJson
  // buffers now share this stack, and the old size overflows silently once
  // networking is in the mix.
  xTaskCreatePinnedToCore(appTask, "appTask", 8192, nullptr, 1, nullptr,
                          PRO_CPU_NUM);
}

void loop() {
  // Intentionally empty. Arduino's loopTask runs on core 1, which belongs to
  // the avatar; app work goes in appTask on core 0.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
