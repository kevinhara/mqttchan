# services

TypeScript. Two kinds of thing live here: **the API**, which is the only process
that speaks MQTT, and **feeds**, which poll something and POST what they found.

```
packages/contract   the feed <-> API interface (shared types + JSON schema)
packages/feed-kit   the client, poll loop and change tracking every feed needs
api                 triage, rate limiting, and the MQTT connection
feeds/example       a template feed, not a real integration
feeds/flights       arrivals and departures at ZQN, from live ADS-B
```

## The idea

A feed says *what happened*. The API decides *how it looks* and *when it shows*.

A feed never picks an expression, a color or a jingle, and never opens an MQTT
connection. It sends:

```json
{ "source": "ha-temperature", "kind": "sensor.reading",
  "text": "Office 20.1C", "priority": "low",
  "dedupeKey": "sensor.office", "ttlSeconds": 900 }
```

...and gets back `{"decision":"queued"}`, `"deduped"` or `"dropped"` with a
reason. That response is worth reading: a feed that is being rate limited should
know.

## What the API does with it

1. **Rate limit per source** — a token bucket, so one noisy feed cannot
   monopolise the device.
2. **Triage** (`src/triage.ts`) — `kind` + `priority` decide expression, LED
   color, blink and jingle. This is the one place presentation is decided, which
   is what keeps the device's voice consistent and lets the MQTT contract change
   without touching a feed.
3. **Dedupe** — a newer message with the same `dedupeKey` *replaces* the queued
   older one. For sensor feeds this is the rule that does the real work: you see
   the current reading, not a backlog of its history.
4. **Pace** — the gap before the next publish is derived from the message, not a
   flat constant: `jingle + text.length * 45ms + 30s hold`. A long message
   legitimately needs longer, because the bubble types it out one character at a
   time.
5. **Expire** — anything that has waited longer than its `ttlSeconds` is dropped
   rather than shown stale.
6. **Order** — `high` overtakes `normal` overtakes `low`; FIFO within a priority.

`GET /v1/status` reports queue depth, per-source counters and the dedupe/drop
counts. That is how you find out whether the tuning is right: lots of drops means
a feed is too chatty, an idle device means its threshold is too high.

## Writing a feed

Copy `feeds/example`. It is deliberately small — `feed-kit` already provides the
API client with retries, a poll loop that survives a throwing tick, and
`ChangeTracker` for edge-triggered reporting.

**Use `ChangeTracker`.** The Python service this replaces had a producer that
announced the ISS overhead on *every* poll while it was still overhead, and it
got switched off for being noisy. `changedBy(key, value, threshold)` reports on
the first reading and then only when the value has moved enough to be worth
saying, measuring drift from the last *reported* value rather than the last seen
one.

Every feed should support `--once --dry-run`, which polls the real source and
prints what it would send without connecting to anything. It is the fastest way
to check a new feed's output.

## Running

```
npm install
npm run build
npm test

MQTT_HOST=osmo node api/dist/index.js
API_URL=http://localhost:8420 node feeds/example/dist/index.js --once --dry-run
```

## Deploy

One image for the whole workspace — the API and every feed share dependencies
and a build step, so compose picks which to run with `command`. Unlike the rest
of this homelab (a directory per service, published to a host port, addressed by
osmo's LAN IP), these share a compose network: they are one stack, and a feed has
no reason to be reachable from outside it. Only the API publishes a port, and
only for debugging.

```
scp -r . kevin@osmo:~/mqttchan-services
ssh kevin@osmo 'cd ~/mqttchan-services && docker compose up -d --build'
```

Copy `.env.example` to `.env` on the host and `chmod 600` it.

## The ZQN flights feed

`feeds/flights` announces arrivals and departures at Queenstown from live
ADS-B. Built and verified against a real landing on 2026-09-18; see
[`feeds/flights/README.md`](feeds/flights/README.md) for how a movement is
detected, the four rules that stop false positives, and two undocumented
adsb.lol behaviours that will otherwise cost you an hour.

## Still to do: the Home Assistant temperature feed

**Changed 2026-09-18:** this section used to read "Next: the Home Assistant
temperature feed", and the top-level README called it the first real feed.
`feeds/flights` was built first instead — it was simply what got asked for. The
research below was done on 2026-09-17 and still stands; nothing about it was
wrong, it just is not next any more.

Verified against live HA on 2026-09-17, so it doesn't need rediscovering:

- HA exposes **12 entities with `device_class: temperature`**, all °C. Eight are
  useful room sensors — Office, Dining Area, Bedroom, Shed, Laundry, Network
  Cabinet, Bathroom, plus `sensor.home_temperature`.
- **Exclude `sensor.system_monitor_processor_temperature`** — it changes every
  ~45s and is CPU temperature, not a room.
- **Two sensors read `unavailable`** (Kitchen Pantry, Living Room) — the known
  BTHome casualty of osmo's `hci0` Bluetooth crash loop. Treat
  `unavailable`/`unknown` as "no reading", never as a change worth announcing.
  Zigbee end-devices also read that way for up to an hour after an HA restart.
- Cadence is roughly one change every 1–3 minutes across the set — bursty, and
  near the device's ceiling. Dedupe by entity id and a ~0.5°C threshold.
- `Office 20.1°C` is 13 characters, so text length is not a concern here.

The long-lived token goes in `.env` on osmo. The one still sitting in the old
`avatar-brain/.env` returns 401, which is why that service never published
anything.
