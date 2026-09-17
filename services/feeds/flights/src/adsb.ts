/**
 * The live aircraft source: adsb.lol's point query.
 *
 * Chosen over a local SDR because this was already the documented decision -
 * the predecessor service's README named "airplanes.live or adsb.lol keyed to
 * lat/lon + radius, not local SDR hardware" as the way to do aircraft, and
 * nothing has changed that. No key, no account, no hardware in the loop.
 *
 * Verified live against NZQN on 2026-09-18: a 30nm point query returned 8
 * contacts, including ANZ611 (ZK-OYB, A21N) descending through 6825ft 14nm
 * out, and a helicopter over the field at 1750ft. Response carries no
 * rate-limit headers and `cache-control: no-store`.
 *
 * airplanes.live serves the same /v2/point shape and is the fallback if this
 * goes away; it was not needed, so it is not wired in.
 */

import https from "node:https";

/** One contact. Every field but `hex` is optional - the API omits what the
 *  aircraft did not transmit, and this feed has to cope with all of it. */
export interface Aircraft {
  hex: string;
  /**
   * Emitter category. Anything ending in `_nt` is a non-transponder address:
   * ADS-B ground infrastructure and vehicles, NOT aircraft. NZQN's 30nm circle
   * contains six of them - two self-identify as `TWR` - and they sit
   * permanently at `alt_baro: "ground"`, so they must be dropped before
   * anything reasons about who is on the ground here.
   */
  type?: string;
  /** Callsign, space-padded by the API: `"ANZ611   "`. */
  flight?: string;
  /** Registration, e.g. `ZK-OYB`. Often absent. */
  r?: string;
  /** ICAO type code, e.g. `A21N`. Often absent. */
  t?: string;
  /**
   * Barometric altitude in feet, or the literal string `"ground"`. That string
   * is the aircraft's own air/ground status bit (weight-on-wheels), not a
   * guess derived from altitude, which is why this feed trusts it directly
   * instead of comparing altitudes against field elevation.
   */
  alt_baro?: number | "ground";
  gs?: number;
  lat?: number;
  lon?: number;
}

interface PointResponse {
  ac?: Aircraft[];
  msg?: string;
}

const BASE_URL = "https://api.adsb.lol/v2";

/**
 * adsb.lol rejects a generic User-Agent outright - `403 "User-Agent too
 * generic; include valid contact info."`, verified 2026-09-18 - and node's
 * default UA is generic enough to trigger it. curl's is not, which is why a
 * hand-rolled curl against this API succeeds where the obvious `fetch()` does
 * not. Identify the service and give them somewhere to complain.
 */
const USER_AGENT =
  process.env["ADSB_USER_AGENT"] ??
  "mqttchan-flights/1.0 (+https://mqttchan.kev.nz)";

/**
 * Why `node:https` and an explicit address family rather than `fetch()`.
 *
 * api.adsb.lol publishes both A and AAAA records, and its IPv6 addresses
 * blackhole from this network - `curl -6` never connects while `curl -4`
 * returns 200 (verified 2026-09-18). `fetch()` hangs until it times out and
 * offers no way to say "IPv4 only": `dns.setDefaultResultOrder("ipv4first")`
 * does not help, undici's happy-eyeballs fallback does not rescue it, and
 * setting a dispatcher would mean taking on undici as a dependency this
 * workspace does not otherwise have. `https.request` takes `family` natively.
 *
 * Set ADSB_IP_FAMILY=0 to hand the choice back to the OS if this is ever
 * running somewhere with working IPv6 to them.
 */
const IP_FAMILY = Number(process.env["ADSB_IP_FAMILY"] ?? 4);

function getJson(url: string, timeoutMs: number): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const req = https.get(
      url,
      {
        ...(IP_FAMILY === 0 ? {} : { family: IP_FAMILY }),
        headers: { accept: "application/json", "user-agent": USER_AGENT },
      },
      (res) => {
        const status = res.statusCode ?? 0;
        let body = "";
        res.setEncoding("utf8");
        res.on("data", (chunk: string) => {
          body += chunk;
        });
        res.on("end", () => {
          if (status < 200 || status >= 300) {
            reject(
              new Error(
                `adsb.lol returned ${status} for ${url}: ${body.slice(0, 200)}`,
              ),
            );
            return;
          }
          try {
            resolve(JSON.parse(body));
          } catch {
            reject(new Error(`adsb.lol returned unparseable JSON for ${url}`));
          }
        });
      },
    );

    req.on("error", reject);
    req.setTimeout(timeoutMs, () => {
      req.destroy(new Error(`adsb.lol timed out after ${timeoutMs}ms`));
    });
  });
}

/**
 * Aircraft within `radiusNm` of a point. Throws on any transport or HTTP
 * error; the poll loop isolates the tick, so a failed fetch costs one poll
 * rather than the process.
 */
export async function fetchNearby(
  lat: number,
  lon: number,
  radiusNm: number,
  timeoutMs = 15_000,
): Promise<Aircraft[]> {
  const body = (await getJson(
    `${BASE_URL}/point/${lat}/${lon}/${radiusNm}`,
    timeoutMs,
  )) as PointResponse;
  return body.ac ?? [];
}
