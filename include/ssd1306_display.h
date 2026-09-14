// An M5GFX device for a plain SSD1306 128x64 I2C OLED.
//
// Why this file exists: m5stack-avatar draws unconditionally into `M5.Lcd` /
// `M5.Display` (see Face.cpp), so the only way to put the avatar on a
// non-M5 panel is to make M5Unified's primary display *be* that panel.
// M5GFX ships lgfx::Panel_SSD1306 but no device wrapper for it — only
// M5UnitOLED, which is an SH110x 64x128. This is that wrapper, built to the
// same pattern as M5GFX's own M5UnitOLED.h (init_impl allocating panel+bus,
// handing ownership to _panel_last/_bus_last).
#pragma once

#include <M5GFX.h>

#include "lgfx/v1/panel/Panel_SSD1306.hpp"

class SSD1306Display : public M5GFX {
  lgfx::Bus_I2C::config_t _bus_cfg;

 public:
  // i2c_port defaults to 1 deliberately. M5Unified's own begin() claims port 0
  // for internal/PortA devices on GPIO21/22; sharing the port would mean two
  // drivers reconfiguring the same peripheral with different pins.
  SSD1306Display(uint8_t pin_sda, uint8_t pin_scl, uint32_t i2c_freq = 800000,
                 int8_t i2c_port = 1, uint8_t i2c_addr = 0x3C) {
    _bus_cfg.freq_write = i2c_freq;
    _bus_cfg.freq_read = i2c_freq;
    _bus_cfg.pin_sda = pin_sda;
    _bus_cfg.pin_scl = pin_scl;
    _bus_cfg.i2c_port = i2c_port;
    _bus_cfg.i2c_addr = i2c_addr;
    // SSD1306 control byte: 0x00 prefixes a command, 0x40 prefixes pixel data.
    _bus_cfg.prefix_cmd = 0x00;
    _bus_cfg.prefix_data = 0x40;
    _bus_cfg.prefix_len = 1;
  }

  bool init_impl(bool use_reset, bool use_clear) {
    if (_panel_last.get() != nullptr) {
      return true;
    }
    auto p = new lgfx::Panel_SSD1306();
    auto b = new lgfx::Bus_I2C();
    b->config(_bus_cfg);
    p->bus(b);
    {
      auto cfg = p->config();
      cfg.memory_width = cfg.panel_width = 128;
      cfg.memory_height = cfg.panel_height = 64;
      cfg.bus_shared = false;
      p->config(cfg);
    }
    // 0x12 = alternating COM pins, correct for a 128x64 module. A 128x32
    // module needs 0x02 here or every other row is dropped.
    p->setComPins(0x12);
    setPanel(p);
    if (lgfx::LGFX_Device::init_impl(use_reset, use_clear)) {
      _panel_last.reset(p);
      _bus_last.reset(b);
      return true;
    }
    setPanel(nullptr);
    delete p;
    delete b;
    return false;
  }
};
