/**
 * Geomagnetic activity, as a proxy for "is the aurora worth looking for
 * tonight" - the practical form "sun activity" takes for a Queenstown
 * latitude. NOAA SWPC's planetary Kp index needs no API key and no
 * registration.
 *
 * A real solar flare (X-ray flux/class) is a plausible later addition; Kp is
 * the one that actually gates whether stepping outside is worth it down
 * here, so it's the one built first.
 */

const KP_URL = "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json";

interface KpRow {
  time_tag: string;
  Kp: number;
}

/**
 * The most recent estimated planetary Kp index (0-9), or null on any fetch
 * or parse failure - space weather is decoration, never a reason to take the
 * rest of the feed down with it.
 *
 * Verified live 2026-09-19: the endpoint returns a JSON array of
 * `{time_tag, Kp, a_running, station_count}` objects, one per 3-hour window,
 * oldest first - no header row, unlike some of NOAA's other `/products/*.json`
 * endpoints.
 */
export async function fetchKp(timeoutMs = 5000): Promise<number | null> {
  try {
    const res = await fetch(KP_URL, { signal: AbortSignal.timeout(timeoutMs) });
    if (!res.ok) return null;
    const rows = (await res.json()) as KpRow[];
    const last = rows.at(-1);
    return typeof last?.Kp === "number" ? last.Kp : null;
  } catch {
    return null;
  }
}

export interface AuroraState {
  shouldAlert: boolean;
  alerted: boolean;
}

/**
 * Edge-triggers an aurora alert as Kp *crosses up* through `onThreshold`,
 * rather than firing on every poll while it stays elevated. `offThreshold`
 * (below `onThreshold`) is hysteresis, so a Kp bouncing between 4.7 and 5.0
 * across two polls doesn't alert twice for the same event.
 *
 * Kp 5 (G1 minor storm) is the default: Queenstown's magnetic latitude is
 * high enough that the aurora is at least worth a look from G1 up, well
 * before it would be visible further north in NZ.
 */
export function auroraEdge(
  kp: number,
  wasAlerted: boolean,
  onThreshold: number,
  offThreshold: number,
): AuroraState {
  if (!wasAlerted && kp >= onThreshold) return { shouldAlert: true, alerted: true };
  if (wasAlerted && kp < offThreshold) return { shouldAlert: false, alerted: false };
  return { shouldAlert: false, alerted: wasAlerted };
}
