import { describe, expect, it } from "vitest";
import type { HaState } from "../src/ha.js";
import { isTemperatureSensor, nameOf, readingsOf } from "../src/temperature.js";

/** Illustrative fixtures shaped like HA's `/api/states` response - not
 *  captured from a live instance, unlike flights/test's ADS-B fixtures. */
const office: HaState = {
  entity_id: "sensor.office_temperature",
  state: "20.1",
  attributes: { device_class: "temperature", friendly_name: "Office Temperature" },
};

const bedroom: HaState = {
  entity_id: "sensor.bedroom_temperature",
  state: "19.4",
  attributes: { device_class: "temperature", friendly_name: "Bedroom Temperature" },
};

/** Same device_class as every Zigbee sensor here - BTHome/Bluetooth reports
 *  "temperature" too, which is exactly why entity discovery can't stop at
 *  device_class and has to check integration membership as well. */
const kitchenPantryBluetooth: HaState = {
  entity_id: "sensor.kitchen_pantry_temperature",
  state: "unavailable",
  attributes: { device_class: "temperature", friendly_name: "Kitchen Pantry Temperature" },
};

const cpu: HaState = {
  entity_id: "sensor.system_monitor_processor_temperature",
  state: "54.2",
  attributes: { device_class: "temperature", friendly_name: "System Monitor Processor Temperature" },
};

const humidity: HaState = {
  entity_id: "sensor.office_humidity",
  state: "45",
  attributes: { device_class: "humidity", friendly_name: "Office Humidity" },
};

const ZIGBEE_IDS = new Set([
  office.entity_id,
  bedroom.entity_id,
  cpu.entity_id, // a Zigbee-paired device could plausibly report CPU temp too
]);

describe("isTemperatureSensor", () => {
  it("is true only for device_class temperature", () => {
    expect(isTemperatureSensor(office)).toBe(true);
    expect(isTemperatureSensor(humidity)).toBe(false);
  });
});

describe("nameOf", () => {
  it("drops a trailing 'Temperature' from the friendly name", () => {
    expect(nameOf(office)).toBe("Office");
    expect(nameOf(bedroom)).toBe("Bedroom");
  });

  it("falls back to the entity id when there is no friendly name", () => {
    const noName: HaState = { entity_id: "sensor.c829ac", state: "20", attributes: {} };
    expect(nameOf(noName)).toBe("sensor.c829ac");
  });

  it("falls back to the untrimmed name if trimming would leave nothing", () => {
    const bare: HaState = {
      entity_id: "sensor.x",
      state: "20",
      attributes: { friendly_name: "Temperature" },
    };
    expect(nameOf(bare)).toBe("Temperature");
  });
});

describe("readingsOf", () => {
  it("keeps only device_class temperature entities the Zigbee integration claims", () => {
    const readings = readingsOf(
      [office, bedroom, kitchenPantryBluetooth, humidity],
      ZIGBEE_IDS,
    );
    expect(readings).toEqual([
      { entityId: "sensor.office_temperature", name: "Office", celsius: 20.1 },
      { entityId: "sensor.bedroom_temperature", name: "Bedroom", celsius: 19.4 },
    ]);
  });

  it("excludes a temperature entity Zigbee does not claim, even on the ground truth device_class", () => {
    // kitchenPantryBluetooth is device_class temperature but not in
    // ZIGBEE_IDS - it must not appear even though its state is also
    // "unavailable", which alone would already exclude it.
    const readings = readingsOf([kitchenPantryBluetooth], ZIGBEE_IDS);
    expect(readings).toEqual([]);
  });

  it("skips unavailable and unknown states rather than reporting them", () => {
    const unknown: HaState = { ...bedroom, state: "unknown" };
    expect(readingsOf([unknown], ZIGBEE_IDS)).toEqual([]);
  });

  it("skips a non-numeric state defensively", () => {
    const weird: HaState = { ...office, state: "on" };
    expect(readingsOf([weird], ZIGBEE_IDS)).toEqual([]);
  });

  it("reports a Zigbee sensor even when the entity id suggests something else", () => {
    // isTemperatureSensor + zigbee membership are what decide this, not the
    // "system_monitor" name a CPU sensor usually carries.
    expect(readingsOf([cpu], ZIGBEE_IDS)).toEqual([
      { entityId: cpu.entity_id, name: "System Monitor Processor", celsius: 54.2 },
    ]);
  });
});
