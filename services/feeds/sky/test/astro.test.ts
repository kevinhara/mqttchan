import { describe, expect, it } from "vitest";
import {
  moonAngle,
  moonIllumination,
  moonPhaseName,
  moonTimes,
  observerFor,
  sunTimes,
  visiblePlanets,
  type Location,
} from "../src/astro.js";

const QUEENSTOWN: Location = {
  name: "Queenstown, Otago, NZ",
  lat: -45.0311,
  lon: 168.6626,
  elevationM: 310,
};

/** A fixed reference instant, not "now" - astronomy-engine is a
 *  deterministic offline ephemeris, so every value below was computed once
 *  against this exact date and is expected to reproduce exactly. */
const FROM = new Date("2026-09-19T12:00:00Z");

describe("moonPhaseName", () => {
  it("buckets the eight named phases around their exact angle", () => {
    expect(moonPhaseName(0)).toBe("New Moon");
    expect(moonPhaseName(45)).toBe("Waxing Crescent");
    expect(moonPhaseName(90)).toBe("First Quarter");
    expect(moonPhaseName(135)).toBe("Waxing Gibbous");
    expect(moonPhaseName(180)).toBe("Full Moon");
    expect(moonPhaseName(225)).toBe("Waning Gibbous");
    expect(moonPhaseName(270)).toBe("Last Quarter");
    expect(moonPhaseName(315)).toBe("Waning Crescent");
  });

  it("wraps at the 360/0 boundary", () => {
    expect(moonPhaseName(359)).toBe("New Moon");
    expect(moonPhaseName(-10)).toBe("New Moon");
  });

  it("resolves a real angle to the wedge it falls in", () => {
    // 96.92 degrees, computed for FROM below - inside the 67.5-112.5 wedge.
    expect(moonPhaseName(96.92)).toBe("First Quarter");
  });
});

describe("sunTimes / moonTimes for Queenstown", () => {
  const observer = observerFor(QUEENSTOWN);

  it("finds the next sunrise and sunset", () => {
    const { sunrise, sunset } = sunTimes(observer, FROM);
    expect(sunrise?.toISOString()).toBe("2026-09-19T18:39:27.670Z");
    expect(sunset?.toISOString()).toBe("2026-09-20T06:39:13.156Z");
  });

  it("finds the next moonrise and moonset", () => {
    const { moonrise, moonset } = moonTimes(observer, FROM);
    expect(moonrise?.toISOString()).toBe("2026-09-19T23:28:17.278Z");
    expect(moonset?.toISOString()).toBe("2026-09-19T15:19:43.451Z");
  });
});

describe("moonAngle / moonIllumination for Queenstown", () => {
  it("matches the reference computation for FROM", () => {
    expect(moonAngle(FROM)).toBeCloseTo(96.9217, 3);
    expect(moonIllumination(FROM)).toBeCloseTo(0.5614, 3);
  });
});

describe("visiblePlanets for Queenstown", () => {
  it("finds the naked-eye planets above the horizon at dusk and dawn", () => {
    const observer = observerFor(QUEENSTOWN);
    const sightings = visiblePlanets(observer, FROM);

    // Reference altitudes at dusk (2026-09-20T07:08:36.941Z) and the
    // following dawn (2026-09-20T18:08:12.914Z): Mercury +11.0/-11.3, Venus
    // +31.6/-11.4, Mars -60.2/+15.0, Jupiter -42.1/+8.4, Saturn -6.4/+13.7.
    expect(sightings).toContainEqual({ name: "Mercury", when: "evening" });
    expect(sightings).toContainEqual({ name: "Venus", when: "evening" });
    expect(sightings).toContainEqual({ name: "Mars", when: "morning" });
    expect(sightings).toContainEqual({ name: "Jupiter", when: "morning" });
    expect(sightings).toContainEqual({ name: "Saturn", when: "morning" });
    expect(sightings).toHaveLength(5);
  });
});
