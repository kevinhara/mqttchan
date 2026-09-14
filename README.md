# avatar-demo

[stack-chan/m5stack-avatar](https://github.com/stack-chan/m5stack-avatar) running
on a bare ESP-WROOM-32 with a 128x64 SSD1306 I2C OLED — no M5Stack hardware
involved. A second SSD1306 shows what's "said" as a word-wrapped speech
bubble (see "The speech bubble" below) — optional, and everything below runs
fine without it wired up.

By default the board joins WiFi, subscribes to an MQTT topic, and drives the
avatar + bubble from whatever `{"text":..., "expression":...}` JSON arrives
(see "MQTT-driven mode" below) — this is meant to run against the
`avatar-brain` business-layer service in the wider homelab, not standalone.
Build with `-DAVATAR_DEMO_MODE` instead to get the old self-cycling bench
demo (cycles the six built-in expressions, fakes lip-sync babble on every
third one) with no network required.

Verified live 2026-09-14 on ESP32-D0WDQ6 rev v1.0 (`/dev/cu.usbserial-0001`):
demo mode renders all six expressions correctly, heap flat at ~295 KB across
9 cycles, no reboots. MQTT mode verified the same day via the headless bench
test described below (no live broker was up yet): WiFi correctly times out
and retries rather than hanging when `secrets.h` has placeholder credentials,
and a synthetic payload correctly renders both the Happy expression and a
two-line-wrapped bubble.

## Core split

This was the reason for building it — the avatar gets one core, the other
stays free for application work.

| Core | Owner | What runs there |
|---|---|---|
| 1 (`APP_CPU`) | the avatar | `drawLoop` + `facialLoop` (blink, saccade, breath) |
| 0 (`PRO_CPU`) | you | `appTask` — triggers, TTS, LLM calls, network |

The library already does most of this: `Avatar::start()` hardcodes both of its
tasks to `APP_CPU_NUM`, so no patching was needed. The part that *is* on us is
that Arduino's `loop()` also runs on core 1 — so `loop()` is deliberately left
empty and all app logic goes in `appTask`, pinned to `PRO_CPU_NUM`. Putting
work in `loop()` would quietly land it on the avatar's core and fight the
renderer for time.

Cross-core calls into the avatar are safe for what the demo uses:
`setExpression()` suspends the draw task before mutating, and the float
setters (`setMouthOpenRatio`, gaze, breath) are single-word writes the draw
task only reads.

## Wiring

Two SSD1306 panels, **each on its own I2C bus** — they don't share pins:

| Panel | SDA | SCL | VCC | GND | Bus |
|---|---|---|---|---|---|
| Face (`oled`) | GPIO4 | GPIO15 | 3V3 | GND | hardware I2C port 1 |
| Bubble (`oledText`) | GPIO32 | GPIO33 | 3V3 | GND | software (bit-banged) I2C |

Both panels answer at the default address **0x3C** — no jumper needed, since
they're on physically separate buses and there's nothing to collide with.

**Do not move the face panel's SDA to GPIO2.** GPIO2 is a boot-strapping pin;
the OLED's pull-up on it holds the ESP32 out of USB download mode (`Wrong boot
mode detected (0x1b)`) and flashing fails whenever the display is attached.
Same lesson the sibling `hello-world` panel learned on 2026-09-14. Port 1 is
likewise deliberate for the face panel: M5Unified's `begin()` claims port 0 for
its own PortA devices, and sharing it means two drivers reconfiguring one
peripheral with different pins.

**Why the bubble panel is on GPIO32/33 instead of a second hardware port:**
the ESP32 classic only has two hardware I2C peripherals, and both are already
spoken for (port 0 by M5Unified, port 1 by the face panel). `SSD1306Display`
in `ssd1306_display.h` passes its `i2c_port` argument straight through to
LGFX's `Bus_I2C`, which treats a **negative** port number as a request for its
bit-banged software I2C path (`soft_i2c.inl`) on whatever GPIOs you give it —
so `oledText` in `main.cpp` is constructed with port `-1` and pins 32/33
instead. GPIO32/33 were picked because they're not boot-strapping pins, not
the SPI-flash pins (6-11), and not already claimed by the face panel or
M5Unified. Clocked at 400kHz rather than the face panel's 800kHz, since
bit-banged timing is CPU-cycle-bound and less forgiving at high speed.

## The speech bubble (second panel)

`include/speech_bubble.h`'s `SpeechBubble` class draws a bordered,
word-wrapped text box to a second panel — `oledText` in `main.cpp` — kept
completely separate from the avatar's own `M5.Display`. Text is revealed one
character at a time (`show()`'s `charDelayMs`, default 25ms/char) rather than
appearing all at once, so it reads as the character actually talking. This
blocks `appTask` for the reveal's duration (~strlen * charDelayMs) — fine
here since nothing else needs that core's attention mid-phrase, but worth
knowing if something latency-sensitive ever gets added to the same task.
`appTask` calls `bubble->show(...)` with a canned phrase alongside each
expression change; swap that call for real TTS output text and nothing else
needs to change. The canned phrases (`kPhrases` in `main.cpp`) are nihilistic
one-liners — a demo bubble should at least be funny.

Status: verified live 2026-09-14 on real hardware (both failure and success
paths). With no second panel attached, `setup()` logged `Second SSD1306
(text) init failed` exactly as designed and the avatar kept cycling normally
on the first panel. With the second panel wired to GPIO32/33 per Wiring above,
the framebuffer dump (`dumpBubbleFramebuffer`, under `-DAVATAR_FB_DUMP`)
confirmed the rounded-rect border draws correctly and word-wrap works: short
phrases ("Great to see you!") render on one line, and the longest phrase
("Feeling kind of blue.") correctly wraps to two lines without overlap.

## How it was made to work on a non-M5 panel

Two problems had to be solved; both are worth knowing before editing this.

**1. The avatar only draws to `M5.Display`.** `Face.cpp` references `M5.Lcd` /
`M5.Display` directly — there is no "render to an arbitrary canvas" entry
point. So the fix is to make M5Unified's primary display *be* the SSD1306.
M5GFX ships `lgfx::Panel_SSD1306` but no device wrapper for it (only
`M5UnitOLED`, which is an SH110x 64x128), so `include/ssd1306_display.h` is
that wrapper, written to the same shape as M5GFX's own `M5UnitOLED.h`. It is
attached with `M5.addDisplay()` + `M5.setPrimaryDisplay()`.

Ordering is load-bearing: **`oled.init()` must come after `M5.begin()`.**
`M5.begin()` runs M5GFX board autodetect, which probes SPI pins — GPIO15 among
them, which is our SCL. Initialising the OLED afterwards re-owns those pins and
sends the SSD1306 its full reset sequence. The other order leaves the panel
blank.

**2. The stock face is drawn for a 320x240 canvas.** `m5avatar::Face` puts the
eyes at x=90/230 and the mouth at y=148; the library's own `faces/OledFace.h`
is in the same coordinate space despite its name, so it is not the answer
here. Using `Avatar::setScale()` to shrink 320x240 into 128x64 works but looks
bad — on a 1-bit panel there is no grey to antialias a downscale into, so the
eyes come out as ragged blobs. `include/small_oled_face.h` instead authors the
geometry at 1:1 for 128x64.

When editing that geometry: every position is the **centre** of its part, not a
corner, and `BoundingRect` takes **(top, left)** — y first. `Eye` reads
`getCenterX/Y()` while `Mouth` and `Eyeblow` read `getLeft/getTop()`; the
two-arg `BoundingRect` leaves width/height at 0, which is what makes those
agree.

## Seeing what the panel shows, without looking at it

Build with `-DAVATAR_FB_DUMP` and the panel framebuffer gets dumped to serial
as ASCII art (`Panel_HasBuffer` keeps a readable RAM copy, so this reads back
what was actually rasterised, not what was intended) — after each expression
change in demo mode, or once after the synthetic bench-test payload in MQTT
mode (see "MQTT-driven mode" above). It costs nothing when the flag is off,
which is the default. This is how every "verified live" claim in this file
was checked.

```
pio run -t upload                                              # MQTT mode (default)
PLATFORMIO_BUILD_FLAGS="-DAVATAR_DEMO_MODE" pio run -t upload   # demo mode, no network
PLATFORMIO_BUILD_FLAGS="-DAVATAR_FB_DUMP" pio run -t upload     # + framebuffer dump
```

Note `pio device monitor` fails in a non-TTY shell (`termios.error: (19,
Operation not supported by device)`) — read the port with pyserial instead.

## MQTT-driven mode

Default build (no flags). `include/mqtt_link.h`'s `MqttLink` owns WiFi +
MQTT: connects, subscribes to one topic, and on every message parses
`{"text": "...", "expression": "..."}` JSON and calls
`avatar.setExpression()` + `bubble->show(text)` straight from the PubSubClient
callback — safe here because that callback already runs inside
`mqttLink.loop()` on core 0/`appTask`, the same task that owned those calls in
demo mode, and `setExpression()` already suspends the draw task internally.
No queue needed for a single-producer, single-consumer, already-single-task
design.

Config lives in `include/secrets.h` (gitignored — copy `secrets.h.example`
and fill in `WIFI_SSID`/`WIFI_PASS`/`MQTT_HOST`/`MQTT_PORT`/`MQTT_TOPIC`).
Expression strings must match `kExpressionNames[]` in `main.cpp` exactly
(`Happy`/`Angry`/`Sad`/`Doubt`/`Sleepy`/`Neutral`) — an unrecognized string
falls back to `Neutral` and logs a warning rather than failing silently.

A few sharp edges worth knowing if this stops working:
- **PubSubClient's default 256-byte buffer silently drops anything larger** —
  no error, nothing in the callback, the message just never arrives.
  `mqtt_link.h` calls `setBufferSize(512)` explicitly; if messages start
  disappearing, check payload size against that first.
- **Keepalive is bumped to 60s** (`setKeepAlive(60)`, default is 15s) because
  `bubble->show()` blocks per-character for `strlen * charDelayMs` — a long
  message can eat several seconds inside the callback, during which
  `client.loop()` isn't being re-entered.
- **`appTask`'s stack is 8192 bytes**, not the demo's old 4096 — WiFi,
  PubSubClient and ArduinoJson buffers all share it now, and the old size
  overflows silently once networking is in the mix.
- **Reconnect checks WiFi before retrying MQTT**, not two independent retry
  loops — the common hobby-AP failure mode is WiFi dropping mid-MQTT-retry
  and hammering `client.connect()` against a dead link.
- The board publishes `avatar/status` (`"online"`/`"offline"` via MQTT LWT,
  retained) so anything watching can tell if it's actually up.
- Flash usage is noticeably tighter in this mode: **88% of the 1.25MB app
  partition** (vs 46% in demo mode) once WiFi + PubSubClient + ArduinoJson are
  linked in — still fits, but there isn't a lot of headroom left for more
  libraries without moving to a bigger partition scheme.

**Headless bench test, no broker needed:** build with `-DAVATAR_FB_DUMP` and
`appTask` injects one synthetic payload straight into the parse-and-display
path (`MqttLink::injectForTest()`) before entering its normal loop, then
dumps both panels' framebuffers to serial — exercises the whole JSON →
expression → display pipeline without a live MQTT broker. See "Seeing what
the panel shows" below for the flag mechanics.

**Known limitation, not solved here:** `SpeechBubble::wrapLines()` doesn't
cap vertical line count — text too long for the 64px panel just wraps past
the bottom edge and isn't visible (no crash, no corruption). Message-length
discipline belongs in whatever publishes to `avatar/say` (keep it well under
~50 characters), not as new truncation/scrolling logic here.

## Where to hook the "TBD" half

`speakFor()` (demo mode only) drives `mouthOpenRatio` from random noise as a
placeholder lip-sync. A real TTS or mic-amplitude source would need its own
hook in MQTT mode, since the MQTT callback currently only calls
`setExpression()`/`bubble->show()` — nothing drives the mouth outside demo
mode yet. No speaker is wired to this board either way, and
`cfg.internal_spk` is off in `setup()` — turn it on when an I2S DAC is added.
The library also ships `tasks/LipSync.h` for mic-driven mouth movement, which
is unused here.
