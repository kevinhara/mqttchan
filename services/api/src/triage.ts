/**
 * The single place presentation is decided: semantic (kind, priority) in,
 * device fields out.
 *
 * Feeds never pick an expression or a color. That is what lets the device
 * contract change - as it did for contract v2 - without touching a feed, and
 * what keeps the device's voice consistent across sources instead of each feed
 * inventing its own.
 */

import type { Priority } from "@mqttchan/contract";
import type { Expression, Jingle, Led } from "./device.js";

export interface Presentation {
  expression: Expression;
  led: Led;
  blink: boolean;
  jingle: "" | Jingle;
}

const QUIET: Presentation = {
  expression: "neutral",
  led: "",
  blink: false,
  jingle: "",
};

/**
 * Known kinds. Unknown kinds fall back to QUIET rather than throwing - a feed
 * inventing a new kind should produce a plain message, not a 500.
 */
const BY_KIND: Record<string, Presentation> = {
  /** Background information nobody needs to act on. Deliberately silent. */
  ambient: QUIET,
  /** A sensor value changed. Same treatment: seen, not announced. */
  "sensor.reading": QUIET,
  /**
   * Something worth looking up for. `boarding` (added 2026-09-18) is the
   * default here rather than `chime` because the flights feed is currently
   * the only source of `notice` messages - a discreet PA-style chime for an
   * aircraft movement, not yet re-tuned for any other `notice` producer.
   */
  notice: {
    expression: "happy",
    led: "#3399ff",
    blink: false,
    jingle: "boarding",
  },
  /** Something wrong. The only kind that blinks. */
  alert: {
    expression: "doubt",
    led: "#ff0000",
    blink: true,
    jingle: "alert",
  },
  /** Good news. */
  celebrate: {
    expression: "happy",
    led: "cycle",
    blink: false,
    jingle: "fanfare",
  },
};

export function knownKinds(): string[] {
  return Object.keys(BY_KIND);
}

/**
 * Priority modifies the kind's presentation rather than replacing it:
 *   - `high` guarantees the message is noticeable even if its kind is quiet.
 *   - `low` strips the jingle, so ambient chatter never makes noise.
 * Ordering within the queue is handled separately, in queue.ts.
 */
export function presentationFor(kind: string, priority: Priority): Presentation {
  const base = BY_KIND[kind] ?? QUIET;

  if (priority === "high") {
    return {
      ...base,
      led: base.led === "" ? "#ff0000" : base.led,
      jingle: base.jingle === "" ? "alert" : base.jingle,
    };
  }
  if (priority === "low") {
    return { ...base, jingle: "" };
  }
  return base;
}
