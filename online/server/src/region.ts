// Where a room runs and where its players are (finding 135). Pure helpers: the Durable Object
// location hint for a new room from its creator's Cloudflare data (request.cf), the short region a
// player sees ("EU", "NA", "ASIA"...), and the order the public list is served in.
//
// A room is a Durable Object. Without a hint it is placed near the Cloudflare location the first
// request went through, which for a player whose ISP routes far away (or who reaches another
// continent's edge) is far from them and from everyone else in their region. The hint comes from
// the creator's continent and country as Cloudflare's geolocation sees them, not from the edge.
//
// Since finding 181 the creator can pick the region (CHOICES), and Auto goes by the Cloudflare
// location the creator reached (src/colo-hints.ts, made from Cloudflare's published Durable Object
// locations and measured round trips), the country and continent only when that colo isn't known.
import { COLO_HINTS } from "./colo-hints";

/** Cloudflare's Durable Object location hints (developers.cloudflare.com/durable-objects/reference/
    data-location/, checked 2026-11-02). sam, afr and me host no Durable Objects yet: Cloudflare puts
    those rooms in the nearest region that does (South America's in eastern North America). */
export const LOCATION_HINTS = [
  "wnam", "enam", "sam", "weur", "eeur", "apac", "apac-ne", "apac-se", "oc", "afr", "me",
] as const;
export type LocationHint = (typeof LOCATION_HINTS)[number];

/** What a room or a lobby request knows of a connection: Cloudflare's request.cf, never the IP. */
export interface Place {
  colo?: string;
  country?: string;
  continent?: string;
  regionCode?: string;
  longitude?: string | number;
}

export function placeOf(request: Request): Place {
  const cf = (request.cf ?? {}) as Record<string, unknown>;
  const str = (v: unknown) => (typeof v === "string" ? v : undefined);
  return {
    colo: str(cf.colo),
    country: str(cf.country) ?? request.headers.get("cf-ipcountry") ?? undefined,
    continent: str(cf.continent),
    regionCode: str(cf.regionCode),
    longitude: str(cf.longitude) ?? (typeof cf.longitude === "number" ? cf.longitude : undefined),
  };
}

// Western Europe's Durable Object locations are AMS, LHR, CDG, MAD, MRS and LIS; eastern Europe's
// FRA, MXP, WAW, ARN, OTP, VIE and PRG (where.durableobjects.live, 2026-11-02). So "west" here is
// the Atlantic side; Germany, Italy, the Nordics and everything east of them are eeur.
const WEST_EUROPE = new Set(["GB", "IE", "FR", "ES", "PT", "NL", "BE", "LU", "IS", "AD", "MC", "GI", "IM", "JE", "GG", "FO"]);
// Northeast and southeast Asia have their own hints (2026-06); the rest of Asia takes apac.
const NORTHEAST_ASIA = new Set(["JP", "KR", "KP", "TW", "HK", "MO", "CN", "MN"]);
const SOUTHEAST_ASIA = new Set(["SG", "MY", "ID", "TH", "VN", "PH", "KH", "LA", "MM", "BN", "TL"]);
// Middle East countries Cloudflare's geolocation puts in Asia (continent AS).
const MIDDLE_EAST = new Set(["AE", "SA", "QA", "BH", "KW", "OM", "YE", "IQ", "IR", "IL", "PS", "JO", "LB", "SY"]);
// North America west of the Rockies, when there is no longitude: US states and Canadian provinces.
const WEST_US = new Set(["CA", "OR", "WA", "NV", "AZ", "UT", "ID", "MT", "WY", "CO", "NM", "AK", "HI"]);
const WEST_CA = new Set(["BC", "AB", "YT", "NT"]);

/** The location hint for a room its creator makes from `p`, or undefined (no data: no hint, the
    default placement). */
export function locationHintFor(p: Place): LocationHint | undefined {
  const country = (p.country ?? "").toUpperCase();
  switch ((p.continent ?? "").toUpperCase()) {
    case "EU":
      return WEST_EUROPE.has(country) ? "weur" : "eeur";
    case "NA": {
      const lon = Number(p.longitude);
      if (p.longitude !== undefined && p.longitude !== "" && Number.isFinite(lon)) return lon < -100 ? "wnam" : "enam";
      const region = (p.regionCode ?? "").toUpperCase();
      if (country === "US") return WEST_US.has(region) ? "wnam" : "enam";
      if (country === "CA") return WEST_CA.has(region) ? "wnam" : "enam";
      return country === "MX" ? "wnam" : "enam";
    }
    case "SA":
      return "sam";
    case "AS":
      if (MIDDLE_EAST.has(country)) return "me";
      if (NORTHEAST_ASIA.has(country)) return "apac-ne";
      if (SOUTHEAST_ASIA.has(country)) return "apac-se";
      return "apac";
    case "OC":
      return "oc";
    case "AF":
      return "afr";
    case "ME":
      return "me";
    default:
      return undefined;
  }
}

/** How the hint of a new room was picked: the creator's choice, Auto by the colo they reached, Auto
    by their country and continent, or none (the default placement). */
export type HintSource = "chosen" | "colo" | "place" | "none";

/** The hint for a room the creator makes from `p`, with `choice` the region they picked (undefined:
    Auto). Auto takes the creator's colo's row in src/colo-hints.ts: their ping to the room is their
    way to that colo plus the colo's to the room, and the first part is the same wherever the room
    runs. A colo the table doesn't have (a new one) falls back to the country and continent. */
export function placeRoom(p: Place, choice?: LocationHint): { hint?: LocationHint; how: HintSource } {
  if (choice) return { hint: choice, how: "chosen" };
  const byColo = p.colo ? COLO_HINTS[p.colo.toUpperCase()] : undefined;
  if (byColo) return { hint: byColo, how: "colo" };
  const byPlace = locationHintFor(p);
  return byPlace ? { hint: byPlace, how: "place" } : { how: "none" };
}

