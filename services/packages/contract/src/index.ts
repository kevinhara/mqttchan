/**
 * The feed <-> API interface, and the only thing both sides share.
 *
 * Deliberately *semantic*, not presentational: there is no `expression`, `led`
 * or `jingle` here. Feeds describe what happened; the API decides how the
 * device should render it. That separation is the whole point of the API layer
 * - the MQTT payload contract changed underneath these types once already
 * (lowercase expressions, hex LED colors) without any feed needing to know.
 */

export const PRIORITIES = ["low", "normal", "high"] as const;
export type Priority = (typeof PRIORITIES)[number];

export interface MessageRequest {
  /** Which feed sent this, e.g. "ha-temperature". Used for rate limiting. */
  source: string;
  /** What kind of thing happened, e.g. "sensor.reading". Drives presentation. */
  kind: string;
  /** Human-readable message text. */
  text: string;
  /** Default "normal". `high` overtakes queued lower-priority messages. */
  priority?: Priority;
  /**
   * Messages queued under the same key collapse: a newer one replaces the
   * older instead of both being shown. For sensor feeds this is the rule that
   * does the real work - you see the current value, not a backlog of history.
   */
  dedupeKey?: string;
  /** Dropped rather than shown if it has not reached the device in time. */
  ttlSeconds?: number;
}

export type Decision = "queued" | "deduped" | "dropped";

export interface MessageResponse {
  decision: Decision;
  /** Present when the decision needs explaining, e.g. "rate limited". */
  reason?: string;
}

/**
 * Fastify validates POST bodies against this. It lives beside the types rather
 * than in the API so the two cannot drift apart.
 */
export const messageRequestSchema = {
  type: "object",
  required: ["source", "kind", "text"],
  additionalProperties: false,
  properties: {
    source: { type: "string", minLength: 1, maxLength: 64 },
    kind: { type: "string", minLength: 1, maxLength: 64 },
    text: { type: "string", minLength: 1, maxLength: 2000 },
    priority: { type: "string", enum: [...PRIORITIES] },
    dedupeKey: { type: "string", minLength: 1, maxLength: 128 },
    ttlSeconds: { type: "integer", minimum: 1, maximum: 86400 },
  },
} as const;
