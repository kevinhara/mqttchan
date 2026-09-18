/**
 * The live source: Home Assistant's own REST API.
 *
 * Base URL and token follow the predecessor Python service's convention
 * (`avatar-brain/producers/ha_sensors.py`, `HA_URL` + `HA_TOKEN`) - a
 * long-lived access token from HA's own profile page, read-only, no MQTT
 * integration needed on the HA side.
 *
 * That predecessor named an explicit, comma-separated list of entity ids
 * (`HA_ENTITIES`) and left the rest of the discovery to a human. This feed
 * instead has to answer "which of these is Zigbee?" itself, and the plain
 * `/api/states` response has no integration field to check - `device_class`
 * says a sensor reads temperature, not what radio it arrived over. The only
 * way to get that from HA's REST API is `/api/template`, which can evaluate
 * `integration_entities(domain)` and hand back exactly the entity ids that
 * belong to one integration's config entries.
 */

export interface HaState {
  entity_id: string;
  /** Always a string over this API, even for numeric sensors: "20.1", or
   *  "unavailable" / "unknown" when there is nothing to report. */
  state: string;
  attributes: {
    device_class?: string;
    friendly_name?: string;
    [key: string]: unknown;
  };
}

async function haRequest(
  baseUrl: string,
  token: string,
  path: string,
  timeoutMs: number,
  init?: { method?: string; body?: string },
): Promise<Response> {
  const res = await fetch(`${baseUrl}${path}`, {
    method: init?.method ?? "GET",
    body: init?.body,
    headers: {
      authorization: `Bearer ${token}`,
      "content-type": "application/json",
    },
    signal: AbortSignal.timeout(timeoutMs),
  });
  if (!res.ok) {
    const text = await res.text();
    throw new Error(
      `home assistant returned ${res.status} for ${path}: ${text.slice(0, 200)}`,
    );
  }
  return res;
}

/** Every entity's current state. Filtering to temperature sensors happens
 *  after this - HA has no query parameter for it. */
export async function fetchStates(
  baseUrl: string,
  token: string,
  timeoutMs = 15_000,
): Promise<HaState[]> {
  const res = await haRequest(baseUrl, token, "/api/states", timeoutMs);
  return (await res.json()) as HaState[];
}

/**
 * The entity ids belonging to one HA integration, via a template render.
 *
 * `/api/template` returns the rendered text directly, not JSON - `| tojson`
 * inside the template is what makes the body parseable, so this is the one
 * request in this feed that reads `res.text()` and parses it itself instead
 * of trusting `res.json()`.
 */
export async function fetchIntegrationEntityIds(
  baseUrl: string,
  token: string,
  integration: string,
  timeoutMs = 15_000,
): Promise<Set<string>> {
  const template = `{{ integration_entities(${JSON.stringify(integration)}) | tojson }}`;
  const res = await haRequest(baseUrl, token, "/api/template", timeoutMs, {
    method: "POST",
    body: JSON.stringify({ template }),
  });
  const text = await res.text();
  let ids: unknown;
  try {
    ids = JSON.parse(text);
  } catch {
    throw new Error(
      `home assistant returned unparseable template output for integration ${JSON.stringify(integration)}: ${text.slice(0, 200)}`,
    );
  }
  if (!Array.isArray(ids)) {
    throw new Error(
      `home assistant template for integration ${JSON.stringify(integration)} did not return a list: ${text.slice(0, 200)}`,
    );
  }
  return new Set(ids as string[]);
}
