# feeds/sky

Announces sun, moon and visible-planet events, aurora-relevant space
weather, and MetService severe weather watches/warnings for one location.
Configured for **Queenstown, Otago** by default; the location is five
environment variables, so pointing it elsewhere needs no code change.

```
node feeds/sky/dist/index.js --once --dry-run
```

## What's in v1

- **Sun/moon/planet digest, once a day.** `src/astro.ts` wraps
  [`astronomy-engine`](https://github.com/cosinekitty/astronomy), a
  deterministic offline ephemeris - no network call, no key, same inputs
  always give the same outputs. It computes sunrise/sunset, moonrise/moonset,
  the moon's phase name and illumination percentage, and which naked-eye
  planets (Mercury, Venus, Mars, Jupiter, Saturn) clear 5 degrees of altitude
  at dusk or the following dawn. The digest fires once per calendar day (in
  `LOCATION_TIMEZONE`), not on every poll.
- **Today's weather outlook, in the same digest.** `src/weather.ts` calls
  MetService's Point Forecast API (`forecast-v2.metoceanapi.com`) for high/low
  temperature, cloud cover, expected rainfall and wind - the one part of this
  feed that needs `METSERVICE_API_KEY`. With no key set, the digest is sent
  without this clause rather than not sent at all - see below.
- **Aurora alert from NOAA's planetary Kp index.** `src/spaceweather.ts`
  polls `services.swpc.noaa.gov/products/noaa-planetary-k-index.json`, public
  and keyless. This is the practical form "sun activity" takes for a
  Queenstown latitude: geomagnetic activity, not solar X-ray flux, is what
  actually gates whether the aurora is worth a look from here. Kp 5 (G1
  minor storm) is the default alert threshold - edge-triggered (fires once as
  Kp crosses up through it, not on every poll while it stays elevated) with
  hysteresis against a reading bouncing across the line.
- **MetService severe weather watches/warnings.** `src/warnings.ts` reads
  MetService's public CAP Atom feed at `alerts.metservice.com/cap/atom` - no
  key, no registration.

Everything except the weather outlook needs no API key at all.

## Wiring up the weather outlook (`METSERVICE_API_KEY`)

Researched 2026-09-19: MetService's current docs site
(`developer.metservice.com`) describes the Point Forecast API's existence
(REST, OpenAPI 3.0, x-api-key auth) but does not publish the endpoint path,
request shape or variable names anywhere reachable without a console
account. The real shape came from MetOcean's own examples repo instead -
[`metoceanapi/forecast-api-examples`](https://github.com/metoceanapi/forecast-api-examples) -
and was then confirmed live against a real key and Queenstown's coordinates:

- `POST https://forecast-v2.metoceanapi.com/point/time`, header
  `x-api-key: <key>`, body
  `{points: [{lon, lat}], variables: [...], time: {from, interval, repeat}}`.
- The variable catalogue is itself an endpoint -
  `GET https://forecast-v2.metoceanapi.com/variables/` - rather than a fixed
  list to keep in sync by hand. This feed requests six:
  `air.temperature.at-2m` (Kelvin), `precipitation.rate` (mm/hour),
  `wind.speed.at-10m`, `wind.speed.gust.at-10m` (both meters/second),
  `wind.direction.at-10m` (degrees, meteorological "from" convention) and
  `cloud.cover` (percent).
- The response is `{dimensions, noDataReasons, variables}`, where each
  requested variable comes back as `{data: number[], noData: number[]}` -
  one entry per requested time step. `noData` is `0` for a good sample and a
  `noDataReasons` code otherwise (e.g. a wave variable requested over land
  comes back `MASK_LAND`); `weather.ts` drops any sample whose `noData` isn't
  `0` rather than trusting a bogus number.
- Sample response for Queenstown's coordinates, 2026-09-19: 5 time steps
  (3-hourly) gave `air.temperature.at-2m` of 279.7K down to 274.5K (6.6C down
  to 1.4C - a realistic September night), `wind.speed.gust.at-10m` up to
  17.9 m/s (64km/h), matching a live Crown Range Road snow warning seen the
  same day.

`fetchDailyOutlook` requests 8 steps at a 3-hour cadence (roughly the next
24h) and reduces them to a daily high/low, a rough total-rainfall estimate
(each sample's mm/hour rate times the 3-hour interval, summed - a real
integral this is not, but close enough for "rain expected today: roughly
Xmm"), the windiest step's speed/gust/direction, and mean cloud cover
bucketed into clear/partly cloudy/overcast.

Also plausible, not built: solar flare class (X-ray flux) as a second space
weather signal alongside Kp; supermoon/perigee note on the moon phase;
sidereal notes (equinox/solstice countdown). None of these needed a decision
to leave out - they just aren't "the basics" yet.

## Region matching for warnings

MetService's Atom feed lists every current watch/warning nationwide with no
region field on the entry itself - only the linked CAP XML document (one
extra HTTP request per entry) carries an `areaDesc`. `matchesRegion` checks
that description, case-insensitively, against `SKY_REGION_KEYWORDS`
(default: Otago, Queenstown, Southern Lakes, Wakatipu, Wanaka, Crown Range,
Cardrona, Coronet Peak, Remarkables, Fiordland).

**Verified live 2026-09-19** against the real feed: a "Strong Wind Watch"
entry's `areaDesc` read "Hawke's Bay south of Waipawa and the Tararua
District" - nowhere near Otago, and nothing in its title or id would have
told you that. A same-day "Road Snowfall Warning" entry's *id* already named
`crownrangeroad`, but that is not something to rely on - the general "Strong
Wind Watch" entries use a generic `severeweather.nz` id with no location
hint at all, so opening the CAP document is the only reliable way to check.

## Escalation: Watch vs Warning

MetService's own ladder is Watch (possible) below Warning (expected).
`isWarningLevel` reads that straight off the headline text ("Warning" vs
anything else, e.g. "Watch" or "Advisory"). A Warning sends `kind: "alert"`
at `priority: "high"` (the device's only blinking kind); a Watch sends
`kind: "notice"` at `priority: "normal"` (the same boarding-style chime
`feeds/flights` uses) - worth knowing about, not worth the alarm.

## Verified

2026-09-19, against the real astronomy-engine library (offline, deterministic)
for Queenstown's coordinates: sunrise/sunset, moonrise/moonset, moon phase
angle and illumination, and dusk/dawn planet altitudes all reproduced exactly
against a fixed reference time in `test/astro.test.ts` - see that file for
the reference values.

2026-09-19, against the live services:

- `services.swpc.noaa.gov/products/noaa-planetary-k-index.json` returns a
  JSON array of `{time_tag, Kp, a_running, station_count}` objects, oldest
  first, no header row.
- `alerts.metservice.com/cap/atom` returns a real Atom feed of current
  watches/warnings (a Strong Wind Watch and two Road Snowfall Warnings seen
  live); each entry's `link[rel=related]` points at a CAP 1.2 XML document
  with `info.headline`, `info.description`, `info.severity` and
  `info.area.areaDesc`.

2026-09-19, against the live Point Forecast API with a real key: a
`point/time` request for Queenstown's coordinates returned real, physically
sane data (see above), and `--once --dry-run` produced a full digest
including the weather clause, e.g. "high 7C, low 1C, overcast. 8mm rain
expected. WSW wind to 15km/h, gusts 65km/h." - consistent with the
Crown Range Road snow warning active the same day.

Not yet observed in production: an actual aurora alert firing (Kp has not
reached 5 since this was built). Exercised by unit tests against real
captured data, not yet by a live event.
