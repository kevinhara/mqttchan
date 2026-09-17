# mqttchan

A desk notification device: an ESP32 with two 128x64 OLEDs — one showing an
animated avatar face, one showing message text as a typed-out speech bubble —
plus an RGB status LED and a piezo for notification jingles.

This repository holds the whole system: the firmware, the broker config, and the
services that send it messages.

```
   feeds ──HTTP──▶ services/api ──MQTT──▶ broker ──▶ firmware (the device)
(what happened)   (how it looks,        (mosquitto      (renders it)
                   how often)            on osmo)
```

| Directory | What it is |
|---|---|
| [`firmware/`](firmware/) | PlatformIO ESP32 firmware. Flash with `cd firmware && pio run -t upload` |
| [`broker/`](broker/) | mosquitto config, deployed to osmo |
| [`services/`](services/) | TypeScript: the API layer and the feeds that talk to it |
| [`docs/`](docs/) | The control page at [mqttchan.kev.nz](https://mqttchan.kev.nz), served by GitHub Pages |

## Why feeds don't publish to MQTT themselves

Feeds POST a *semantic* message to the API — "this happened, it's low priority"
— and the API alone decides how it looks and when it is shown. Nothing but the
API connects to the broker.

That split earns its keep in two places:

- **The device contract can change without touching a feed.** It already has:
  contract v2 made expressions case-insensitive and gave `led` arbitrary hex
  colors, and no feed needed to know.
- **The device is slow, and one place has to know that.** It shows one message
  at a time and holds it ~30s after typing it out, so it absorbs roughly one
  message per 35 seconds — and its own queue is unbounded, so once you overrun
  it, it never catches up. The API paces publishes to the device's real speed,
  collapses repeated readings of the same thing, drops stale messages, and lets
  genuine alerts jump the queue.

## Status

Built and verified 2026-09-17:

- Firmware builds clean at 45.8% of the 3MB app partition. **Contract v2 is
  written but not yet flashed** — the device was not plugged in. Until it is,
  the device still runs the previous firmware, which ignores lowercase
  expressions and hex colors.
- API verified end-to-end against the real broker on osmo: publishes valid
  contract v2 payloads, paces them, dedupes by key, and lets a high-priority
  message overtake queued ambient ones.
- 32 unit tests passing across the API and feed-kit.

No real data source is connected yet. The first one will be Home Assistant
temperature readings — see [`services/README.md`](services/README.md).

## Deploy

```
cd firmware && pio run -t upload                      # the device
scp -r broker kevin@osmo:~/mosquitto                  # the broker
scp -r services kevin@osmo:~/mqttchan-services        # the API and feeds
ssh kevin@osmo 'cd ~/mqttchan-services && docker compose up -d --build'
```

`services/.env` is not in git — copy `services/.env.example` on the host and
fill it in.

## History

The broker config and a Python publisher called `avatar-brain` used to live in
`~/Homelab/osmo/`, which is not a git repository. Both moved here on
2026-09-17: the broker config unchanged, and `avatar-brain` superseded rather
than migrated — it had been running on osmo for weeks in a dormant state, with
no entities configured and an expired token, and had never published anything.
Stubs in the old Homelab directories point here.
