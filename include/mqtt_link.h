// Subscribes to a single MQTT topic carrying {"text":..., "expression":...}
// JSON and drives the avatar + speech bubble from whatever arrives. Owns
// WiFi and MQTT connect/reconnect; call begin() once from setup-time code
// and loop() repeatedly from appTask (core 0) — see main.cpp's core-split
// comment for why this must not run on core 1.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Avatar.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <deque>
#include <utility>

#include "speech_bubble.h"
#include "digital_clock.h"

class MqttLink {
 public:
  // clock, if given, is what the bubble panel reverts to once a message has
  // finished displaying (see display()) — nullptr just leaves the last
  // message on screen, the old behavior.
  MqttLink(m5avatar::Avatar *avatar, SpeechBubble *bubble,
           const char *const *expressionNames,
           const m5avatar::Expression *expressions, size_t expressionCount,
           DigitalClock *clock = nullptr)
      : avatar_(avatar),
        bubble_(bubble),
        names_(expressionNames),
        exprs_(expressions),
        count_(expressionCount),
        clock_(clock),
        mqtt_(wifiClient_) {
    self_ = this;
  }

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

  // True once display() has left a message up under keepLastMessage_ instead
  // of reverting to the idle clock. main.cpp's loop must skip idleClock->tick()
  // while this holds - tick() redraws the same oledText panel the bubble used,
  // so an untouched idle clock would otherwise overwrite the "kept" message
  // the moment the wall-clock second changes, undoing keepLastMessage_ within
  // about a second of it finishing typing.
  bool isHoldingMessage() const { return messageHeld_; }

  // One-off "what am I connected to" message, shown right after begin()'s
  // connect attempts settle so a glance at the bubble confirms the broker,
  // link state, network identity and BLE identity without needing a laptop.
  // Always holds for a fixed 10s regardless of setDisplayOptions()'s
  // holdMs_/keepLastMessage_ - those govern real MQTT messages, not this one
  // - then reverts to the idle clock the same way display() does. Only safe
  // to call before loop() starts pulling from the queue (same constraint as
  // injectForTest()/handleMessage() - see there), which is how main.cpp uses
  // it.
  //
  // bleName/bleAddress are handed in rather than read from a BLE object this
  // class doesn't own - main.cpp calls this after BleConfigService::begin(),
  // passing the same device name it advertised and NimBLEDevice::getAddress()
  // (see main.cpp's appTask). WiFi's hostname is set here, in connectWiFi(),
  // rather than read back from anywhere else, since kHostname is this
  // class's own constant either way.
  void showStartupInfo(const String &bleName, const String &bleAddress) {
    if (bubble_ == nullptr) return;
    String text = String("MQTT ") + host_ + ":" + port_ + " - " +
                  (mqtt_.connected() ? "online" : "offline") + " | " +
                  kHostname + " " + WiFi.localIP().toString() + " | BLE " +
                  bleName + " " + bleAddress;
    bubble_->show(text.c_str());
    bubble_->holdWithCountdown(10000);
    if (clock_ != nullptr) clock_->showNow();
  }

 private:
  static MqttLink *self_;
  static constexpr const char *kClientId = "avatar-demo";
  // Advertised to the router/DHCP and shown on the startup screen above, so
  // the device is identifiable in a DHCP lease list or `ping` without first
  // opening BLE. Set in connectWiFi(), ahead of WiFi.begin() - ESP32's
  // WiFi.setHostname() only takes effect if called before the connection is
  // established.
  static constexpr const char *kHostname = "mqttchan";
  // First-boot default for holdMs_ below - see DeviceSettings::load().
  static constexpr uint32_t kDefaultMessageHoldMs = 30000;

  void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("WiFi: connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(kHostname);  // must precede begin() to take effect
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
    String exprName;
  };

