/**
 * Reports room temperatures from Home Assistant, Zigbee sensors only.
 *
 * Unlike `feeds/flights`, this is exactly the case `ChangeTracker` was built
 * for: report the current reading, then only again once it has moved enough
 * to matter - no edge detection needed, HA already did the sensing.
 *
 * "Zigbee only" is enforced by asking HA itself which entities belong to the
 * configured integration (`HA_ZIGBEE_INTEGRATION`, see `src/ha.ts`), not by
 * guessing from entity id or room name - this HA instance also has BTHome
 * (Bluetooth) temperature sensors reporting the exact same `device_class`,
 * and nothing in an entity's name or state distinguishes the two.
 *
 * See what it can see right now, without sending anything:
 *   node dist/index.js --once --dry-run
 */

import { ApiClient, ChangeTracker, startPolling, WarnOnce } from "@mqttchan/feed-kit";
import { fetchIntegrationEntityIds, fetchStates } from "./ha.js";
import { readingsOf, type TemperatureReading } from "./temperature.js";

const SOURCE = "ha-temperature";

const args = new Set(process.argv.slice(2));
const once = args.has("--once");
const dryRun = args.has("--dry-run");

function requireEnv(key: string): string {
  const value = process.env[key];
  if (value === undefined || value === "") {
    throw new Error(`${key} must be set`);
  }
  return value;
}

function num(key: string, fallback: number): number {
  const raw = process.env[key];
  if (raw === undefined || raw === "") return fallback;
  const parsed = Number(raw);
  if (!Number.isFinite(parsed)) {
    throw new Error(`${key} must be a number, got ${JSON.stringify(raw)}`);
  }
  return parsed;
}

const haUrl = requireEnv("HA_URL").replace(/\/+$/, "");
const haToken = requireEnv("HA_TOKEN");

/**
 * Which HA integration counts as "Zigbee". Defaults to `zha`, HA's own
 * built-in Zigbee integration, because that's the common case for a fresh
 * setup with no separate MQTT bridge - but this has not been confirmed
 * against the live instance, and Zigbee2MQTT or deCONZ would need a different
 * value here. Check with `--once --dry-run`: a `zha` guess against a
 * Zigbee2MQTT setup prints zero sensors rather than the wrong ones, so a
 * misconfiguration reads as silence, not bad data.
 */
const zigbeeIntegration = process.env["HA_ZIGBEE_INTEGRATION"] ?? "zha";

/** Matches the predecessor service's own research: cadence is bursty enough
 *  that anything smaller would be noisy, and a room does not need finer. */
const thresholdC = num("TEMPERATURE_CHANGE_THRESHOLD_C", 0.5);
const ttlSeconds = num("TEMPERATURE_TTL_SECONDS", 900);

const client = new ApiClient({
  baseUrl: process.env["API_URL"] ?? "http://api:8420",
  source: SOURCE,
  dryRun,
});

const changes = new ChangeTracker();
const warned = new WarnOnce();

/**
 * Plain ASCII, no degree sign. The message screen's typewriter reveal
 * (`firmware/include/speech_bubble.h`'s `show()`) prints one raw byte at a
 * time, which splits `°`'s 2-byte UTF-8 encoding apart before either byte
 * reaches the font - it renders as two unrecognised-glyph boxes, not a
 * degree symbol. Confirmed live 2026-09-18 against the deployed device.
 */
function formatReading(r: TemperatureReading): string {
  return `${r.name} ${r.celsius.toFixed(1)}C`;
}

async function tick(): Promise<void> {
  const [states, zigbeeEntityIds] = await Promise.all([
    fetchStates(haUrl, haToken),
    fetchIntegrationEntityIds(haUrl, haToken, zigbeeIntegration),
  ]);

  const readings = readingsOf(states, zigbeeEntityIds);

  if (readings.length === 0) {
    warned.warn(
      "empty",
      `${SOURCE}: no Zigbee temperature sensors found (integration=${zigbeeIntegration}) - wrong HA_ZIGBEE_INTEGRATION, or none paired yet`,
    );
  } else {
    warned.reset("empty");
  }

  if (dryRun) {
    // A single poll can only ever establish baselines against ChangeTracker,
    // so print the picture instead: the readings and the integration used to
    // find them are what you are checking.
    console.log(
      `${SOURCE}: ${readings.length} Zigbee temperature sensors (integration=${zigbeeIntegration})`,
    );
    for (const r of readings) {
      console.log(`  ${formatReading(r).padEnd(24)} (${r.entityId})`);
    }
  }

  for (const r of readings) {
    if (!changes.changedBy(r.entityId, r.celsius, thresholdC)) continue;

    const result = await client.send({
      kind: "sensor.reading",
      text: formatReading(r),
      priority: "low",
      // Per entity, so a newer reading replaces a queued older one instead of
      // both being shown - see the API's dedupe rule.
      dedupeKey: `${SOURCE}:${r.entityId}`,
      ttlSeconds,
    });

    if (result !== null) {
      console.log(
        `${SOURCE}: ${formatReading(r)} -> ${result.decision}${result.reason ? ` (${result.reason})` : ""}`,
      );
    }
  }
}

if (once) {
  tick().catch((err) => {
    console.log(`${SOURCE}: ${String(err)}`);
    process.exit(1);
  });
} else {
  const intervalMs = num("POLL_INTERVAL_MS", 60_000);
  const stop = startPolling({ name: SOURCE, intervalMs, immediate: true }, tick);
  process.on("SIGTERM", () => { stop(); process.exit(0); });
  process.on("SIGINT", () => { stop(); process.exit(0); });
}
