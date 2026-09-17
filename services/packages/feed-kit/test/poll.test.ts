import { describe, expect, it, vi } from "vitest";
import { startPolling } from "../src/poll.js";

describe("startPolling", () => {
  it("survives a throwing tick and keeps polling", async () => {
    vi.useFakeTimers();
    const original = console.log;
    console.log = () => {};
    try {
      let calls = 0;
      const stop = startPolling({ name: "t", intervalMs: 1000 }, async () => {
        calls++;
        throw new Error("boom");
      });
      await vi.advanceTimersByTimeAsync(3500);
      stop();
      // Three ticks fired despite every one of them throwing.
      expect(calls).toBe(3);
    } finally {
      console.log = original;
      vi.useRealTimers();
    }
  });

  it("stops firing after stop()", async () => {
    vi.useFakeTimers();
    try {
      let calls = 0;
      const stop = startPolling({ name: "t", intervalMs: 1000 }, async () => { calls++; });
      await vi.advanceTimersByTimeAsync(1500);
      stop();
      await vi.advanceTimersByTimeAsync(5000);
      expect(calls).toBe(1);
    } finally {
      vi.useRealTimers();
    }
  });
});