  static void onMessage(char *topic, uint8_t *payload, unsigned int length) {
    if (self_ != nullptr) self_->enqueue(payload, length);
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
    out->exprName = doc["expression"] | "";
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
    m5avatar::Expression expr = m5avatar::Expression::Neutral;
    bool matched = false;
    for (size_t i = 0; i < count_; i++) {
      if (strcmp(names_[i], msg.exprName.c_str()) == 0) {
        expr = exprs_[i];
        matched = true;
        break;
      }
    }
    if (!matched) {
      Serial.printf(
          "MQTT: unrecognized expression '%s', defaulting to Neutral\n",
          msg.exprName.c_str());
    }

    avatar_->setExpression(expr);
    // A new message is about to take over the panel, so any previously held
    // message is no longer what's on screen - reset before the keepLastMessage_
    // branch below decides whether this one ends up held too.
    messageHeld_ = false;
    if (bubble_ != nullptr && msg.text.length() > 0) {
      lipSyncActive_ = true;
      xTaskCreatePinnedToCore(lipSyncTask, "lipSync", 2048, this, 1, nullptr,
                              PRO_CPU_NUM);
      bubble_->show(msg.text.c_str());
      lipSyncActive_ = false;
      bubble_->drawQueueIndicator(remainingQueued);

      if (keepLastMessage_) {
        // Configured to leave the last message up indefinitely (see
        // setDisplayOptions()) - skip the hold/countdown and the revert to
        // the idle clock below; the face and bubble stay exactly as set
        // until the next message arrives. messageHeld_ tells main.cpp's loop
        // to stop calling idleClock->tick(), which would otherwise draw over
        // this same panel the moment the wall-clock second changes.
        messageHeld_ = true;
        return;
      }

      // Hold the message on screen for a bit (with a shrinking countdown bar
      // along the bottom) before handing the face and bubble panel back to
      // their idle look, rather than reverting the instant typing finishes.
      bubble_->holdWithCountdown(holdMs_);
      avatar_->setExpression(m5avatar::Expression::Neutral);
      if (clock_ != nullptr) clock_->showNow();
    }
  }

  // Fake lip-sync, same noisy-envelope trick as main.cpp's demo-mode
  // speakFor(): drives mouthOpenRatio while bubble_->show() reveals the
  // text on the caller's thread, so the mouth flaps for as long as the
  // avatar is "talking". Runs as its own task since show() blocks the
  // thread that would otherwise drive this loop; setMouthOpenRatio() is a
  // single-word write the draw task only reads, so calling it from here is
  // safe per the core-split comment in main.cpp. Exits and closes the mouth
  // once display() clears lipSyncActive_ after show() returns.
  static void lipSyncTask(void *arg) {
    auto *self = static_cast<MqttLink *>(arg);
    while (self->lipSyncActive_) {
      self->avatar_->setMouthOpenRatio(random(0, 100) / 100.0f);
      vTaskDelay(pdMS_TO_TICKS(random(60, 140)));
    }
    self->avatar_->setMouthOpenRatio(0.0f);
    vTaskDelete(nullptr);
  }

  volatile bool lipSyncActive_ = false;

  // Set by setDisplayOptions(), read by display() - see there for behavior.
  uint32_t holdMs_ = kDefaultMessageHoldMs;
  bool keepLastMessage_ = false;
  // Set by display() when keepLastMessage_ leaves a message up instead of
  // reverting to the idle clock; read by isHoldingMessage(). See there.
  bool messageHeld_ = false;

  // Messages waiting to be displayed - pushed by enqueue() (from either the
  // MQTT callback on appTask or a BLE test-message write on the NimBLE host
  // task) and popped by loop() on appTask - see popQueued(). Not touched by
  // injectForTest, which displays straight away (see handleMessage()).
  std::deque<PendingMessage> queue_;
  // Guards queue_ since enqueue()/popQueued()/queuedCount() can be called
  // from two different FreeRTOS tasks (appTask and the NimBLE host task) -
  // see enqueue()'s comment.
  SemaphoreHandle_t queueMutex_ = xSemaphoreCreateMutex();

  m5avatar::Avatar *avatar_;
  SpeechBubble *bubble_;
  const char *const *names_;
  const m5avatar::Expression *exprs_;
  size_t count_;
  DigitalClock *clock_;

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
