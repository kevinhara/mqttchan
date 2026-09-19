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
  /**
   * A sensor value changed - today, always a `ha-temperature` room reading
   * (see `services/feeds/ha-temperature`). Changed 2026-09-19 from QUIET
   * ("seen, not announced") to a single quiet beep + green LED: fully silent
   * made a temperature update indistinguishable from the device being off,
   * and green (rather than any of `notice`/`alert`/`celebrate`'s colors)
   * reads as "routine, nothing to act on" at a glance.
   */
  "sensor.reading": {
    expression: "neutral",
    led: "#00ff00",
    blink: false,
    jingle: "beep",
  },
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
 * Priority modifies the kind's presentation only by escalating - a kind's own
 * jingle/LED (or lack of one) already encodes how much attention it deserves,
 * so `normal`/`low` both leave it untouched:
 *   - `high` guarantees the message is noticeable even if its kind is quiet.
 *   - `low` used to unconditionally strip the jingle too ("so ambient chatter
 *     never makes noise"), which was fine while every low-priority kind was
 *     QUIET anyway - but `ha-temperature` always sends `sensor.reading` at
 *     `low` (see services/feeds/ha-temperature), so once that kind got its
 *     own beep (2026-09-19), the blanket strip would have silenced it right
 *     back. Removed rather than special-cased: nothing else relies on `low`
 *     muting a kind that wants to make noise.
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
  return base;
}
