import { describe, expect, it } from "vitest";
import type { Aircraft } from "../src/adsb.js";
import {
  MovementDetector,
  distanceNm,
  identify,
  movementText,
  presenceOf,
  type Airport,
} from "../src/movements.js";

const ZQN: Airport = {
  code: "ZQN",
  lat: -45.0211,
  lon: 168.7392,
  groundRadiusNm: 1.5,
};

/** Real contacts from a live adsb.lol 30nm point query on NZQN, 2026-09-18. */
const REAL = {
  /** Air NZ A321neo descending through 6825ft, 14nm out - an arrival inbound. */
  anz611: {
    hex: "c829ac",
    type: "adsb_icao",
    flight: "ANZ611  ",
    r: "ZK-OYB",
    t: "A21N",
    alt_baro: 6825,
    gs: 190.1,
    lat: -44.985626,
    lon: 169.06893,
  } satisfies Aircraft,
  /** An ADS-B ground station that self-identifies as TWR, permanently
   *  "on the ground" 2.9nm from the field. Six of these are in range. */
  tower: {
    hex: "c87eec",
    type: "adsb_icao_nt",
    r: "TWR",
    t: "TWR",
    alt_baro: "ground",
    lat: -44.984058,
    lon: 168.782812,
  } satisfies Aircraft,
  /** Helicopter over the field at 1750ft. */
  zkhgj: {
    hex: "c82b42",
    type: "adsb_icao",
    flight: "ZKHGJ   ",
    alt_baro: 1750,
    gs: 77.9,
    lat: -45.019831,
    lon: 168.738009,
  } satisfies Aircraft,
} as const;

/** The same aircraft, parked on the apron. */
function onGroundAtZqn(ac: Aircraft): Aircraft {
  return { ...ac, alt_baro: "ground", lat: -45.0208, lon: 168.7395 };
}

describe("distanceNm", () => {
  it("matches a known distance", () => {
    // Queenstown -> Dunedin is 172km; the predecessor service verified the
    // same figure against the same pair of coordinates.
    const km = distanceNm(-45.0312, 168.6626, -45.8788, 170.5028) / 0.539957;
    expect(km).toBeGreaterThan(168);
    expect(km).toBeLessThan(176);
  });

  it("is ~0 for the same point", () => {
    expect(distanceNm(ZQN.lat, ZQN.lon, ZQN.lat, ZQN.lon)).toBeCloseTo(0, 6);
  });
});

describe("identify", () => {
  it("trims the API's padded callsign", () => {
    expect(identify(REAL.anz611)).toBe("ANZ611");
  });

  it("falls back to registration, then to hex", () => {
    expect(identify({ hex: "c829ac", r: "ZK-OYB" })).toBe("ZK-OYB");
    expect(identify({ hex: "c829ac" })).toBe("C829AC");
    expect(identify({ hex: "c829ac", flight: "   " })).toBe("C829AC");
  });
});

describe("presenceOf", () => {
  it("ignores non-transponder ground infrastructure", () => {
    // The whole point: without this, six permanently-grounded beacons look
    // like aircraft sitting at the airport.
    expect(presenceOf(REAL.tower, ZQN)).toBeNull();
  });

  it("reads a numeric altitude as airborne", () => {
    expect(presenceOf(REAL.anz611, ZQN)).toBe("airborne");
    expect(presenceOf(REAL.zkhgj, ZQN)).toBe("airborne");
  });

  it("reads the ground bit at the field as on the ground", () => {
    expect(presenceOf(onGroundAtZqn(REAL.anz611), ZQN)).toBe("ground");
  });

  it("ignores an aircraft on the ground at another strip", () => {
    // Cromwell, ~25nm away and inside the 30nm watch circle.
    const elsewhere: Aircraft = {
      ...REAL.anz611,
      alt_baro: "ground",
      lat: -45.0384,
      lon: 169.1966,
    };
    expect(presenceOf(elsewhere, ZQN)).toBeNull();
  });

  it("ignores a contact with no position", () => {
    expect(presenceOf({ hex: "abc123", alt_baro: 3000 }, ZQN)).toBeNull();
  });
});

function detector(): MovementDetector {
  return new MovementDetector({
    airport: ZQN,
    cooldownMs: 10 * 60_000,
    forgetMs: 45 * 60_000,
  });
}

