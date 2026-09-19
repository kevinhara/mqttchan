/**
 * Today's weather outlook from MetService's Point Forecast API
 * (`forecast-v2.metoceanapi.com`), the one part of this feed that needs a
 * registered `METSERVICE_API_KEY`.
 *
 * Verified live 2026-09-19 against Queenstown's coordinates with a real key:
 * `POST /point/time` with `{points, variables, time}` and an `x-api-key`
 * header returns `{dimensions: {time, point}, noDataReasons, variables}`,
 * where each requested variable is `{standardName, units, data: number[],
 * noData: number[]}` - one entry per requested time step, in order. `noData`
 * is 0 for a good sample and a `noDataReasons` code otherwise (e.g. a wave
 * variable requested over land comes back `MASK_LAND`); this module treats
 * any non-zero `noData` as "drop this sample" rather than trusting a bogus
 * number.
 *
 * The variable catalogue was read from the live `/variables/` endpoint, not
 * guessed - `air.temperature.at-2m` comes back in Kelvin, wind in
 * meters/second, so both get converted before anything is displayed.
 */

const BASE_URL = "https://forecast-v2.metoceanapi.com";

const VARIABLES = [
  "air.temperature.at-2m",
  "precipitation.rate",
  "wind.speed.at-10m",
  "wind.direction.at-10m",
  "wind.speed.gust.at-10m",
  "cloud.cover",
] as const;

interface VariableSeries {
  data: number[];
  noData: number[];
}

interface PointTimeResponse {
  variables: Record<string, VariableSeries>;
}

export interface DailyOutlook {
  minC: number;
  maxC: number;
  /** Rough accumulated rainfall over the outlook window, in mm - see
   *  `totalPrecipMm`'s own note on why this is an approximation. */
  precipMm: number;
  windKmh: number;
  gustKmh: number;
  windDirection: string;
  cloud: "clear" | "partly cloudy" | "overcast";
}

export function kelvinToC(kelvin: number): number {
  return kelvin - 273.15;
}

export function msToKmh(metersPerSecond: number): number {
  return metersPerSecond * 3.6;
}

const COMPASS_POINTS = [
  "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
] as const;

/** `wind.direction.at-10m` is "wind FROM direction", meteorological
 *  convention - the same sense as "a northwesterly", which is what this
 *  produces. */
export function compassDirection(degrees: number): string {
  const normalized = ((degrees % 360) + 360) % 360;
  const index = Math.round(normalized / 22.5) % 16;
  return COMPASS_POINTS[index] as string;
}

export function cloudDescription(coverPercent: number): DailyOutlook["cloud"] {
  if (coverPercent < 20) return "clear";
  if (coverPercent < 70) return "partly cloudy";
  return "overcast";
}

/** Valid (`noData === 0`) values from a variable series, in original order. */
function validValues(series: VariableSeries | undefined): number[] {
  if (series === undefined) return [];
  return series.data.filter((_, i) => series.noData[i] === 0);
}

/**
 * `precipitation.rate` is millimeters *per hour*, sampled every `interval`
 * hours rather than integrated continuously - multiplying each sample by
 * the interval and summing is a rough trapezoid-free estimate of the day's
 * total rainfall, not a real integral. Good enough for "rain expected today:
 * roughly Xmm", not a number to defend to a meteorologist.
 */
function totalPrecipMm(series: VariableSeries | undefined, intervalHours: number): number {
  return validValues(series).reduce((sum, ratePerHour) => sum + ratePerHour * intervalHours, 0);
}

/**
 * Today's outlook for one point, or `null` if the key is missing, the
 * request fails, or every sample came back `noData` - weather is one input
 * among several in the daily digest, never a reason to fail the whole tick.
 */
export async function fetchDailyOutlook(
  lat: number,
  lon: number,
  apiKey: string,
  now: Date,
  timeoutMs = 10_000,
): Promise<DailyOutlook | null> {
  const intervalHours = 3;
  const steps = 8; // roughly the next 24h at a 3h cadence

  try {
    const res = await fetch(`${BASE_URL}/point/time`, {
      method: "POST",
      headers: { "content-type": "application/json", "x-api-key": apiKey },
      body: JSON.stringify({
        points: [{ lon, lat }],
        variables: VARIABLES,
        time: { from: now.toISOString(), interval: `${intervalHours}h`, repeat: steps - 1 },
      }),
      signal: AbortSignal.timeout(timeoutMs),
    });
    if (!res.ok) return null;

    const body = (await res.json()) as PointTimeResponse;
    const temps = validValues(body.variables["air.temperature.at-2m"]).map(kelvinToC);
    const gusts = validValues(body.variables["wind.speed.gust.at-10m"]).map(msToKmh);
    const winds = validValues(body.variables["wind.speed.at-10m"]).map(msToKmh);
    const directions = validValues(body.variables["wind.direction.at-10m"]);
    const clouds = validValues(body.variables["cloud.cover"]);
    if (temps.length === 0 || winds.length === 0 || directions.length === 0 || clouds.length === 0) {
      return null;
    }

    // The direction at the windiest step reads more naturally than an
    // average of compass bearings, which is meaningless near the N/0 wrap.
    const windiestIndex = winds.indexOf(Math.max(...winds));
    const meanCloud = clouds.reduce((a, b) => a + b, 0) / clouds.length;

    return {
      minC: Math.min(...temps),
      maxC: Math.max(...temps),
      precipMm: totalPrecipMm(body.variables["precipitation.rate"], intervalHours),
      windKmh: winds[windiestIndex] as number,
      gustKmh: gusts.length > 0 ? Math.max(...gusts) : winds[windiestIndex] as number,
      windDirection: compassDirection(directions[windiestIndex] as number),
      cloud: cloudDescription(meanCloud),
    };
  } catch {
    return null;
  }
}
