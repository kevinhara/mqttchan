/**
 * Turning astro.ts's raw values into the one daily message the device shows.
 * Pure and network-free, same reasoning as astro.ts itself - so this is
 * tested with fixed inputs rather than a live sky.
 */

import type { MoonTimes, PlanetSighting, SunTimes } from "./astro.js";
import type { DailyOutlook } from "./weather.js";

/**
 * Builds "6:39am" by hand from `Intl.DateTimeFormat` parts rather than
 * trusting its formatted string directly. `en-NZ`'s default rendering joins
 * the hour and the am/pm marker with a narrow no-break space (U+202F), a
 * multi-byte character - and `speech_bubble.h`'s typewriter reveal prints
 * one raw byte at a time, which splits any multi-byte UTF-8 character apart
 * before either half reaches the font. That is the exact failure the
 * ha-temperature feed already hit with the degree sign; this sidesteps it
 * the same way, by never emitting anything but plain ASCII.
 */
export function formatTime(date: Date, timeZone: string): string {
  const parts = new Intl.DateTimeFormat("en-NZ", {
    timeZone,
    hour: "numeric",
    minute: "2-digit",
    hour12: true,
  }).formatToParts(date);
  const hour = parts.find((p) => p.type === "hour")?.value ?? "?";
  const minute = parts.find((p) => p.type === "minute")?.value ?? "??";
  const dayPeriod = (parts.find((p) => p.type === "dayPeriod")?.value ?? "").toLowerCase();
  return `${hour}:${minute}${dayPeriod}`;
}

/** "sunrise 6:39am, sunset 6:38pm", soonest event first. Drops whichever
 *  half is missing rather than guessing - `SearchRiseSet` returns null for a
 *  body that does not rise or set within the search window, which does
 *  happen at high latitudes and is worth showing honestly rather than
 *  silently. */
function orderedPair(
  timeZone: string,
  a: { label: string; date: Date | null },
  b: { label: string; date: Date | null },
): string {
  const present = [a, b].filter(
    (x): x is { label: string; date: Date } => x.date !== null,
  );
  present.sort((x, y) => x.date.getTime() - y.date.getTime());
  return present.map((x) => `${x.label} ${formatTime(x.date, timeZone)}`).join(", ");
}

export interface DigestInput {
  locationName: string;
  timeZone: string;
  sun: SunTimes;
  moon: MoonTimes;
  moonPhase: string;
  /** 0 to 1, as returned by `moonIllumination`. */
  moonIlluminationFraction: number;
  planets: PlanetSighting[];
  /** Omitted entirely when `METSERVICE_API_KEY` isn't set - see weather.ts. */
  weather?: DailyOutlook;
}

/** Rain under 1mm reads as "no rain expected" rather than as a number
 *  nobody would notice missing outdoors. */
const NOTICEABLE_RAIN_MM = 1;

export function formatDigest(input: DigestInput): string {
  const clauses: string[] = [];

  if (input.weather) {
    const w = input.weather;
    clauses.push(`high ${Math.round(w.maxC)}C, low ${Math.round(w.minC)}C, ${w.cloud}`);
    if (w.precipMm >= NOTICEABLE_RAIN_MM) {
      clauses.push(`${Math.round(w.precipMm)}mm rain expected`);
    }
    clauses.push(
      `${w.windDirection} wind to ${Math.round(w.windKmh)}km/h, gusts ${Math.round(w.gustKmh)}km/h`,
    );
  }

  const sunClause = orderedPair(
    input.timeZone,
    { label: "sunrise", date: input.sun.sunrise },
    { label: "sunset", date: input.sun.sunset },
  );
  if (sunClause !== "") clauses.push(sunClause);

  const moonTimesClause = orderedPair(
    input.timeZone,
    { label: "moonrise", date: input.moon.moonrise },
    { label: "moonset", date: input.moon.moonset },
  );
  const litPct = Math.round(input.moonIlluminationFraction * 100);
  const moonClause = [
    `moon ${input.moonPhase.toLowerCase()}`,
    `${litPct}% lit`,
    moonTimesClause,
  ]
    .filter((s) => s !== "")
    .join(", ");
  clauses.push(moonClause);

  const evening = input.planets.filter((p) => p.when === "evening").map((p) => p.name);
  const morning = input.planets.filter((p) => p.when === "morning").map((p) => p.name);
  if (evening.length > 0) clauses.push(`${evening.join("/")} visible at dusk`);
  if (morning.length > 0) clauses.push(`${morning.join("/")} visible before dawn`);

  return `${input.locationName}: ${clauses.join(". ")}.`;
}
