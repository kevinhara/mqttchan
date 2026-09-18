/**
 * Turning a stream of position snapshots into "arrived" and "departed".
 *
 * There is no arrivals/departures API here - only where every aircraft is
 * right now - so a movement is an *edge*: the same aircraft seen on the ground
 * at the field after being airborne, or the reverse. Everything awkward in
 * this file exists to stop that edge firing when nothing actually happened.
 */

import type { Aircraft } from "./adsb.js";
import type { Route } from "./routes.js";

export type Presence = "ground" | "airborne";
export type Movement = "arrived" | "departed";

export interface Airport {
  /** Shown in the message text, e.g. "ZQN". */
  code: string;
  lat: number;
  lon: number;
  /**
   * How close "on the ground *here*" is. An aircraft sitting on the ground
   * anywhere further out is on someone else's strip - Cromwell is ~25nm inside
   * a 30nm watch circle - and is ignored rather than tracked, so its eventual
   * arrival here is still detected cleanly.
   */
  groundRadiusNm: number;
}

export interface DetectedMovement {
  hex: string;
  ident: string;
  movement: Movement;
}

const NM_PER_KM = 0.539957;

/**
 * Great-circle distance in nautical miles.
 *
 * The API hands back a `dst` field that would save this, but `dst` is measured
 * from whatever point was *queried*, not from the airport. The day someone
 * widens the search by moving the query point, `dst` quietly stops meaning
 * "distance to the field" and every ground test starts lying. Computing it
 * from the airport itself cannot drift that way.
 */
export function distanceNm(
  lat1: number,
  lon1: number,
  lat2: number,
  lon2: number,
): number {
  const r = 6371.0;
  const p1 = (lat1 * Math.PI) / 180;
  const p2 = (lat2 * Math.PI) / 180;
  const dPhi = ((lat2 - lat1) * Math.PI) / 180;
  const dLambda = ((lon2 - lon1) * Math.PI) / 180;
  const a =
    Math.sin(dPhi / 2) ** 2 +
    Math.cos(p1) * Math.cos(p2) * Math.sin(dLambda / 2) ** 2;
  return 2 * r * Math.asin(Math.sqrt(a)) * NM_PER_KM;
}

/** Callsign, else registration, else the ICAO hex. Callsigns arrive padded. */
export function identify(ac: Aircraft): string {
  const flight = ac.flight?.trim();
  if (flight !== undefined && flight !== "") return flight;
  const reg = ac.r?.trim();
  if (reg !== undefined && reg !== "") return reg;
  return ac.hex.toUpperCase();
}

/**
 * Where this contact counts as being, or `null` for "not something to track".
 *
 * `null` is not the same as "not here": a contact that returns null leaves the
 * tracker's memory of it untouched, so an aircraft that lands away and comes
 * back is still remembered as airborne and its arrival here still fires.
 */
export function presenceOf(ac: Aircraft, airport: Airport): Presence | null {
  // Ground infrastructure, not aircraft - see Aircraft.type. Six of these sit
  // inside NZQN's 30nm circle, permanently "on the ground".
  if (ac.type !== undefined && ac.type.endsWith("_nt")) return null;
  if (typeof ac.lat !== "number" || typeof ac.lon !== "number") return null;

  if (ac.alt_baro === "ground") {
    const d = distanceNm(airport.lat, airport.lon, ac.lat, ac.lon);
    return d <= airport.groundRadiusNm ? "ground" : null;
  }
  if (typeof ac.alt_baro === "number") return "airborne";
  return null;
}

export interface DetectorOptions {
  airport: Airport;
  /**
   * How long to suppress a repeat of the same aircraft's same movement.
   * Circuits and a bounced landing both produce real ground/air edges that
   * nobody wants announced twice.
   */
  cooldownMs: number;
  /**
   * Drop an aircraft's remembered state after this long unseen, so the maps do
   * not grow without bound. Forgetting only ever costs a missed announcement,
   * never invents one: a forgotten aircraft is re-baselined in silence.
   */
  forgetMs: number;
}

/**
 * Deliberately not built on `feed-kit`'s `ChangeTracker`, which the other feeds
 * use and `services/README.md` tells you to reach for. Two of its choices are
 * wrong here and neither can be configured away:
 *
 *  - it reports `true` on first sight, which is right for a sensor (report the
 *    reading you found) and wrong for a movement - it would announce every
 *    aircraft parked on the apron as a fresh arrival the moment the feed
 *    starts, which is the noise the rule exists to prevent;
 *  - it stores only a value, with no record of when it was last seen, and
 *    this has to expire aircraft it stops hearing from.
 *
 * The rule behind ChangeTracker - report edges, never the persisting state -
 * is obeyed; only the implementation differs.
 */
export class MovementDetector {
  private readonly state = new Map<
    string,
    { presence: Presence; seenAt: number }
  >();
  private readonly announced = new Map<string, number>();

  constructor(private readonly opts: DetectorOptions) {}

  /** The movements worth announcing from this snapshot. */
  update(aircraft: Aircraft[], now: number): DetectedMovement[] {
    this.prune(now);

    const movements: DetectedMovement[] = [];

    for (const ac of aircraft) {
      const presence = presenceOf(ac, this.opts.airport);
      if (presence === null) continue;

      const previous = this.state.get(ac.hex);
      this.state.set(ac.hex, { presence, seenAt: now });

      // First time we have placed this aircraft: record where it is and say
      // nothing. We have no idea whether it just got there.
      if (previous === undefined) continue;
      if (previous.presence === presence) continue;

      const movement: Movement = presence === "ground" ? "arrived" : "departed";
      const key = `${ac.hex}:${movement}`;
      const last = this.announced.get(key);
      if (last !== undefined && now - last < this.opts.cooldownMs) continue;
      this.announced.set(key, now);

      movements.push({ hex: ac.hex, ident: identify(ac), movement });
    }

    return movements;
  }

  private prune(now: number): void {
    for (const [hex, s] of this.state) {
      if (now - s.seenAt >= this.opts.forgetMs) this.state.delete(hex);
    }
    for (const [key, at] of this.announced) {
      if (now - at >= this.opts.cooldownMs) this.announced.delete(key);
    }
  }
}

/**
 * e.g. `ANZ611 landed at ZQN` or, with a route on file,
 * `ANZ611 landed at ZQN from AUCKLAND`. Kept short on purpose: the device
 * types the bubble out at ~45ms a character and holds it for 30s afterwards -
 * `route` is omitted (`undefined` or `null`) whenever adsbdb has nothing for
 * this callsign, and the message just says less.
 */
export function movementText(
  m: DetectedMovement,
  airportCode: string,
  route?: Route | null,
): string {
  if (m.movement === "arrived") {
    return route
      ? `${m.ident} landed at ${airportCode} from ${route.origin}`
      : `${m.ident} landed at ${airportCode}`;
  }
  return route
    ? `${m.ident} left ${airportCode} for ${route.destination}`
    : `${m.ident} left ${airportCode}`;
}
