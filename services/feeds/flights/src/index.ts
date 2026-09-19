/**
 * Announces arrivals and departures at one configured airport, from live
 * ADS-B. Configured for ZQN (Queenstown, NZQN) by default; the airport is
 * entirely env-driven, so pointing it somewhere else is three variables.
 *
 * As with every feed, this says only *what happened*. It picks no colour and
 * no jingle, and never touches MQTT - `kind: "notice"` is the whole of its
 * opinion, and the API's triage turns that into a face.
 *
 * See what it can see right now, without sending anything:
 *   node dist/index.js --once --dry-run
 */

import { ApiClient, startPolling, WarnOnce } from "@mqttchan/feed-kit";
import { fetchNearby } from "./adsb.js";
import {
  MovementDetector,
  distanceNm,
  identify,
  movementText,
  presenceOf,
  type Airport,
} from "./movements.js";
import { fetchRoute } from "./routes.js";

const SOURCE = "flights";

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

/** Queenstown, NZQN - the aerodrome reference point. */
const airport: Airport = {
  code: process.env["AIRPORT_CODE"] ?? "ZQN",
  lat: num("AIRPORT_LAT", -45.0211),
  lon: num("AIRPORT_LON", 168.7392),
  /** The field is small; 1.5nm covers both ends of the runway and the apron
   *  without reaching any neighbouring strip. */
  groundRadiusNm: num("AIRPORT_GROUND_RADIUS_NM", 1.5),
};

/**
 * How far out to watch. This is not about seeing distant traffic - it is the
 * runway an arrival needs to be observed airborne on before it lands, so that
 * its touchdown reads as an edge rather than a first sighting. 30nm is roughly
 * ten minutes of descent.
 */
const radiusNm = num("AIRPORT_RADIUS_NM", 30);

const client = new ApiClient({
  baseUrl: process.env["API_URL"] ?? "http://api:8420",
  source: SOURCE,
  dryRun,
});

const detector = new MovementDetector({
  airport,
  /** A go-around or a circuit produces genuine ground/air edges; nobody wants
   *  the same aircraft's same movement twice inside ten minutes. */
  cooldownMs: num("FLIGHTS_COOLDOWN_MS", 10 * 60_000),
  /** Long enough to cover a turnaround with the transponder switched off at
   *  the gate, short enough that the maps stay small. */
  forgetMs: num("FLIGHTS_FORGET_MS", 45 * 60_000),
});

const warned = new WarnOnce();

async function tick(): Promise<void> {
  const aircraft = await fetchNearby(airport.lat, airport.lon, radiusNm);

  if (aircraft.length === 0) {
    warned.warn(
      "empty",
      `${SOURCE}: no contacts within ${radiusNm}nm of ${airport.code} - quiet, or coverage is out`,
    );
  } else {
    warned.reset("empty");
  }

  const movements = detector.update(aircraft, Date.now());

  if (dryRun) {
    // A single poll can only ever establish baselines, so print the picture
    // instead: the source and the classification are what you are checking.
    console.log(
      `${SOURCE}: ${aircraft.length} contacts within ${radiusNm}nm of ${airport.code}`,
    );
    for (const ac of aircraft) {
      const presence = presenceOf(ac, airport);
      const where =
        typeof ac.lat === "number" && typeof ac.lon === "number"
          ? `${distanceNm(airport.lat, airport.lon, ac.lat, ac.lon).toFixed(1)}nm`
          : "no position";
      console.log(
        `  ${identify(ac).padEnd(10)} ${(presence ?? "ignored").padEnd(9)} ${where.padStart(9)}  alt=${String(ac.alt_baro ?? "-")}`,
      );
    }
    if (movements.length === 0) {
      console.log(`${SOURCE}: no movements this poll (expected on a first poll)`);
    }
  }

  for (const movement of movements) {
    // Only a real callsign has a route on file - the hex fallback identify()
    // uses when no callsign was transmitted never does.
    const route =
      movement.ident.toLowerCase() === movement.hex.toLowerCase()
        ? null
        : await fetchRoute(movement.ident);
    const text = movementText(movement, airport.code, route);

    const result = await client.send({
      kind: "notice",
      text,
      // Worth interrupting for, but not an alert - `normal` keeps the
      // boarding-style chime that `kind: "notice"` carries, which `low`
      // would strip.
      priority: "normal",
      // Per aircraft and per movement, so two different flights never collapse
      // into one another - only a repeat of this aircraft's own arrival or
      // departure does.
      dedupeKey: `${SOURCE}:${movement.hex}:${movement.movement}`,
      // A movement is only news while it is fresh. Five minutes behind the
      // device is already too late to be worth showing.
      ttlSeconds: num("FLIGHTS_TTL_SECONDS", 300),
    });

    if (result !== null) {
      console.log(
        `${SOURCE}: ${text} -> ${result.decision}${result.reason ? ` (${result.reason})` : ""}`,
      );
    }
  }
}

if (once) {
  tick().catch((err) => {
    console.log(`${SOURCE}: ${String(err)}`);
    process.exit(1);
  });
} else {
  // 60s is comfortably fast enough: an aircraft stays on the ground for many
  // minutes either side of a movement, so the edge cannot slip between polls.
  const intervalMs = num("POLL_INTERVAL_MS", 60_000);
  const stop = startPolling({ name: SOURCE, intervalMs, immediate: true }, tick);
  process.on("SIGTERM", () => { stop(); process.exit(0); });
  process.on("SIGINT", () => { stop(); process.exit(0); });
}
