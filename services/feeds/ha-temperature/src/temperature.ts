/**
 * Turning HA's raw state list into the Zigbee room readings worth reporting.
 *
 * Pure and network-free on purpose, same reasoning as `flights/src/movements.ts`:
 * this is the part with the actual rules in it, so it is the part that gets
 * tested without a live Home Assistant to talk to.
 */

import type { HaState } from "./ha.js";

export interface TemperatureReading {
  entityId: string;
  name: string;
  celsius: number;
}

const NO_READING = new Set(["unavailable", "unknown"]);

/** HA's own classification, not a guess from the entity id - entity ids are
 *  free text and prove nothing about what a sensor measures. */
export function isTemperatureSensor(s: HaState): boolean {
  return s.attributes.device_class === "temperature";
}

/**
 * A short room label from HA's `friendly_name`, e.g. "Office Temperature" ->
 * "Office" - matching the "Office 20.1°C" shape the display expects. Falls
 * back to the entity id when there is no friendly name to trim.
 */
export function nameOf(s: HaState): string {
  const friendly = s.attributes.friendly_name?.trim();
  if (friendly === undefined || friendly === "") return s.entity_id;
  const trimmed = friendly.replace(/\s*temperature\s*$/i, "").trim();
  return trimmed !== "" ? trimmed : friendly;
}

/**
 * The Zigbee temperature readings worth reporting from one poll.
 *
 * `zigbeeEntityIds` is HA's own answer (via `integration_entities`) to "which
 * entities belong to the Zigbee integration" - not inferred from naming or
 * device class, both of which a Bluetooth or Wi-Fi sensor can share. A
 * `device_class: temperature` entity that Zigbee does not claim is skipped
 * even if it looks exactly like a room sensor.
 *
 * `unavailable`/`unknown` is skipped rather than reported as a reading: it is
 * the shape of an HA restart or a radio dropout, not a temperature, and both
 * BTHome and Zigbee end-devices sit there for a while after either one.
 */
export function readingsOf(
  states: HaState[],
  zigbeeEntityIds: ReadonlySet<string>,
): TemperatureReading[] {
  const readings: TemperatureReading[] = [];
  for (const s of states) {
    if (!isTemperatureSensor(s)) continue;
    if (!zigbeeEntityIds.has(s.entity_id)) continue;
    if (NO_READING.has(s.state)) continue;

    const celsius = Number(s.state);
    if (!Number.isFinite(celsius)) continue;

    readings.push({ entityId: s.entity_id, name: nameOf(s), celsius });
  }
  return readings;
}
