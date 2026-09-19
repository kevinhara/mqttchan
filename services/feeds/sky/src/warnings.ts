/**
 * MetService's public Common Alerting Protocol feed - severe weather watches
 * and warnings for the whole of New Zealand, no API key or registration
 * required. This is the one weather integration this feed can do without
 * `METSERVICE_API_KEY`; see the README for what still needs it.
 *
 * The Atom feed lists every current alert nationwide but says nothing about
 * *where* - the affected area only exists in each alert's own CAP XML
 * document, one HTTP request per entry away. Verified live 2026-09-19: a
 * "Strong Wind Watch" entry's `areaDesc` read "Hawke's Bay south of Waipawa
 * and the Tararua District", nowhere near Otago, while a same-day
 * "Road Snowfall Warning" entry's id already named "crownrangeroad" - so the
 * area check has to open the CAP document, not guess from the title.
 */

import { XMLParser } from "fast-xml-parser";

const ATOM_URL = "https://alerts.metservice.com/cap/atom";

/**
 * No key required for this feed, but a real UA is good manners to a public
 * service - `flights/src/adsb.ts` learned adsb.lol enforces this; nothing
 * says MetService does, but there is no reason to find out the hard way.
 */
const USER_AGENT = "mqttchan-sky-feed/1.0";

export interface Warning {
  /** The CAP identifier. Stable per issuance - an update to an existing
   *  alert (new `msgType: Update`) gets a new id, so this is safe to use as
   *  a dedupe key without separately tracking "have I seen this version". */
  id: string;
  headline: string;
  description: string;
  areaDesc: string;
  severity: string;
  /** True for "Warning", false for "Watch"/"Advisory" - MetService's own
   *  escalation ladder, read off the headline text. */
  isWarning: boolean;
}

const parser = new XMLParser({ ignoreAttributes: false, attributeNamePrefix: "@_" });

function asArray<T>(value: T | T[] | undefined): T[] {
  if (value === undefined) return [];
  return Array.isArray(value) ? value : [value];
}

async function getText(url: string, timeoutMs: number): Promise<string | null> {
  try {
    const res = await fetch(url, {
      headers: { accept: "application/xml", "user-agent": USER_AGENT },
      signal: AbortSignal.timeout(timeoutMs),
    });
    if (!res.ok) return null;
    return await res.text();
  } catch {
    return null;
  }
}

/** Case-insensitive substring match against any configured region keyword. */
export function matchesRegion(areaDesc: string, keywords: readonly string[]): boolean {
  const lower = areaDesc.toLowerCase();
  return keywords.some((keyword) => lower.includes(keyword.toLowerCase()));
}

export function isWarningLevel(headline: string): boolean {
  return /\bwarning\b/i.test(headline);
}

interface AtomEntry {
  id: unknown;
  title: unknown;
  summary: unknown;
  link?: { "@_rel"?: string; "@_href"?: string } | { "@_rel"?: string; "@_href"?: string }[];
}

interface CapInfo {
  headline?: unknown;
  description?: unknown;
  severity?: unknown;
  area?: { areaDesc?: unknown } | { areaDesc?: unknown }[];
}

/**
 * Active watches/warnings whose CAP `areaDesc` mentions one of
 * `regionKeywords`. Never throws.
 *
 * Returns `null` only when the feed itself could not be fetched or parsed at
 * all - worth logging once. A per-entry failure (its detail document 404s,
 * times out, or fails to parse) instead just drops that one entry, since one
 * bad id says nothing about the rest of the feed; the caller cannot tell
 * that case apart from "fetched fine, nothing matched", which is by design -
 * neither is worth logging on every poll.
 */
export async function fetchActiveWarnings(
  regionKeywords: readonly string[],
  timeoutMs = 8000,
): Promise<Warning[] | null> {
  const feedXml = await getText(ATOM_URL, timeoutMs);
  if (feedXml === null) return null;

  let entries: AtomEntry[];
  try {
    const feed = parser.parse(feedXml) as { feed?: { entry?: AtomEntry | AtomEntry[] } };
    entries = asArray(feed.feed?.entry);
  } catch {
    return null;
  }

  const warnings: Warning[] = [];
  for (const entry of entries) {
    const href = asArray(entry.link).find((l) => l["@_rel"] === "related")?.["@_href"];
    if (typeof href !== "string") continue;

    const capXml = await getText(href, timeoutMs);
    if (capXml === null) continue;

    try {
      const cap = parser.parse(capXml) as { alert?: { info?: CapInfo } };
      const info = cap.alert?.info;
      const areaDesc = asArray(info?.area)[0]?.areaDesc;
      if (typeof areaDesc !== "string" || !matchesRegion(areaDesc, regionKeywords)) continue;

      const headline = String(info?.headline ?? entry.title ?? "Weather warning");
      warnings.push({
        id: String(entry.id),
        headline,
        description: String(info?.description ?? entry.summary ?? ""),
        areaDesc,
        severity: String(info?.severity ?? "Unknown"),
        isWarning: isWarningLevel(headline),
      });
    } catch {
      continue;
    }
  }
  return warnings;
}
