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
#include <string.h>

#include "speech_bubble.h"

class MqttLink {
 public:
  MqttLink(m5avatar::Avatar *avatar, SpeechBubble *bubble,
           const char *const *expressionNames,
           const m5avatar::Expression *expressions, size_t expressionCount)
      : avatar_(avatar),
        bubble_(bubble),
        names_(expressionNames),
        exprs_(expressions),
        count_(expressionCount),
        mqtt_(wifiClient_) {
    self_ = this;
  }

  void begin(const char *ssid, const char *pass, const char *host,
             uint16_t port, const char *topic) {
    ssid_ = ssid;
    pass_ = pass;
    topic_ = topic;
    mqtt_.setServer(host, port);
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
  }

  // Headless bench test (see AVATAR_FB_DUMP in main.cpp): exercises the
  // parse-and-display path with a synthetic payload, no live broker needed.
  void injectForTest(const char *json) {
    handleMessage(reinterpret_cast<const uint8_t *>(json), strlen(json));
  }

 private:
  static MqttLink *self_;
  static constexpr const char *kClientId = "avatar-demo";

  void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("WiFi: connecting...");
    WiFi.mode(WIFI_STA);
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
      mqtt_.subscribe(topic_);
      Serial.println("MQTT: connected and subscribed");
    } else {
      Serial.printf("MQTT: connect failed, rc=%d\n", mqtt_.state());
      vTaskDelay(pdMS_TO_TICKS(2000 + random(0, 1000)));  // jitter
    }
  }

  static void onMessage(char *topic, uint8_t *payload, unsigned int length) {
    if (self_ != nullptr) self_->handleMessage(payload, length);
  }

  void handleMessage(const uint8_t *payload, unsigned int length) {
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, reinterpret_cast<const char *>(payload), length);
    if (err) {
      Serial.printf("MQTT: bad JSON payload (%s)\n", err.c_str());
      return;
    }
    const char *text = doc["text"] | "";
    const char *exprName = doc["expression"] | "";

    m5avatar::Expression expr = m5avatar::Expression::Neutral;
    bool matched = false;
    for (size_t i = 0; i < count_; i++) {
      if (strcmp(names_[i], exprName) == 0) {
        expr = exprs_[i];
        matched = true;
        break;
      }
    }
    if (!matched) {
      Serial.printf(
          "MQTT: unrecognized expression '%s', defaulting to Neutral\n",
          exprName);
    }

    avatar_->setExpression(expr);
    if (bubble_ != nullptr && text[0] != '\0') {
      bubble_->show(text);
    }
  }

  m5avatar::Avatar *avatar_;
  SpeechBubble *bubble_;
  const char *const *names_;
  const m5avatar::Expression *exprs_;
  size_t count_;

  const char *ssid_ = nullptr;
  const char *pass_ = nullptr;
  const char *topic_ = nullptr;

  WiFiClient wifiClient_;
  PubSubClient mqtt_;
};

MqttLink *MqttLink::self_ = nullptr;
