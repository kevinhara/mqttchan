/**
 * Where a callsign is headed, from adsbdb.com's public route lookup.
 *
 * adsb.lol's own feed carries only position, never a filed route, so "from"
 * and "for" come from a second, unrelated API keyed on the callsign alone.
 * This is decoration on top of a movement that has already been detected: a
 * failed or missing lookup must never stop the movement being announced, it
 * just announces without a place name.
 *
 * Verified live 2026-09-18: `GET /v0/callsign/ANZ611` returns Auckland ->
 * Queenstown. An unfiled or malformed callsign (`ZK-OYB`, `NOTREAL1`) comes
 * back `400`/`404` with `{"response": "<message>"}` rather than throwing, so
 * that shape is read as "no route" instead of an error.
 */

import https from "node:https";

export interface Route {
  origin: string;
  destination: string;
}

interface AirportInfo {
  municipality?: string;
}

interface FlightRoute {
  origin?: AirportInfo;
  destination?: AirportInfo;
}

interface RouteResponse {
  response?: { flightroute?: FlightRoute } | string;
}

const BASE_URL = "https://api.adsbdb.com/v0";

/**
 * The same IPv6 blackhole documented in `adsb.ts` against api.adsb.lol also
 * hits this host (verified 2026-09-18: node's `fetch()` hangs to
 * `ETIMEDOUT`, `https.get` with `family: 4` returns 200 immediately) - force
 * IPv4 for the same reason.
 */
const IP_FAMILY = Number(process.env["ADSB_IP_FAMILY"] ?? 4);

function getJson(url: string, timeoutMs: number): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const req = https.get(
      url,
      {
        ...(IP_FAMILY === 0 ? {} : { family: IP_FAMILY }),
        headers: { accept: "application/json" },
      },
      (res) => {
        let body = "";
        res.setEncoding("utf8");
        res.on("data", (chunk: string) => {
          body += chunk;
        });
        res.on("end", () => {
          try {
            resolve(JSON.parse(body));
          } catch {
            reject(new Error(`adsbdb returned unparseable JSON for ${url}`));
          }
        });
      },
    );

    req.on("error", reject);
    req.setTimeout(timeoutMs, () => {
      req.destroy(new Error(`adsbdb timed out after ${timeoutMs}ms`));
    });
  });
}

/**
 * A callsign shaped like one with a filed route - letters and digits, airline
 * flight-number length. Filters out the hex and registration (`ZK-OYB`)
 * fallbacks `identify()` produces when no callsign was transmitted, which
 * adsbdb has no route for anyway; skipping them saves the request.
 */
function looksLikeCallsign(ident: string): boolean {
  return (
    /^[A-Z0-9]{3,8}$/.test(ident) &&
    /[A-Z]/.test(ident) &&
    /[0-9]/.test(ident)
  );
}

/**
 * The origin/destination city for a callsign, or `null` if adsbdb has no
 * route on file, `ident` isn't shaped like a callsign, or the lookup fails
 * for any reason. Never throws.
 */
export async function fetchRoute(
  ident: string,
  timeoutMs = 5_000,
): Promise<Route | null> {
  if (!looksLikeCallsign(ident)) return null;

  try {
    const body = (await getJson(
      `${BASE_URL}/callsign/${encodeURIComponent(ident)}`,
      timeoutMs,
    )) as RouteResponse;
    const response = body.response;
    if (typeof response === "string" || response === undefined) return null;

    const route = response.flightroute;
    const origin = route?.origin?.municipality;
    const destination = route?.destination?.municipality;
    if (origin === undefined || destination === undefined) return null;

    return {
      origin: origin.toUpperCase(),
      destination: destination.toUpperCase(),
    };
  } catch {
    return null;
  }
}
