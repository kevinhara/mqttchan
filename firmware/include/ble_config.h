// BLE GATT service for on-device configuration: a phone or BLE app (e.g.
// nRF Connect, LightBlue) can connect, read/write WiFi + MQTT settings, and
// push a one-off test message through the same {"text":...,"expression":...}
// JSON contract MQTT messages use (see mqtt_link.h's handleMessage) — handy
// for exercising the avatar without a broker.
//
// This class only owns the GATT plumbing; persisting settings (NVS) and
// applying them (WiFi/MQTT reconnect) are the caller's job via the
// callbacks passed to begin(). Advertising stays on for the device's whole
// life — this is a hobby config tool, not something worth gating behind a
// button/timeout.
//
// Built against h2zero/NimBLE-Arduino, not arduino-esp32's bundled
// BLEDevice.h. That bundled library links Bluedroid's dual-mode (classic
// BT + BLE) controller blob regardless of which half is actually used —
// CONFIG_BT_CLASSIC_ENABLED is baked into this core's prebuilt sdkconfig —
// which alone overflows the 128KB IRAM region once WiFi is also in the
// build. NimBLE is its own BLE-only stack: ~50% less flash, ~100KB less RAM,
// same functionality. Still shares the radio and heap with WiFi, on top of
// the buffers PubSubClient/ArduinoJson already need (see appTask's stack
// size comment in main.cpp), just with much more headroom to do it in.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <functional>

class BleConfigService {
 public:
  // Called with a parsed JSON doc whenever the config characteristic is
  // written. Only keys present in the doc were sent by the client — this is
  // a merge, not a full replacement (see DeviceSettings::applyAndSave).
  using ConfigCallback = std::function<void(const JsonDocument &)>;
  // Called with the raw bytes written to the test-message characteristic —
  // same wire format MQTT messages use, handed straight to
  // MqttLink::injectMessage.
  using TestMessageCallback = std::function<void(const uint8_t *, size_t)>;
  // Called just before a client reads the config characteristic, so reads
  // reflect the settings actually in effect rather than a stale snapshot.
  using ConfigReader = std::function<void(JsonDocument &)>;

  void begin(const char *deviceName, ConfigCallback onConfig,
             TestMessageCallback onTest, ConfigReader configReader) {
    onConfig_ = std::move(onConfig);
    onTest_ = std::move(onTest);
    configReader_ = std::move(configReader);

    NimBLEDevice::init(deviceName);
    NimBLEServer *server = NimBLEDevice::createServer();
    // m_advertiseOnDisconnect defaults to false, so without this the device
    // only ever advertises once at boot: after a client disconnects (e.g.
    // the browser tab closing or hitting Disconnect), NimBLE never resumes
    // advertising and the device becomes permanently invisible to a second
    // requestDevice() scan until the next reboot.
    server->advertiseOnDisconnect(true);
    NimBLEService *service = server->createService(kServiceUuid);

    configChar_ = service->createCharacteristic(
        kConfigCharUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    configChar_->setCallbacks(new ConfigCallbacks(this));
    configChar_->setValue("{}");

    testChar_ =
        service->createCharacteristic(kTestCharUuid, NIMBLE_PROPERTY::WRITE);
    testChar_->setCallbacks(new TestCallbacks(this));

    statusChar_ = service->createCharacteristic(kStatusCharUuid,
                                                 NIMBLE_PROPERTY::READ);
    statusChar_->setValue("booting");

    // No explicit service->start() with NimBLE - services go live as soon
    // as they're created (unlike the classic Bluedroid BLE library, where
    // that call was required).
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(kServiceUuid);
    advertising->enableScanResponse(true);
    // NimBLEDevice::init(deviceName) above only sets the GAP device-name
    // characteristic, readable after a connection - it does NOT put the name
    // in the advertisement/scan-response payload. Without this, the OS
    // Bluetooth picker (and Web Bluetooth's requestDevice() chooser) shows an
    // unnamed device / bare MAC address, not "MqttChan-Config" - effectively
    // making the device hard to find among other nearby BLE devices. setName()
    // must come after enableScanResponse(true): it writes into whichever of
    // advData/scanData is currently active, and the scan response is where a
    // full name usually fits (the primary advertising packet is only 31 bytes
    // and the service UUID above already uses a chunk of it).
    advertising->setName(deviceName);
    NimBLEDevice::startAdvertising();
    Serial.printf("BLE: advertising as \"%s\"\n", deviceName);
  }

  // Refreshes the read-only status characteristic, e.g. "wifi=up mqtt=up".
  // Call from appTask whenever connection state might have changed.
  void setStatus(const String &status) {
    if (statusChar_ != nullptr) statusChar_->setValue(status.c_str());
  }

  // Re-advertises under a new device name (the GAP name plus the
  // scan-response name set at begin()) without a reboot. Safe to call
  // directly from onConfig_'s own NimBLE host-task thread - unlike the
  // WiFi/MQTT/SNTP work reconfigure() triggers (see main.cpp's
  // bleConfigDirty comment), this never touches lwIP, only the BLE stack
  // it's already running on. Doesn't touch an already-open GATT connection;
  // a connected client keeps working under the old name until it
  // disconnects and rescans.
  void renameDevice(const String &name) {
    NimBLEDevice::setDeviceName(name.c_str());
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->setName(name.c_str());
    // NimBLE doesn't push advertisement-data changes to an already-running
    // advertiser - stop/start is what makes the new scan-response payload
    // actually go out.
    advertising->stop();
    advertising->start();
  }

 private:
  static constexpr const char *kServiceUuid =
      "9f2e0000-9b5f-4a2e-8b7a-9c9f6f5b1a10";
  static constexpr const char *kConfigCharUuid =
      "9f2e0001-9b5f-4a2e-8b7a-9c9f6f5b1a10";
  static constexpr const char *kTestCharUuid =
      "9f2e0002-9b5f-4a2e-8b7a-9c9f6f5b1a10";
  static constexpr const char *kStatusCharUuid =
      "9f2e0003-9b5f-4a2e-8b7a-9c9f6f5b1a10";

  class ConfigCallbacks : public NimBLECharacteristicCallbacks {
   public:
    explicit ConfigCallbacks(BleConfigService *owner) : owner_(owner) {}

    void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
      if (!owner_->configReader_) return;
      JsonDocument doc;
      owner_->configReader_(doc);
      String out;
      serializeJson(doc, out);
      chr->setValue(out.c_str());
    }

    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
      const auto &value = chr->getValue();
      if (value.length() == 0) return;
      JsonDocument doc;
      DeserializationError err = deserializeJson(
          doc, reinterpret_cast<const char *>(value.data()), value.length());
      if (err) {
        Serial.printf("BLE: bad config JSON (%s)\n", err.c_str());
        return;
      }
      if (owner_->onConfig_) owner_->onConfig_(doc);
    }

   private:
    BleConfigService *owner_;
  };

  class TestCallbacks : public NimBLECharacteristicCallbacks {
   public:
    explicit TestCallbacks(BleConfigService *owner) : owner_(owner) {}

    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
      const auto &value = chr->getValue();
      if (value.length() == 0 || !owner_->onTest_) return;
      owner_->onTest_(value.data(), value.length());
    }

   private:
    BleConfigService *owner_;
  };

  ConfigCallback onConfig_;
  TestMessageCallback onTest_;
  ConfigReader configReader_;
  NimBLECharacteristic *configChar_ = nullptr;
  NimBLECharacteristic *testChar_ = nullptr;
  NimBLECharacteristic *statusChar_ = nullptr;
};
