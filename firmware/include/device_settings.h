// Runtime WiFi/MQTT settings, backed by NVS (Preferences) so values written
// over BLE (see ble_config.h) survive a reboot. secrets.h's compile-time
// constants are only the first-boot defaults — once BLE config saves
// anything, NVS wins from then on regardless of what secrets.h says.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

struct DeviceSettings {
  String name;  // affectionate device name - BLE beacon, LAN hostname, self-references
  String ssid;
  String pass;
  String host;
  uint16_t port;
  String topic;
  String tz;                    // POSIX TZ string, e.g. DigitalClock::kDefaultTz
  uint16_t messageHoldSeconds;  // see MqttLink::setDisplayOptions()
  bool keepLastMessage;         // see MqttLink::setDisplayOptions()
  uint8_t messageTextSize;      // see SpeechBubble::setTextSize() - 1 (Small,
                                 // the original/default GLCD size) or 2 (Large)

  void load(const char *defaultName, const char *defaultSsid,
            const char *defaultPass, const char *defaultHost,
            uint16_t defaultPort, const char *defaultTopic,
            const char *defaultTz, uint16_t defaultMessageHoldSeconds,
            bool defaultKeepLastMessage, uint8_t defaultMessageTextSize) {
    Preferences prefs;
    prefs.begin(kNamespace, /*readOnly=*/true);
    name = prefs.getString("name", defaultName);
    ssid = prefs.getString("ssid", defaultSsid);
    pass = prefs.getString("pass", defaultPass);
    host = prefs.getString("host", defaultHost);
    port = prefs.getUShort("port", defaultPort);
    topic = prefs.getString("topic", defaultTopic);
    tz = prefs.getString("tz", defaultTz);
    messageHoldSeconds =
        prefs.getUShort("holdSeconds", defaultMessageHoldSeconds);
    keepLastMessage = prefs.getBool("keepLast", defaultKeepLastMessage);
    messageTextSize = prefs.getUChar("textSize", defaultMessageTextSize);
    prefs.end();
  }

  // Merges whichever fields are present in doc onto the current settings and
  // persists just those — a BLE config write is a partial update, not a full
  // replacement, so a client can e.g. send {"topic":"..."} alone.
  void applyAndSave(const JsonDocument &doc) {
    Preferences prefs;
    prefs.begin(kNamespace, /*readOnly=*/false);
    if (doc["name"].is<const char *>()) {
      name = doc["name"].as<const char *>();
      prefs.putString("name", name);
    }
    if (doc["ssid"].is<const char *>()) {
      ssid = doc["ssid"].as<const char *>();
      prefs.putString("ssid", ssid);
    }
    if (doc["pass"].is<const char *>()) {
      pass = doc["pass"].as<const char *>();
      prefs.putString("pass", pass);
    }
    if (doc["host"].is<const char *>()) {
      host = doc["host"].as<const char *>();
      prefs.putString("host", host);
    }
    if (doc["port"].is<uint16_t>()) {
      port = doc["port"].as<uint16_t>();
      prefs.putUShort("port", port);
    }
    if (doc["topic"].is<const char *>()) {
      topic = doc["topic"].as<const char *>();
      prefs.putString("topic", topic);
    }
    if (doc["tz"].is<const char *>()) {
      tz = doc["tz"].as<const char *>();
      prefs.putString("tz", tz);
    }
    if (doc["holdSeconds"].is<uint16_t>()) {
      messageHoldSeconds = doc["holdSeconds"].as<uint16_t>();
      prefs.putUShort("holdSeconds", messageHoldSeconds);
    }
    if (doc["keepLast"].is<bool>()) {
      keepLastMessage = doc["keepLast"].as<bool>();
      prefs.putBool("keepLast", keepLastMessage);
    }
    if (doc["textSize"].is<uint8_t>()) {
      messageTextSize = doc["textSize"].as<uint8_t>();
      prefs.putUChar("textSize", messageTextSize);
    }
    prefs.end();
  }

 private:
  static constexpr const char *kNamespace = "avatar";
};
