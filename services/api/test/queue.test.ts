import { beforeEach, describe, expect, it, vi } from "vitest";
import { MessageQueue } from "../src/queue.js";
import type { DevicePayload } from "../src/device.js";

/** Controllable clock so the timing rules can be tested without waiting. */
function harness() {
  let now = 1_000_000;
  const published: DevicePayload[] = [];
  const queue = new MessageQueue(
    async (p) => { published.push(p); },
    () => now,
  );
  return {
    queue,
    published,
    advance(ms: number) { now += ms; },
    get now() { return now; },
  };
}

describe("dedupe", () => {
  it("collapses two queued messages with the same key into one", async () => {
    const h = harness();
    expect(h.queue.submit({ source: "s", kind: "ambient", text: "Office 20.0C", dedupeKey: "office" }).decision).toBe("queued");
    expect(h.queue.submit({ source: "s", kind: "ambient", text: "Office 20.5C", dedupeKey: "office" }).decision).toBe("deduped");
    expect(h.queue.status().queueDepth).toBe(1);

    await h.queue.drain();
    // The newer value wins - you see the current reading, not the stale one.
    expect(h.published).toHaveLength(1);
    expect(h.published[0]?.text).toBe("Office 20.5C");
  });

  it("keeps distinct keys separate", () => {
    const h = harness();
    h.queue.submit({ source: "s", kind: "ambient", text: "a", dedupeKey: "one" });
    h.queue.submit({ source: "s", kind: "ambient", text: "b", dedupeKey: "two" });
    expect(h.queue.status().queueDepth).toBe(2);
  });
});

describe("pacing", () => {
  it("publishes one message then waits out the device's display time", async () => {
    const h = harness();
    h.queue.submit({ source: "s", kind: "ambient", text: "one" });
    h.queue.submit({ source: "s", kind: "ambient", text: "two" });

    await h.queue.drain();
    expect(h.published).toHaveLength(1);

    // Still busy: draining again immediately must not publish the second.
    await h.queue.drain();
    expect(h.published).toHaveLength(1);

    h.advance(60_000);
    await h.queue.drain();
    expect(h.published).toHaveLength(2);
  });

  it("gives a long message more room than a short one", () => {
    const h = harness();
    const short = h.queue.estimateDisplayMs({ text: "hi", led: "", blink: false, jingle: "" });
    const long = h.queue.estimateDisplayMs({ text: "x".repeat(200), led: "", blink: false, jingle: "" });
    // ~45ms/char of typing, so 198 extra chars is ~8.9s more.
    expect(long - short).toBeGreaterThan(8000);
  });

  it("counts the jingle against the gap", () => {
    const h = harness();
    const withJingle = h.queue.estimateDisplayMs({ text: "hi", led: "", blink: false, jingle: "alert" });
    const without = h.queue.estimateDisplayMs({ text: "hi", led: "", blink: false, jingle: "" });
    expect(withJingle).toBeGreaterThan(without);
  });
});

describe("priority", () => {
  it("lets a high-priority message overtake queued low-priority ones", async () => {
    const h = harness();
    h.queue.submit({ source: "s", kind: "ambient", text: "ambient one", priority: "low" });
    h.queue.submit({ source: "s", kind: "ambient", text: "ambient two", priority: "low" });
    h.queue.submit({ source: "s", kind: "alert", text: "something broke", priority: "high" });

    await h.queue.drain();
    expect(h.published[0]?.text).toBe("something broke");
  });

  it("stays FIFO within a priority", async () => {
    const h = harness();
    h.queue.submit({ source: "s", kind: "ambient", text: "first" });
    h.queue.submit({ source: "s", kind: "ambient", text: "second" });
    await h.queue.drain();
    h.advance(60_000);
    await h.queue.drain();
    expect(h.published.map((p) => p.text)).toEqual(["first", "second"]);
  });
});

describe("ttl", () => {
  it("drops a stale message rather than showing it late", async () => {
    const h = harness();
    h.queue.submit({ source: "s", kind: "ambient", text: "old news", ttlSeconds: 60 });
    h.advance(61_000);
    await h.queue.drain();
    expect(h.published).toHaveLength(0);
    expect(h.queue.status().totals.droppedExpired).toBe(1);
  });
});

describe("rate limiting", () => {
  it("drops once a source burns its bucket, and recovers as it refills", () => {
    const h = harness();
    const decisions: string[] = [];
    // Default bucket is 10 tokens refilling at 10/min.
    for (let i = 0; i < 12; i++) {
      decisions.push(h.queue.submit({ source: "noisy", kind: "ambient", text: `m${i}` }).decision);
    }
    expect(decisions.filter((d) => d === "dropped")).toHaveLength(2);

    h.advance(60_000);
    expect(h.queue.submit({ source: "noisy", kind: "ambient", text: "later" }).decision).toBe("queued");
  });

  it("budgets each source separately", () => {
    const h = harness();
    for (let i = 0; i < 12; i++) {
      h.queue.submit({ source: "noisy", kind: "ambient", text: `m${i}` });
    }
    // A different source is unaffected by the first one exhausting its bucket.
    expect(h.queue.submit({ source: "quiet", kind: "ambient", text: "hello" }).decision).toBe("queued");
  });
});

describe("failure handling", () => {
  it("keeps a message queued when the broker publish fails", async () => {
    let now = 1_000_000;
    const queue = new MessageQueue(async () => { throw new Error("broker down"); }, () => now);
    queue.submit({ source: "s", kind: "ambient", text: "important" });
    await expect(queue.drain()).rejects.toThrow("broker down");
    // Not silently eaten - still there for the next attempt.
    expect(queue.status().queueDepth).toBe(1);
  });
});
