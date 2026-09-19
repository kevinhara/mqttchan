/**
 * Announces sun, moon and visible-planet events, aurora-relevant space
 * weather, today's weather outlook, and MetService severe weather
 * watches/warnings for one location. Configured for Queenstown, Otago by
 * default; everything location-specific is env-driven, same convention as
 * `feeds/flights`' airport.
 *
 * Weather conditions need a registered `METSERVICE_API_KEY`
 * (console.metoceanapi.com); everything else in this feed needs no key. With
 * none set, the digest is sent without a weather clause - see weather.ts.
 *
 * See what it would send right now, without sending anything:
 *   node dist/index.js --once --dry-run
 */

import { ApiClient, startPolling, WarnOnce } from "@mqttchan/feed-kit";
import {
  moonAngle,
  moonIllumination,
  moonPhaseName,
  moonTimes,
  observerFor,
  sunTimes,
  visiblePlanets,
  type Location,
} from "./astro.js";
import { formatDigest } from "./format.js";
import { auroraEdge, fetchKp } from "./spaceweather.js";
import { fetchActiveWarnings } from "./warnings.js";
import { fetchDailyOutlook, type DailyOutlook } from "./weather.js";

const SOURCE = "sky";

const args = new Set(process.argv.slice(2));
const once = args.has("--once");
const dryRun = args.has("--dry-run");

function num(key: string, fallback: number): number {
  const raw = process.env[key];
  if (raw === undefined || raw === "") return fallback;
  const parsed = Number(raw);
  if (!Number.isFinite(parsed)) {
    throw new Error(`${key} must be a number, got ${JSON.stringify(raw)}`);
  }
  return parsed;
}

function list(key: string, fallback: string[]): string[] {
  const raw = process.env[key];
  if (raw === undefined || raw === "") return fallback;
  return raw
    .split(",")
    .map((s) => s.trim())
    .filter((s) => s !== "");
}

/** Queenstown township, Otago - overridable for anywhere else. */
const location: Location = {
  name: process.env["LOCATION_NAME"] ?? "Queenstown, Otago, NZ",
  lat: num("LOCATION_LAT", -45.0311),
  lon: num("LOCATION_LON", 168.6626),
  elevationM: num("LOCATION_ELEVATION_M", 310),
};
const timeZone = process.env["LOCATION_TIMEZONE"] ?? "Pacific/Auckland";

/** Matched case-insensitively against each CAP alert's `areaDesc` - covers
 *  Queenstown Lakes district and its usual named sub-areas (roads, ranges)
 *  without needing every one of them listed individually. */
const regionKeywords = list("SKY_REGION_KEYWORDS", [
  "Otago",
  "Queenstown",
  "Southern Lakes",
  "Wakatipu",
  "Wanaka",
  "Crown Range",
  "Cardrona",
  "Coronet Peak",
  "Remarkables",
  "Fiordland",
]);

/** Kp 5 = G1 minor geomagnetic storm - see spaceweather.ts for why this is
 *  the "worth a look" line for Queenstown's latitude. */
const auroraOnThreshold = num("SKY_AURORA_KP_ON", 5);
const auroraOffThreshold = num("SKY_AURORA_KP_OFF", 4);

/** Weather conditions are the one part of this feed that needs a key -
 *  everything else needs none. Empty/unset just means no weather clause. */
const metserviceApiKey = process.env["METSERVICE_API_KEY"] ?? "";

const observer = observerFor(location);

const client = new ApiClient({
  baseUrl: process.env["API_URL"] ?? "http://api:8420",
  source: SOURCE,
  dryRun,
});

const warned = new WarnOnce();

let lastDigestDate: string | null = null;
let auroraAlerted = false;
/** CAP id -> first-seen time, so ids can be forgotten instead of the set
 *  growing forever over a long-lived process. */
const seenWarnings = new Map<string, number>();
const SEEN_WARNING_TTL_MS = 48 * 60 * 60_000;

function todayIn(date: Date): string {
  return new Intl.DateTimeFormat("en-CA", { timeZone }).format(date);
}

function pruneSeenWarnings(nowMs: number): void {
  for (const [id, seenAt] of seenWarnings) {
    if (nowMs - seenAt > SEEN_WARNING_TTL_MS) seenWarnings.delete(id);
  }
}

