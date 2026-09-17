/**
 * Everything a feed would otherwise reinvent. A feed should be its polling
 * logic and nothing else - no MQTT, no retry loops, no change tracking.
 */

export * from "./client.js";
export * from "./poll.js";
export * from "./change.js";
export * from "./warn.js";
