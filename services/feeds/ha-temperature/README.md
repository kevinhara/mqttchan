# feeds/ha-temperature

Reports room temperatures from Home Assistant - Zigbee sensors only.

```
node feeds/ha-temperature/dist/index.js --once --dry-run
```

## Why "Zigbee only" needs asking, not guessing

`GET /api/states` gives every entity's `device_class`, and `temperature` looks
the same regardless of radio. This HA instance also has BTHome (Bluetooth)
sensors with `device_class: temperature` - two of them, Kitchen Pantry and
Living Room, are recorded in `services/README.md` as the known casualty of
osmo's `hci0` Bluetooth crash loop. Nothing in an entity's id, its name or its
state distinguishes a Zigbee reading from a Bluetooth one; only HA's own
integration membership does.

So this feed asks HA directly, via `/api/template` and its `integration_entities()`
Jinja function (`src/ha.ts`), for the entity ids that belong to one configured
integration (`HA_ZIGBEE_INTEGRATION`), and only reports a `device_class:
temperature` entity that also appears in that set. `src/temperature.ts` is the
whole of that rule, kept pure and tested without a live HA to talk to, same
reasoning as `flights/src/movements.ts`.

**`HA_ZIGBEE_INTEGRATION` defaults to `zha`, and that default is unverified.**
HA's built-in ZHA integration is the common case for a setup with no separate
MQTT bridge, but nothing in this repo or in the old `avatar-brain` producer it
replaces says which integration osmo's HA actually runs - Zigbee2MQTT and
deCONZ are both real possibilities and would need a different value here. A
wrong guess is safe, not silent-wrong: it shows up as `--once --dry-run`
printing zero sensors, not as a Bluetooth reading mislabelled as Zigbee, because
the two checks (`device_class` and integration membership) are independent and
both have to pass.

## Why report-on-change rather than an edge detector

Unlike `feeds/flights`, this is exactly the case `feed-kit`'s `ChangeTracker`
was built for: HA has already done the sensing, so there is no arrival/departure
style edge to detect - just a value that should be reported once and then only
again once it has moved. `services/README.md` names the previous Python
service's failure mode this avoids: a producer that re-announced its reading on
every poll while nothing had changed.

## Notes on the source (Home Assistant's REST API)

- **`/api/states` has no integration field.** `device_class`, `friendly_name`
  and `unit_of_measurement` are on every state, but which integration owns an
  entity is not - that's what makes `/api/template` + `integration_entities()`
  necessary rather than an incidental choice.
- **`/api/template` returns rendered text, not JSON**, even though the request
  body is JSON. `src/ha.ts` renders the template with a trailing `| tojson`
  filter and parses the response as text for exactly this reason - reading it
  with `res.json()` would fail.
- **`unavailable`/`unknown` is not a reading.** `services/README.md` records
  that Zigbee end-devices sit in that state for up to an hour after an HA
  restart, same as the two BTHome sensors after osmo's Bluetooth crashes.
  `readingsOf` in `src/temperature.ts` skips both states outright rather than
  parsing them as numbers or reporting a change away from "nothing".
- **Same env var names as the predecessor.** `avatar-brain/producers/ha_sensors.py`
  established `HA_URL` and `HA_TOKEN` (a long-lived access token from HA's own
  profile page); this feed keeps them rather than inventing new ones. That
  service's own token in `avatar-brain/.env` on osmo returns 401 - it needs a
  fresh one in `mqttchan-services`' `.env`, not a copy of the old one.

## Verified

**2026-09-18, against live HA on osmo.** `HA_ZIGBEE_INTEGRATION=zha` was a
guess when this was written, and it turned out to be right on the first try:
`--once --dry-run` against the real `HA_URL`/`HA_TOKEN` returned exactly 7
sensors -

```
Office           16.9°C  (sensor.lumi_lumi_weather_temperature)
Dining Area      19.4°C  (sensor.lumi_lumi_weather_temperature_2)
Bedroom          17.3°C  (sensor.lumi_lumi_weather_temperature_3)
Shed             12.0°C  (sensor.lumi_lumi_weather_temperature_4)
Laundry          16.1°C  (sensor.lumi_lumi_weather_temperature_5)
Network Cabinet  20.4°C  (sensor.lumi_lumi_weather_temperature_6)
Bathroom         17.5°C  (sensor.lumi_lumi_weather_temperature_7)
```

- Both BTHome sensors (Kitchen Pantry, Living Room) and the CPU monitor are
  correctly absent - `zha` membership excludes them even though the CPU one
  shares `device_class: temperature`.
- **`sensor.home_temperature`, one of the "eight useful room sensors" named in
  `services/README.md`'s original research, is also absent** - it does not
  belong to the `zha` integration. It reads as a virtual/aggregate entity
  (there is no matching `_2`/`_3` suffix pattern like the seven Lumi/Aqara
  devices above, which is what a template or group sensor built from other
  readings would look like), not a physical Zigbee radio, so this feed
  correctly leaves it out under a "Zigbee sensors only" brief - even though
  the original research counted it among the sensors worth reporting.
- All 7 readings were sent through the real API and `docker logs` confirmed
  `queued` for every one; the deployed container is running continuously via
  `docker-compose.yml`, no profile gate needed any more.
- Unit tests in `test/temperature.test.ts` cover the filtering and naming
  logic against fixtures shaped like HA's state format; the numbers above are
  the first live confirmation that the fixtures weren't hiding anything.

Still unconfirmed: `ChangeTracker`'s 0.5°C threshold and 900s TTL haven't seen
a real day's worth of drift yet, so whether the cadence is right is not yet
known.

**Correction, 2026-09-18 (same day):** the message text this feed sends is
plain ASCII - `Office 20.1C`, no degree sign - not `20.1°C` as the readings
above and the original research in `services/README.md` show it. On the real
device the degree sign rendered as two garbage glyphs, because
`SpeechBubble::show()` (`firmware/include/speech_bubble.h`) types a message
out one raw *byte* at a time, and `°`'s UTF-8 encoding is two bytes (`0xC2
0xB0`) - splitting them apart means neither byte reaches the font as the
character it was meant to be. `src/index.ts`'s `formatReading()` is the fix,
and it is the one place in this feed that still matters for what actually
shows on screen; the numbers logged elsewhere in this README (from `--once
--dry-run`, before this was caught) keep the degree sign because that output
is a terminal, not the device, and terminals render UTF-8 fine.
