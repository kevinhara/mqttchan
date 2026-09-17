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

Built, deployed and verified on hardware 2026-09-17:

- **Contract v2 is flashed and verified on the device** (`Ziggy`, `10.0.0.114`).
  Firmware is at 45.8% of the 3MB app partition. Checked against serial with a
  negative control, so silence means parsed rather than merely unobserved:

  | Payload | Serial |
  |---|---|
  | `happy` + `#ff8800` + `Chime` | silent — v2 forms all parse |
  | `Happy` + `red` + `chime` | silent — the old spellings still work |
  | `sleepy` + `cycle` | silent |
  | `ecstatic` + `#fff` + `kazoo` | all three warned and fell back |

  The `#fff` rejection is deliberate: only the exact 6-digit form parses, so a
  typo warns instead of being guessed at.
- **`services/api` is deployed on osmo** (`mqttchan-api`, port 8420) and
  connected to the broker. Verified publishing valid v2 payloads with pacing,
  dedupe by key, and high-priority messages overtaking queued ambient ones.
- **The Python `avatar-brain` container has been stopped and removed** from
  osmo. Its source directory is left at `~/avatar-brain` there, so the
  decommission is reversible.
- 32 unit tests passing across the API and feed-kit.

No real data source is connected yet — `feeds/example` is a template and is left
disabled behind a compose profile, so the device stays quiet. The first real feed
will be Home Assistant temperature readings; see
[`services/README.md`](services/README.md).

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
