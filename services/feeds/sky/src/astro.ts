/**
 * Sun, moon and naked-eye-planet positions for one location.
 *
 * Unlike every network-backed module in this repo, astronomy-engine is a
 * deterministic offline ephemeris - same inputs, same outputs, no fetch
 * involved - so this whole module is pure and is tested directly against
 * known real dates, the same reasoning as `flights/src/movements.ts`.
 */

import * as Astronomy from "astronomy-engine";

export interface Location {
  name: string;
  lat: number;
  lon: number;
  elevationM: number;
}

export interface SunTimes {
  sunrise: Date | null;
  sunset: Date | null;
}

export interface MoonTimes {
  moonrise: Date | null;
  moonset: Date | null;
}

export interface PlanetSighting {
  name: string;
  when: "evening" | "morning";
}

const RISE = 1;
const SET = -1;

/** The sun 6 degrees below the horizon - civil twilight. Bright planets are
 *  already visible against a dark-enough sky by this point. */
const TWILIGHT_ALTITUDE = -6;

/** Below this a planet is lost in horizon haze even when technically "up".
 *  Also does the job of a glare/elongation check for free: a planet too
 *  close to the sun to be worth mentioning is also too low at twilight to
 *  clear this bar, so no separate check is needed. */
const MIN_PLANET_ALTITUDE = 5;

const NAKED_EYE_PLANETS = ["Mercury", "Venus", "Mars", "Jupiter", "Saturn"] as const;

export function observerFor(loc: Location): Astronomy.Observer {
  return new Astronomy.Observer(loc.lat, loc.lon, loc.elevationM);
}

/** The next sunrise and sunset from `from`. Whichever is closer comes back
 *  chronologically first - during the day that's the coming sunset, at
 *  night it's the coming sunrise. */
export function sunTimes(observer: Astronomy.Observer, from: Date): SunTimes {
  return {
    sunrise: Astronomy.SearchRiseSet(Astronomy.Body.Sun, observer, RISE, from, 2)?.date ?? null,
    sunset: Astronomy.SearchRiseSet(Astronomy.Body.Sun, observer, SET, from, 2)?.date ?? null,
  };
}

export function moonTimes(observer: Astronomy.Observer, from: Date): MoonTimes {
  return {
    moonrise: Astronomy.SearchRiseSet(Astronomy.Body.Moon, observer, RISE, from, 2)?.date ?? null,
    moonset: Astronomy.SearchRiseSet(Astronomy.Body.Moon, observer, SET, from, 2)?.date ?? null,
  };
}

const PHASE_NAMES = [
  "New Moon",
  "Waxing Crescent",
  "First Quarter",
  "Waxing Gibbous",
  "Full Moon",
  "Waning Gibbous",
  "Last Quarter",
  "Waning Crescent",
] as const;

/**
 * Buckets the moon's ecliptic longitude (0-360 degrees, from
 * `Astronomy.MoonPhase`) into the eight named phases, each a 45-degree wedge
 * centred on its exact angle: 0 = new, 90 = first quarter, 180 = full,
 * 270 = last quarter.
 */
export function moonPhaseName(angleDeg: number): string {
  const normalized = ((angleDeg % 360) + 360) % 360;
  const index = Math.round(normalized / 45) % 8;
  return PHASE_NAMES[index] as string;
}

export function moonAngle(date: Date): number {
  return Astronomy.MoonPhase(date);
}

/** Fraction of the moon's visible disc that is lit, 0 to 1. */
export function moonIllumination(date: Date): number {
  return Astronomy.Illumination(Astronomy.Body.Moon, date).phase_fraction;
}

function altitudeOf(body: Astronomy.Body, observer: Astronomy.Observer, date: Date): number {
  const equator = Astronomy.Equator(body, date, observer, true, true);
  return Astronomy.Horizon(date, observer, equator.ra, equator.dec, "normal").altitude;
}

/**
 * Naked-eye planets above `MIN_PLANET_ALTITUDE` at dusk (evening sky) or the
 * dawn that follows it (morning sky) - i.e. for the one night starting at
 * `from`. A planet can appear twice (e.g. Venus low at dusk and higher at
 * the next dawn is not the usual case, but nothing here rules it out).
 */
export function visiblePlanets(observer: Astronomy.Observer, from: Date): PlanetSighting[] {
  const dusk = Astronomy.SearchAltitude(Astronomy.Body.Sun, observer, SET, from, 1, TWILIGHT_ALTITUDE);
  const dawn = dusk
    ? Astronomy.SearchAltitude(Astronomy.Body.Sun, observer, RISE, dusk.date, 1, TWILIGHT_ALTITUDE)
    : null;

  const sightings: PlanetSighting[] = [];
  for (const name of NAKED_EYE_PLANETS) {
    const body = Astronomy.Body[name];
    if (dusk && altitudeOf(body, observer, dusk.date) > MIN_PLANET_ALTITUDE) {
      sightings.push({ name, when: "evening" });
    }
    if (dawn && altitudeOf(body, observer, dawn.date) > MIN_PLANET_ALTITUDE) {
      sightings.push({ name, when: "morning" });
    }
  }
  return sightings;
}