/** The regions a room's creator can pick (Create's REGION row, finding 181): the hints that run
    Durable Objects of their own, each with the name players see and the colos it ran in on
    2026-11-08 (where.durableobjects.live). sam, afr and me run none (their rooms land in enam, weur
    and eeur), and apac only spreads rooms over apac-ne's and apac-se's colos, so they are not
    offered. */
export const CHOICES: { hint: LocationHint; area: string; colos: string }[] = [
  { hint: "weur", area: "EU WEST", colos: "AMS LHR CDG MAD MRS LIS" },
  { hint: "eeur", area: "EU EAST", colos: "FRA MXP WAW VIE OTP ARN PRG" },
  { hint: "enam", area: "NA EAST", colos: "ATL EWR MIA ORD IAD" },
  { hint: "wnam", area: "NA WEST", colos: "DFW SJC LAX DEN SEA" },
  { hint: "apac-ne", area: "ASIA NE", colos: "NRT KIX ICN" },
  { hint: "apac-se", area: "ASIA SE", colos: "SIN HKG" },
  { hint: "oc", area: "OCEANIA", colos: "SYD MEL BNE" },
];

/** The creator's pick from `/v1/rooms/new?region=`: one of CHOICES' hints; anything else (none,
    "auto", an unknown or retired name) is Auto, so a newer or older game never fails to make a
    room over it. */
export function regionChoice(value: string | null | undefined): LocationHint | undefined {
  const v = (value ?? "").trim().toLowerCase();
  return CHOICES.find((c) => c.hint === v)?.hint;
}

const COLO_AREAS: Record<string, string> = {};
for (const c of CHOICES) for (const colo of c.colos.split(" ")) COLO_AREAS[colo] = c.area;

/** Where a room runs, in the finer names of CHOICES ("EU WEST"...): its colo's when that colo is
    one of theirs, else the hint's (sam, afr and me by the region that serves them, apac "ASIA"),
    "" unknown. */
export function roomArea(colo: string | undefined, hint: string | undefined): string {
  const byColo = colo ? COLO_AREAS[colo.toUpperCase()] : undefined;
  if (byColo) return byColo;
  const served: Record<string, string> = { sam: "enam", afr: "weur", me: "eeur" };
  const h = served[hint ?? ""] ?? hint;
  return CHOICES.find((c) => c.hint === h)?.area ?? (h === "apac" ? "ASIA" : "");
}

/** The short region players see. */
export type Region = "EU" | "NA" | "SA" | "ASIA" | "OC" | "AF" | "ME";

export function regionOfHint(hint: string | undefined): Region | "" {
  switch (hint) {
    case "weur": case "eeur": return "EU";
    case "wnam": case "enam": return "NA";
    case "sam": return "SA";
    case "apac": case "apac-ne": case "apac-se": return "ASIA";
    case "oc": return "OC";
    case "afr": return "AF";
    case "me": return "ME";
    default: return "";
  }
}

/** The region of a continent code (request.cf.continent; "T1" Tor and the rest: none). */
export function regionOfContinent(continent: string | undefined): Region | "" {
  switch ((continent ?? "").toUpperCase()) {
    case "EU": return "EU";
    case "NA": return "NA";
    case "SA": return "SA";
    case "AS": return "ASIA";
    case "OC": return "OC";
    case "AF": return "AF";
    default: return "";
  }
}

/** The region of a place: its continent, Middle East countries as ME (as their hint). */
export function regionOfPlace(p: Place): Region | "" {
  return regionOfHint(locationHintFor(p)) || regionOfContinent(p.continent);
}

// The Cloudflare locations that run Durable Objects, by region (where.durableobjects.live,
// 2026-11-02). A room's own colo in this table names its region; one not in it falls back to the
// region of the hint it was made with.
const DO_COLOS: Record<string, Region> = {};
for (const [region, colos] of [
  ["EU", "AMS LHR CDG MAD MRS LIS FRA MXP WAW ARN OTP VIE PRG"],
  ["NA", "DFW SJC LAX SEA DEN ATL EWR MIA IAD ORD"],
  ["ASIA", "SIN HKG NRT KIX ICN"],
  ["OC", "SYD BNE MEL"],
] as const)
  for (const c of colos.split(" ")) DO_COLOS[c] = region;

/** A room's region: where it runs if that colo is known, else where it was meant to run. */
export function roomRegion(colo: string | undefined, hint: string | undefined): Region | "" {
  return (colo && DO_COLOS[colo.toUpperCase()]) || regionOfHint(hint);
}

/** The colo of a Cloudflare trace (`/cdn-cgi/trace`'s text: `key=value` lines), or "". A Durable
    Object that fetches one learns the Cloudflare location it runs in: request.cf.colo inside the
    object is the client's edge, not the object's. */
export function traceColo(text: string): string {
  const m = text.match(/^colo=([A-Z0-9]{3,4})\s*$/m);
  return m ? m[1] : "";
}

/** The public list for a player in `region`: first rooms made in their region (their hint put the
    room where it puts this player's: South America's rooms run in North America, but they are
    still the near ones for a Brazilian), then rooms that run in it, then the rest; each part
    busiest first. Releases up to 0.7.6 read the first 8 rooms and their Quick Race joins the first
    one with room, so this order is what makes them prefer a near room too. */
export function sortForRegion<T extends { players: number; region?: string; createdIn?: string }>(rooms: T[], region: string): T[] {
  const near = (x: T) => (!region ? 0 : x.createdIn === region ? 0 : x.region === region ? 1 : 2);
  return [...rooms].sort((a, b) => near(a) - near(b) || b.players - a.players);
}
