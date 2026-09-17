import { describe, expect, it } from "vitest";
import { presentationFor } from "../src/triage.js";

describe("triage", () => {
  it("keeps ambient readings silent and dark", () => {
    const p = presentationFor("sensor.reading", "low");
    expect(p.jingle).toBe("");
    expect(p.led).toBe("");
    expect(p.expression).toBe("neutral");
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

  it("strips the jingle at low priority", () => {
    expect(presentationFor("alert", "low").jingle).toBe("");
  });

  it("emits only lowercase expressions", () => {
    for (const kind of ["ambient", "notice", "alert", "celebrate"]) {
      const p = presentationFor(kind, "normal");
      expect(p.expression).toBe(p.expression.toLowerCase());
    }
  });
});
