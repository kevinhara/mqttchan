import { describe, expect, it } from "vitest";
import { isWarningLevel, matchesRegion } from "../src/warnings.js";

describe("matchesRegion", () => {
  const KEYWORDS = ["Otago", "Queenstown", "Crown Range"];

  it("matches case-insensitively against a real MetService areaDesc", () => {
    // Verified live 2026-09-19 against alerts.metservice.com/cap/atom.
    expect(matchesRegion("Crown Range Road", KEYWORDS)).toBe(true);
    expect(matchesRegion("crown range road", KEYWORDS)).toBe(true);
  });

  it("does not match an unrelated region", () => {
    expect(
      matchesRegion("Hawke's Bay south of Waipawa and the Tararua District", KEYWORDS),
    ).toBe(false);
  });

  it("matches a substring anywhere in the description", () => {
    expect(matchesRegion("Otago Peninsula and Dunedin City", KEYWORDS)).toBe(true);
  });
});

describe("isWarningLevel", () => {
  it("is true for a Warning headline", () => {
    expect(isWarningLevel("Heavy Rain Warning")).toBe(true);
    expect(isWarningLevel("Road Snowfall Warning")).toBe(true);
  });

  it("is false for a Watch, which is one rung below a Warning", () => {
    expect(isWarningLevel("Strong Wind Watch")).toBe(false);
  });

  it("is case-insensitive", () => {
    expect(isWarningLevel("severe thunderstorm warning")).toBe(true);
  });
});
