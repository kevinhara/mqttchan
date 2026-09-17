import { describe, expect, it } from "vitest";
import { ChangeTracker } from "../src/change.js";
import { WarnOnce } from "../src/warn.js";

describe("ChangeTracker", () => {
  it("fires on the first reading of a key", () => {
    expect(new ChangeTracker().changedBy("office", 20.1, 0.5)).toBe(true);
  });

  it("suppresses changes below the threshold", () => {
    const t = new ChangeTracker();
    t.changedBy("office", 20.0, 0.5);
    expect(t.changedBy("office", 20.2, 0.5)).toBe(false);
    expect(t.changedBy("office", 20.4, 0.5)).toBe(false);
  });

  it("fires once the value has moved far enough", () => {
    const t = new ChangeTracker();
    t.changedBy("office", 20.0, 0.5);
    expect(t.changedBy("office", 20.5, 0.5)).toBe(true);
  });

  it("measures drift from the last REPORTED value, not the last seen one", () => {
    const t = new ChangeTracker();
    t.changedBy("office", 20.0, 0.5);
    t.changedBy("office", 20.3, 0.5); // suppressed, and must not become the baseline
    expect(t.changedBy("office", 20.5, 0.5)).toBe(true);
  });

  it("tracks keys independently", () => {
    const t = new ChangeTracker();
    t.changedBy("office", 20.0, 0.5);
    expect(t.changedBy("shed", 9.0, 0.5)).toBe(true);
  });
});

describe("WarnOnce", () => {
  it("logs once per key and then stays quiet", () => {
    const lines: string[] = [];
    const original = console.log;
    console.log = (msg: string) => { lines.push(msg); };
    try {
      const w = new WarnOnce();
      w.warn("cfg", "not configured");
      w.warn("cfg", "not configured");
      w.warn("other", "also not configured");
    } finally {
      console.log = original;
    }
    expect(lines).toEqual(["not configured", "also not configured"]);
  });
});
