/**
 * A template feed, not a real integration.
 *
 * It exists to prove the whole path - feed -> API -> broker -> device - and to
 * be the thing you copy when writing a real one. Note what it does *not* do:
 * no MQTT client, no payload shape, no color or jingle. It reports what
 * happened and lets the API decide how that looks.
 *
 * Run one tick and print without sending:
 *   node dist/index.js --once --dry-run
 */

import { ApiClient, ChangeTracker, startPolling } from "@mqttchan/feed-kit";

const SOURCE = "example";

const args = new Set(process.argv.slice(2));
const once = args.has("--once");
const dryRun = args.has("--dry-run");

const client = new ApiClient({
  baseUrl: process.env["API_URL"] ?? "http://api:8420",
  source: SOURCE,
  dryRun,
});

const changes = new ChangeTracker();

async function tick(): Promise<void> {
  // A real feed would fetch something here. This stands in for that: a value
  // that moves every tick, reported only when it has moved enough to matter.
  const uptimeMinutes = Math.round(process.uptime() / 60);

  if (!changes.changedBy("uptime", uptimeMinutes, 5)) return;

  const result = await client.send({
    kind: "ambient",
    text: `Example feed up ${uptimeMinutes}m`,
    priority: "low",
    // Keyed so a newer reading replaces a queued older one instead of both
    // being shown - see the API's dedupe rule.
    dedupeKey: "example:uptime",
    ttlSeconds: 600,
  });

  if (result !== null) {
    console.log(`${SOURCE}: ${result.decision}${result.reason ? ` (${result.reason})` : ""}`);
  }
}

if (once) {
  tick().catch((err) => {
    console.log(`${SOURCE}: ${String(err)}`);
    process.exit(1);
  });
} else {
  const intervalMs = Number(process.env["POLL_INTERVAL_MS"] ?? 60_000);
  const stop = startPolling({ name: SOURCE, intervalMs, immediate: true }, tick);
  process.on("SIGTERM", () => { stop(); process.exit(0); });
  process.on("SIGINT", () => { stop(); process.exit(0); });
}
