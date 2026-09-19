import { describe, expect, it } from "vitest";
import { formatDigest, formatTime } from "../src/format.js";

const NZT = "Pacific/Auckland";

describe("formatTime", () => {
  it("renders plain ASCII, no narrow-no-break-space am/pm marker", () => {
    // 2026-09-19T18:39:27Z is 2026-09-20T06:39:27 NZST (+12 in September).
    const text = formatTime(new Date("2026-09-19T18:39:27Z"), NZT);
    expect(text).toBe("6:39am");
    expect(/^[\x00-\x7F]*$/.test(text)).toBe(true);
  });

  it("renders an afternoon time", () => {
    const text = formatTime(new Date("2026-09-20T06:39:13Z"), NZT);
    expect(text).toBe("6:39pm");
  });
});

describe("formatDigest", () => {
  it("orders sun and moon events soonest-first and lists planets by when", () => {
    const text = formatDigest({
      locationName: "Queenstown, Otago, NZ",
      timeZone: NZT,
      sun: {
        sunrise: new Date("2026-09-19T18:39:27Z"),
        sunset: new Date("2026-09-20T06:39:13Z"),
      },
      moon: {
        moonrise: new Date("2026-09-19T23:28:17Z"),
        moonset: new Date("2026-09-19T15:19:43Z"),
      },
      moonPhase: "First Quarter",
      moonIlluminationFraction: 0.5614,
      planets: [
        { name: "Mercury", when: "evening" },
        { name: "Venus", when: "evening" },
        { name: "Mars", when: "morning" },
      ],
    });

    expect(text).toBe(
      "Queenstown, Otago, NZ: sunrise 6:39am, sunset 6:39pm. " +
        "moon first quarter, 56% lit, moonset 3:19am, moonrise 11:28am. " +
        "Mercury/Venus visible at dusk. Mars visible before dawn.",
    );
  });

  it("drops a missing half of a pair instead of guessing", () => {
    const text = formatDigest({
      locationName: "Queenstown, Otago, NZ",
      timeZone: NZT,
      sun: { sunrise: new Date("2026-09-19T18:39:27Z"), sunset: null },
      moon: { moonrise: null, moonset: null },
      moonPhase: "New Moon",
      moonIlluminationFraction: 0,
      planets: [],
    });

    expect(text).toBe("Queenstown, Otago, NZ: sunrise 6:39am. moon new moon, 0% lit.");
  });

  it("leads with a weather clause when an outlook is given", () => {
    const text = formatDigest({
      locationName: "Queenstown, Otago, NZ",
      timeZone: NZT,
      sun: { sunrise: new Date("2026-09-19T18:39:27Z"), sunset: null },
      moon: { moonrise: null, moonset: null },
      moonPhase: "New Moon",
      moonIlluminationFraction: 0,
      planets: [],
      weather: {
        minC: 1.7,
        maxC: 6.6,
        precipMm: 6.4,
        windKmh: 10.2,
        gustKmh: 64.5,
        windDirection: "WNW",
        cloud: "overcast",
      },
    });

    expect(text).toBe(
      "Queenstown, Otago, NZ: high 7C, low 2C, overcast. 6mm rain expected. " +
        "WNW wind to 10km/h, gusts 65km/h. sunrise 6:39am. moon new moon, 0% lit.",
    );
  });

  it("omits the rain clause below the noticeable threshold", () => {
    const text = formatDigest({
      locationName: "Queenstown, Otago, NZ",
      timeZone: NZT,
      sun: { sunrise: new Date("2026-09-19T18:39:27Z"), sunset: null },
      moon: { moonrise: null, moonset: null },
      moonPhase: "New Moon",
      moonIlluminationFraction: 0,
      planets: [],
      weather: {
        minC: 1.7,
        maxC: 6.6,
        precipMm: 0.2,
        windKmh: 10.2,
        gustKmh: 20.5,
        windDirection: "NW",
        cloud: "clear",
      },
    });

    expect(text).toBe(
      "Queenstown, Otago, NZ: high 7C, low 2C, clear. NW wind to 10km/h, gusts 21km/h. " +
        "sunrise 6:39am. moon new moon, 0% lit.",
    );
  });
});
