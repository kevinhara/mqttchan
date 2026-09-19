/**
 * The MQTT payload contract, and the only module that knows it.
 *
 * Contract v3 (2026-09-19), matching firmware/include/mqtt_link.h:
 *   - `expression` is gone. Contract v2's six-word mood vocabulary
 *     (happy/angry/sad/doubt/sleepy/neutral) stopped driving anything the
 *     moment the firmware's portrait pack replaced m5avatar's procedural
 *     eyes/mouth - it had been a parse-and-warn-only no-op field since. See
 *     the firmware's mqtt_link.h constructor comment.
 *   - `led` accepts "#rrggbb" hex, "cycle", or the legacy "red"/"green"/"blue"
 *     aliases. We always emit hex or "cycle" - never the aliases.
 *   - `text` may be long: SpeechBubble scrolls rather than clipping. Length is
 *     a timing concern (45ms/char to reveal), not a truncation one. The only
 *     hard limit is the encoded payload size.
 */

export const JINGLES = [
  "chime",
  "alert",
  "fanfare",
  "gentle",
  "boarding",
  "beep",
  "coin",
  "oneup",
  "stageclear",
  "descend",
  "trill",
] as const;
export type Jingle = (typeof JINGLES)[number];

/** "" = no LED, "cycle" = HSV sweep, otherwise "#rrggbb". */
export type Led = "" | "cycle" | `#${string}`;

export interface DevicePayload {
  text: string;
  led: Led;
  blink: boolean;
  jingle: "" | Jingle;
}

const HEX_RE = /^#[0-9a-f]{6}$/i;

export function isValidLed(led: string): boolean {
  return led === "" || led === "cycle" || HEX_RE.test(led);
}

export function encodedSize(payload: DevicePayload): number {
  return Buffer.byteLength(JSON.stringify(payload), "utf8");
}

export class PayloadError extends Error {}

/**
 * Enforces the device contract on the way out and returns a payload safe to
 * publish.
 *
 * Empty text is refused rather than sent: the firmware gates the entire
 * message body on `text.length() > 0`, so an empty-text payload silently does
 * nothing at all - no LED, no jingle. Better to fail loudly here than to
 * publish a no-op.
 *
 * Over-budget payloads have their *text* trimmed at a word boundary until they
 * fit, rather than being rejected: the device drops oversized payloads with no
 * error of any kind, so anything we let through must already fit.
 */
export function finalisePayload(
  payload: DevicePayload,
  maxBytes: number,
): DevicePayload {
  const text = payload.text.trim();
  if (text.length === 0) {
    throw new PayloadError("text is empty; the device would ignore the message");
  }
  if (!isValidLed(payload.led)) {
    throw new PayloadError(`invalid led ${JSON.stringify(payload.led)}`);
  }
  if (payload.jingle !== "" && !JINGLES.includes(payload.jingle)) {
    throw new PayloadError(`invalid jingle ${JSON.stringify(payload.jingle)}`);
  }

  let candidate: DevicePayload = { ...payload, text };
  if (encodedSize(candidate) <= maxBytes) return candidate;

  // Trim whole words off the end until it fits, then fall back to a hard
  // character cut if a single word is somehow still too long.
  const words = text.split(/\s+/);
  while (words.length > 1) {
    words.pop();
    candidate = { ...payload, text: `${words.join(" ")}…` };
    if (encodedSize(candidate) <= maxBytes) return candidate;
  }

  let cut = candidate.text;
  while (cut.length > 1 && encodedSize({ ...payload, text: cut }) > maxBytes) {
    cut = cut.slice(0, -1);
  }
  candidate = { ...payload, text: cut };
  if (encodedSize(candidate) > maxBytes) {
    throw new PayloadError(
      `cannot fit payload into ${maxBytes} bytes even after trimming text`,
    );
  }
  return candidate;
}
