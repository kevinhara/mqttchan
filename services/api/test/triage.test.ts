import { describe, expect, it } from "vitest";
import { presentationFor } from "../src/triage.js";

describe("triage", () => {
  it("keeps ambient readings silent and dark", () => {
    const p = presentationFor("ambient", "low");
    expect(p.jingle).toBe("");
    expect(p.led).toBe("");
    expect(p.expression).toBe("neutral");
  });

  it("gives a sensor reading a quiet beep and a green LED", () => {
    const p = presentationFor("sensor.reading", "low");
    expect(p.jingle).toBe("beep");
    expect(p.led).toBe("#00ff00");
    expect(p.blink).toBe(false);
  });

  it("falls back for an unknown kind instead of throwing", () => {
    const p = presentationFor("something.nobody.defined", "normal");
    expect(p.expression).toBe("neutral");
    expect(p.jingle).toBe("");
  });

  it("makes a high-priority message noticeable even if its kind is quiet", () => {
    const p = presentationFor("ambient", "high");
    expect(p.led).toBe("#ff0000");
    expect(p.jingle).toBe("alert");
  });

  it("does not override a kind's own presentation when escalating", () => {
    const p = presentationFor("celebrate", "high");
    expect(p.led).toBe("cycle");
    expect(p.jingle).toBe("fanfare");
  });

  it("leaves a kind's own jingle alone at low priority", () => {
    expect(presentationFor("alert", "low").jingle).toBe("alert");
  });

  it("emits only lowercase expressions", () => {
    for (const kind of ["ambient", "notice", "alert", "celebrate"]) {
      const p = presentationFor(kind, "normal");
      expect(p.expression).toBe(p.expression.toLowerCase());
    }
  });
});
