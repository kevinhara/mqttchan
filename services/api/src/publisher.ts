/**
 * The only thing in this system that speaks MQTT.
 *
 * Feeds POST to the API instead of publishing themselves, so this is the one
 * place the broker connection, the topic and the retain flag are decided.
 */

import mqtt, { type MqttClient } from "mqtt";
import { config } from "./config.js";
import type { DevicePayload } from "./device.js";

export class Publisher {
  private client: MqttClient | null = null;

  connect(): void {
    const url = `mqtt://${config.mqtt.host}:${config.mqtt.port}`;
    // A duplicate client id would kick the *device* off the broker - the
    // firmware connects as "avatar-demo", so this must stay distinct.
    this.client = mqtt.connect(url, {
      clientId: config.mqtt.clientId,
      reconnectPeriod: 2000,
    });
    this.client.on("connect", () => console.log(`mqtt: connected to ${url}`));
    this.client.on("reconnect", () => console.log("mqtt: reconnecting"));
    this.client.on("error", (err) => console.log(`mqtt: ${err.message}`));
  }

  get connected(): boolean {
    return this.client?.connected ?? false;
  }

  async publish(payload: DevicePayload): Promise<void> {
    const client = this.client;
    if (client === null) throw new Error("publisher not connected");
    const body = JSON.stringify(payload);
    await client.publishAsync(config.mqtt.topic, body, {
      qos: 0,
      // Never retained. The old Python publisher retained these, which made the
      // device replay a stale message on every boot and reconnect - a
      // notification is a moment, not a state.
      retain: false,
    });
    console.log(`published: ${body}`);
  }

  async end(): Promise<void> {
    await this.client?.endAsync();
    this.client = null;
  }
}
