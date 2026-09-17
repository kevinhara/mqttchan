import { describe, expect, it } from "vitest";
import { encodedSize, finalisePayload, isValidLed, PayloadError } from "../src/device.js";
import type { DevicePayload } from "../src/device.js";

const base: DevicePayload = {
  text: "hello",
  expression: "neutral",
  led: "",
  blink: false,
  jingle: "",
};

describe("led validation", () => {
  it("accepts empty, cycle and 6-digit hex", () => {
    expect(isValidLed("")).toBe(true);
    expect(isValidLed("cycle")).toBe(true);
    expect(isValidLed("#ff8800")).toBe(true);
    expect(isValidLed("#FF8800")).toBe(true);
  });

  it("rejects shorthand hex and names", () => {
    // The firmware only parses the exact 6-digit form; "red" is accepted there
    // as a legacy alias but the API always emits hex.
    expect(isValidLed("#fff")).toBe(false);
    expect(isValidLed("red")).toBe(false);
    expect(isValidLed("#ff88000")).toBe(false);
  });
});

describe("finalisePayload", () => {
  it("refuses empty text, which the device would silently ignore", () => {
    expect(() => finalisePayload({ ...base, text: "   " }, 512)).toThrow(PayloadError);
  });

  it("rejects an invalid expression rather than letting the device fall back", () => {
    const bad = { ...base, expression: "Happy" as never };
    expect(() => finalisePayload(bad, 512)).toThrow(PayloadError);
  });

  it("passes a payload that already fits through untouched", () => {
    const out = finalisePayload({ ...base, text: "Office 20.1C" }, 512);
    expect(out.text).toBe("Office 20.1C");
  });

  it("trims long text at a word boundary to fit the byte budget", () => {
    const long = Array.from({ length: 200 }, (_, i) => `word${i}`).join(" ");
    const out = finalisePayload({ ...base, text: long }, 512);
    expect(encodedSize(out)).toBeLessThanOrEqual(512);
    expect(out.text.endsWith("…")).toBe(true);
    // Trimmed at a word boundary, so no partial word before the ellipsis.
    expect(out.text).toMatch(/word\d+…$/);
  });

  it("keeps multi-byte text within the BYTE budget, not the char count", () => {
    const out = finalisePayload({ ...base, text: "°".repeat(400) }, 512);
    expect(encodedSize(out)).toBeLessThanOrEqual(512);
  });
});