async function sendDigest(now: Date): Promise<void> {
  let weather: DailyOutlook | null = null;
  if (metserviceApiKey !== "") {
    weather = await fetchDailyOutlook(location.lat, location.lon, metserviceApiKey, now);
    if (weather === null) {
      warned.warn(
        "weather",
        `${SOURCE}: could not reach MetService's Point Forecast API - digest will skip weather`,
      );
    } else {
      warned.reset("weather");
    }
  } else {
    warned.warn(
      "weather-unconfigured",
      `${SOURCE}: METSERVICE_API_KEY not set - digest will skip weather`,
    );
  }

  const text = formatDigest({
    locationName: location.name,
    timeZone,
    sun: sunTimes(observer, now),
    moon: moonTimes(observer, now),
    moonPhase: moonPhaseName(moonAngle(now)),
    moonIlluminationFraction: moonIllumination(now),
    planets: visiblePlanets(observer, now),
    ...(weather ? { weather } : {}),
  });

  if (dryRun) console.log(`${SOURCE}: [digest] ${text}`);

  const result = await client.send({
    kind: "notice",
    text,
    priority: "normal",
    dedupeKey: `${SOURCE}:digest:${todayIn(now)}`,
    // Generated near local midnight; not shown within 6 hours of that means
    // tomorrow's digest is more useful than this one arriving stale.
    ttlSeconds: num("SKY_DIGEST_TTL_SECONDS", 6 * 60 * 60),
  });
  if (result !== null) {
    console.log(
      `${SOURCE}: digest -> ${result.decision}${result.reason ? ` (${result.reason})` : ""}`,
    );
  }
}

async function checkAurora(): Promise<void> {
  const kp = await fetchKp();
  if (kp === null) {
    warned.warn(
      "kp",
      `${SOURCE}: could not reach NOAA SWPC for the Kp index - skipping the aurora check`,
    );
    return;
  }
  warned.reset("kp");

  const { shouldAlert, alerted } = auroraEdge(
    kp,
    auroraAlerted,
    auroraOnThreshold,
    auroraOffThreshold,
  );
  auroraAlerted = alerted;

  if (dryRun) console.log(`${SOURCE}: [aurora] Kp ${kp.toFixed(2)}, alerted=${alerted}`);
  if (!shouldAlert) return;

  const text = `Aurora watch: planetary Kp index ${kp.toFixed(1)} - worth a look south tonight`;
  const result = await client.send({
    kind: "celebrate",
    text,
    priority: "high",
    // Keyed so a later, still-elevated reading replaces a queued earlier one
    // rather than both being shown.
    dedupeKey: `${SOURCE}:aurora`,
    ttlSeconds: num("SKY_AURORA_TTL_SECONDS", 60 * 60),
  });
  if (result !== null) {
    console.log(
      `${SOURCE}: aurora -> ${result.decision}${result.reason ? ` (${result.reason})` : ""}`,
    );
  }
}

async function checkWarnings(): Promise<void> {
  const warnings = await fetchActiveWarnings(regionKeywords);
  if (warnings === null) {
    warned.warn(
      "warnings",
      `${SOURCE}: could not reach MetService's CAP feed - skipping the warnings check`,
    );
    return;
  }
  warned.reset("warnings");

  const nowMs = Date.now();
  pruneSeenWarnings(nowMs);

  if (dryRun) {
    console.log(`${SOURCE}: [warnings] ${warnings.length} active for ${location.name}`);
    for (const w of warnings) console.log(`  ${w.headline} (${w.areaDesc})`);
  }

  for (const w of warnings) {
    if (seenWarnings.has(w.id)) continue;
    seenWarnings.set(w.id, nowMs);

    const text = `${w.headline}: ${w.description}`.slice(0, 300);
    const result = await client.send({
      kind: w.isWarning ? "alert" : "notice",
      text,
      priority: w.isWarning ? "high" : "normal",
      // Per CAP id, which MetService already mints fresh for every update -
      // so a superseding update queues as new news rather than deduping
      // against the version it replaces.
      dedupeKey: `${SOURCE}:warning:${w.id}`,
      ttlSeconds: num("SKY_WARNING_TTL_SECONDS", 4 * 60 * 60),
    });
    if (result !== null) {
      console.log(
        `${SOURCE}: ${w.headline} -> ${result.decision}${result.reason ? ` (${result.reason})` : ""}`,
      );
    }
  }
}

async function tick(): Promise<void> {
  const now = new Date();

  const today = todayIn(now);
  if (once || today !== lastDigestDate) {
    lastDigestDate = today;
    await sendDigest(now);
  }

  await checkAurora();
  await checkWarnings();
}

if (once) {
  tick().catch((err) => {
    console.log(`${SOURCE}: ${String(err)}`);
    process.exit(1);
  });
} else {
  // Sun/moon/planet facts and space weather change slowly enough that 60s
  // polling (the flights/ha-temperature default) would be pure waste here;
  // 10 minutes is fast enough to catch a fresh MetService warning promptly.
  const intervalMs = num("POLL_INTERVAL_MS", 10 * 60_000);
  const stop = startPolling({ name: SOURCE, intervalMs, immediate: true }, tick);
  process.on("SIGTERM", () => { stop(); process.exit(0); });
  process.on("SIGINT", () => { stop(); process.exit(0); });
}
