/** All tunables in one place, read once at startup. Env-driven, same as the
 *  rest of the homelab's services - no config schema library. */

function num(key: string, fallback: number): number {
  const raw = process.env[key];
  if (raw === undefined || raw === "") return fallback;
  const parsed = Number(raw);
  if (!Number.isFinite(parsed)) {
    throw new Error(`${key} must be a number, got ${JSON.stringify(raw)}`);
  }
  return parsed;
}

function str(key: string, fallback: string): string {
  const raw = process.env[key];
  return raw === undefined || raw === "" ? fallback : raw;
}

export const config = {
  port: num("PORT", 8420),
  host: str("HOST", "0.0.0.0"),

  mqtt: {
    host: str("MQTT_HOST", "localhost"),
    port: num("MQTT_PORT", 1883),
    topic: str("MQTT_TOPIC", "avatar/say"),
    /** The firmware connects as "avatar-demo". A second client using the same
     *  id would kick the device off the broker, so this must stay distinct. */
    clientId: str("MQTT_CLIENT_ID", "mqttchan-api"),
    /** PubSubClient's setBufferSize() in firmware/include/mqtt_link.h. Anything
     *  larger is dropped by the device silently - no error, no callback, the
     *  message simply never arrives. Raise here only after raising it there. */
    maxPayloadBytes: num("MQTT_MAX_PAYLOAD_BYTES", 512),
  },

  /** Used to estimate how long the device is busy with a message, which sets
   *  the gap before the next publish. See queue.ts estimateDisplayMs(). */
  device: {
    holdSeconds: num("DEVICE_HOLD_SECONDS", 30),
    charMs: num("DEVICE_CHAR_MS", 45),
    jingleMs: num("DEVICE_JINGLE_MS", 1300),
    safetyMs: num("DEVICE_SAFETY_MS", 1500),
  },

  rateLimit: {
    /** Per-source token bucket. The global gap is the real limiter; this only
     *  stops one noisy feed monopolising the device. */
    capacity: num("RATE_LIMIT_CAPACITY", 10),
    refillPerMinute: num("RATE_LIMIT_REFILL_PER_MINUTE", 10),
  },

  defaultTtlSeconds: num("DEFAULT_TTL_SECONDS", 900),
  drainIntervalMs: num("DRAIN_INTERVAL_MS", 1000),
} as const;

export type Config = typeof config;
