import { describe, expect, it } from "vitest";
import { cloudDescription, compassDirection, kelvinToC, msToKmh } from "../src/weather.js";

describe("kelvinToC", () => {
  it("converts a real sample from the live response", () => {
    // 2026-09-19, Queenstown: 279.71933K verified live via forecast-v2.metoceanapi.com.
    expect(kelvinToC(279.71933)).toBeCloseTo(6.57, 1);
  });
});

describe("msToKmh", () => {
  it("converts meters/second to km/h", () => {
    expect(msToKmh(10)).toBeCloseTo(36, 5);
  });
});

describe("compassDirection", () => {
  it("names the 16-point compass at each boundary", () => {
    expect(compassDirection(0)).toBe("N");
    expect(compassDirection(90)).toBe("E");
    expect(compassDirection(180)).toBe("S");
    expect(compassDirection(270)).toBe("W");
  });

  it("resolves a real bearing from the live response", () => {
    // 284.06137 degrees, verified live 2026-09-19.
    expect(compassDirection(284.06137)).toBe("WNW");
  });

  it("wraps at the 360/0 boundary", () => {
    expect(compassDirection(359)).toBe("N");
    expect(compassDirection(-10)).toBe("N");
  });
});

describe("cloudDescription", () => {
  it("buckets into clear, partly cloudy, overcast", () => {
    expect(cloudDescription(0)).toBe("clear");
    expect(cloudDescription(19)).toBe("clear");
    expect(cloudDescription(20)).toBe("partly cloudy");
    expect(cloudDescription(69)).toBe("partly cloudy");
    expect(cloudDescription(70)).toBe("overcast");
    expect(cloudDescription(100)).toBe("overcast");
  });
});