describe("MovementDetector", () => {
  it("says nothing on a first sighting, airborne or parked", () => {
    const d = detector();
    expect(d.update([REAL.anz611, onGroundAtZqn(REAL.zkhgj)], 0)).toEqual([]);
  });

  it("reports an arrival when an airborne aircraft reaches the ground", () => {
    const d = detector();
    d.update([REAL.anz611], 0);
    const found = d.update([onGroundAtZqn(REAL.anz611)], 60_000);
    expect(found).toEqual([
      { hex: "c829ac", ident: "ANZ611", movement: "arrived" },
    ]);
  });

  it("reports a departure when a parked aircraft gets airborne", () => {
    const d = detector();
    d.update([onGroundAtZqn(REAL.anz611)], 0);
    const found = d.update([REAL.anz611], 60_000);
    expect(found).toEqual([
      { hex: "c829ac", ident: "ANZ611", movement: "departed" },
    ]);
  });

  it("does not repeat while the aircraft stays put", () => {
    // This is the failure the predecessor service was switched off for: its
    // ISS producer re-announced on every poll while the condition held.
    const d = detector();
    d.update([REAL.anz611], 0);
    expect(d.update([onGroundAtZqn(REAL.anz611)], 60_000)).toHaveLength(1);
    for (let t = 2; t < 10; t++) {
      expect(d.update([onGroundAtZqn(REAL.anz611)], t * 60_000)).toEqual([]);
    }
  });

  it("suppresses a repeat movement inside the cooldown", () => {
    const d = detector();
    d.update([onGroundAtZqn(REAL.zkhgj)], 0);
    // Circuit: up, down, up again - the second departure is inside 10 minutes.
    expect(d.update([REAL.zkhgj], 60_000)).toHaveLength(1);
    expect(d.update([onGroundAtZqn(REAL.zkhgj)], 120_000)).toHaveLength(1);
    expect(d.update([REAL.zkhgj], 180_000)).toEqual([]);
  });

  it("allows the same movement again once the cooldown has passed", () => {
    const d = detector();
    d.update([onGroundAtZqn(REAL.zkhgj)], 0);
    expect(d.update([REAL.zkhgj], 60_000)).toHaveLength(1);
    expect(d.update([onGroundAtZqn(REAL.zkhgj)], 120_000)).toHaveLength(1);
    expect(d.update([REAL.zkhgj], 11 * 60_000)).toHaveLength(1);
  });

  it("re-baselines in silence after forgetting an aircraft", () => {
    // Forgetting must only ever cost a missed announcement, never invent one.
    const d = detector();
    d.update([REAL.anz611], 0);
    const later = 60 * 60_000;
    expect(d.update([onGroundAtZqn(REAL.anz611)], later)).toEqual([]);
  });

  it("still catches the arrival of an aircraft that landed away and returned", () => {
    // An ignored sample (on the ground elsewhere) must not overwrite the
    // remembered airborne state, or this arrival reads as a first sighting.
    const d = detector();
    d.update([REAL.anz611], 0);
    const atCromwell: Aircraft = {
      ...REAL.anz611,
      alt_baro: "ground",
      lat: -45.0384,
      lon: 169.1966,
    };
    expect(d.update([atCromwell], 60_000)).toEqual([]);
    expect(d.update([onGroundAtZqn(REAL.anz611)], 120_000)).toEqual([
      { hex: "c829ac", ident: "ANZ611", movement: "arrived" },
    ]);
  });

  it("never fires for ground infrastructure", () => {
    const d = detector();
    for (let t = 0; t < 5; t++) {
      expect(d.update([REAL.tower], t * 60_000)).toEqual([]);
    }
  });

  it("tracks two aircraft independently", () => {
    const d = detector();
    d.update([REAL.anz611, onGroundAtZqn(REAL.zkhgj)], 0);
    const found = d.update(
      [onGroundAtZqn(REAL.anz611), REAL.zkhgj],
      60_000,
    );
    expect(found).toEqual([
      { hex: "c829ac", ident: "ANZ611", movement: "arrived" },
      { hex: "c82b42", ident: "ZKHGJ", movement: "departed" },
    ]);
  });
});

describe("movementText", () => {
  it("stays short enough for the device to type out", () => {
    const arrived = movementText(
      { hex: "c829ac", ident: "ANZ611", movement: "arrived" },
      "ZQN",
    );
    expect(arrived).toBe("ANZ611 landed at ZQN");
    expect(
      movementText({ hex: "c82b42", ident: "ZKHGJ", movement: "departed" }, "ZQN"),
    ).toBe("ZKHGJ left ZQN");
    // ~45ms a character on the device, so length is a real cost.
    expect(arrived.length).toBeLessThan(32);
  });
});
