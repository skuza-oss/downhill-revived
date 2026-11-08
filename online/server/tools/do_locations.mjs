#!/usr/bin/env node
// The table of where a new room should run for each Cloudflare location its creator reaches
// (src/colo-hints.ts, finding 181).
//
//   node tools/do_locations.mjs fetch   saves tools/do-locations.json from where.durableobjects.live:
//                                       every Cloudflare location (colo) with its coordinates and
//                                       whether it runs Durable Objects, and for each location hint
//                                       the colos that serve it and how often
//   node tools/do_locations.mjs         writes src/colo-hints.ts from tools/do-locations.json and
//                                       tools/do-rtt.json (measured round trips)
//   node tools/do_locations.mjs check   fails when src/colo-hints.ts is not what the data gives
//   node tools/do_locations.mjs report  the measured colos' three nearest hints, ms
//
// tools/do-rtt.json: for each Cloudflare location (colo) a measuring request reached, the quickest
// round trip from a Worker there to a Durable Object in each colo that runs them (finding 181 says
// how it was measured). A colo gets a row only when the data says something about it:
// - measured: the hint whose two quickest colos are quickest on average (a hint's objects land in
//   its colos near the edge the first request came through: weur from CDG gave CDG and LHR only,
//   from MAD gave MAD, MRS and LIS, 2026-11-08);
// - a neighbour: within NEIGHBOUR_KM of a measured colo, that colo's choice;
// - its own: a colo that runs Durable Objects, its own region's hint.
// Any other colo has no row, and the server falls back to the creator's country and continent:
// great-circle distance was tried and dropped, it put Johannesburg, Beirut and Tel Aviv in eeur
// where measurements say weur, and Fortaleza in weur across the Atlantic. where.durableobjects.live
// is the reference Cloudflare's data-location docs link (refreshed every five minutes).
import { readFileSync, writeFileSync, existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const SNAPSHOT = join(here, "do-locations.json");
const MEASURED = join(here, "do-rtt.json");
const OUT = join(here, "..", "src", "colo-hints.ts");
const SITE = "https://where.durableobjects.live";
// The hints a room can be given; sam, afr and me run no objects (their rooms land in enam, weur and
// eeur: the site's own region pages), so they are never the nearest.
const HINTS = ["wnam", "enam", "sam", "weur", "eeur", "apac", "apac-ne", "apac-se", "oc", "afr", "me"];
const NO_OBJECTS = ["sam", "afr", "me"];
// apac spreads its objects over both apac-ne's and apac-se's colos: one of those two is always as
// near or nearer, so apac is never given.
const CANDIDATES = HINTS.filter((h) => !NO_OBJECTS.includes(h) && h !== "apac");

async function fetchSnapshot() {
  const page = async (path) => {
    const r = await fetch(SITE + path);
    if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
    return r.text();
  };
  const all = await page("/region");
  const colos = {};
  for (const m of all.matchAll(/([A-Z0-9]{3,4}):\{name:"([^"]*)",coords:\[([-\d.e]+),([-\d.e]+)\],status:"(\w+)",isDOCapable:(true|false)\}/g))
    colos[m[1]] = { name: m[2], lon: Number(m[3]), lat: Number(m[4]), objects: m[6] === "true" };
  const updated = Number(all.match(/updatedAt:(\d+)/)?.[1] ?? 0);
  const serves = {};
  for (const hint of HINTS) {
    const text = await page(`/region/${hint}`);
    serves[hint] = Object.fromEntries([...text.matchAll(/\{code:"([A-Z0-9]{3,4})",name:"[^"]*",likelihood:([\d.]+)\}/g)]
      .map((m) => [m[1], Number(m[2]) / 100]));
    if (!Object.keys(serves[hint]).length) throw new Error(`${hint}: no colos found (the page changed?)`);
  }
  if (Object.keys(colos).length < 200) throw new Error(`only ${Object.keys(colos).length} colos found (the page changed?)`);
  return { source: SITE, updated: new Date(updated).toISOString(), serves, colos };
}

const rad = (d) => (d * Math.PI) / 180;
export function km(a, b) {
  const h = Math.sin(rad(b.lat - a.lat) / 2) ** 2 + Math.cos(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.sin(rad(b.lon - a.lon) / 2) ** 2;
  return 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(h)));
}

// The country of each colo's place name (where.durableobjects.live: "City, Region, Country") as
// Cloudflare names countries: two letters and the continent code request.cf.continent gives (GeoNames'
// continents: TR, GE, AM, AZ, KZ in AS; CY and RU in EU). The server compares the region of the colo
// a creator reached with the creator's own (src/region.ts placeRoom: Auto uses the colo only on the
// creator's continent). A colo of the table whose country is missing here stops the generation.
const COUNTRIES = {
  "Albania": "AL EU", "Argentina": "AR SA", "Armenia": "AM AS", "Australia": "AU OC", "Austria": "AT EU",
  "Azerbaijan": "AZ AS", "Bahrain": "BH AS", "Barbados": "BB NA", "Belarus": "BY EU", "Belgium": "BE EU",
  "Bhutan": "BT AS", "Botswana": "BW AF", "Brazil": "BR SA", "Bulgaria": "BG EU", "Cambodia": "KH AS",
  "Canada": "CA NA", "China": "CN AS", "Costa Rica": "CR NA", "Croatia": "HR EU", "Cyprus": "CY EU",
  "Czech Republic": "CZ EU", "Denmark": "DK EU", "Ecuador": "EC SA", "Egypt": "EG AF", "Estonia": "EE EU",
  "Finland": "FI EU", "France": "FR EU", "Georgia": "GE AS", "Germany": "DE EU", "Greece": "GR EU",
  "Grenada": "GD NA", "Guatemala": "GT NA", "Guyana": "GY SA", "Honduras": "HN NA", "Hong Kong": "HK AS",
  "Hungary": "HU EU", "India": "IN AS", "Indonesia": "ID AS", "Iraq": "IQ AS", "Ireland": "IE EU",
  "Israel": "IL AS", "Italy": "IT EU", "Japan": "JP AS", "Jordan": "JO AS", "Kazakhstan": "KZ AS",
  "Kosovo": "XK EU", "Kuwait": "KW AS", "Laos": "LA AS", "Latvia": "LV EU", "Lebanon": "LB AS",
  "Lithuania": "LT EU", "Luxembourg": "LU EU", "Macau": "MO AS", "Malaysia": "MY AS", "Moldova": "MD EU",
  "Mozambique": "MZ AF", "Nepal": "NP AS", "Netherlands": "NL EU", "New Zealand": "NZ OC",
  "North Macedonia": "MK EU", "Norway": "NO EU", "Oman": "OM AS", "Pakistan": "PK AS",
  "Palestinian Territory": "PS AS", "Panama": "PA NA", "Philippines": "PH AS", "Poland": "PL EU",
  "Portugal": "PT EU", "Qatar": "QA AS", "Romania": "RO EU", "Russia": "RU EU", "Saudi Arabia": "SA AS",
  "Serbia": "RS EU", "Singapore": "SG AS", "Slovakia": "SK EU", "Slovenia": "SI EU", "South Africa": "ZA AF",
  "South Korea": "KR AS", "Spain": "ES EU", "Sweden": "SE EU", "Switzerland": "CH EU", "Thailand": "TH AS",
  "Trinidad and Tobago": "TT NA", "Turkey": "TR AS", "United Arab Emirates": "AE AS", "United Kingdom": "GB EU",
  "United States": "US NA",
};

/** A colo's country and continent ("US NA") from its place name; throws for an unknown country. */
export function coloCountry(snapshot, code) {
  const country = snapshot.colos[code].name.split(", ").pop();
  const cc = COUNTRIES[country];
  if (!cc) throw new Error(`${code}: no country code for "${country}" (tools/do_locations.mjs COUNTRIES)`);
  return cc;
}

/** A measured colo's choice stands for unmeasured colos this near it (km): the same city or
    neighbours, which reach the rest of Cloudflare by the same routes. */
export const NEIGHBOUR_KM = 600;

/** For each colo the data speaks for: the hint whose rooms run nearest it, and how it was decided
    ("measured", "neighbour" of a measured colo, or "own": it runs objects itself). */
export function table(snapshot, measured = {}, neighbourKm = NEIGHBOUR_KM) {
  // sam, afr and me are served by enam's, weur's and eeur's colos (their shares differ only by
  // sampling): they are left out, so a room is given the hint that names where it runs.
  const hints = CANDIDATES.filter((h) => Object.keys(snapshot.serves[h]).every((c) => snapshot.colos[c]));
  const own = {};
  for (const hint of hints) for (const c of Object.keys(snapshot.serves[hint])) own[c] = hint;
  const nearest = (cost) => {
    let best = null;
    for (const hint of hints) {
      const costs = Object.keys(snapshot.serves[hint]).map(cost).sort((a, b) => a - b);
      if (costs.some((c) => c === undefined || !Number.isFinite(c))) return null;
      const c = (costs[0] + costs[Math.min(1, costs.length - 1)]) / 2;
      if (!best || c < best.cost) best = { hint, cost: Math.round(c) };
    }
    return best;
  };
  const out = {};
  for (const code of Object.keys(measured)) {
    const best = nearest((c) => measured[code][c]);
    if (best) out[code] = { ...best, how: "measured" };
  }
  for (const [code, colo] of Object.entries(snapshot.colos)) {
    if (out[code]?.how === "measured") continue;
    const near = Object.keys(measured).filter((m) => out[m]?.how === "measured" && snapshot.colos[m])
      .map((m) => ({ m, d: km(colo, snapshot.colos[m]) })).sort((a, b) => a.d - b.d)[0];
    if (own[code]) out[code] = { hint: own[code], cost: 0, how: "own" };
    else if (near && near.d <= neighbourKm) out[code] = { hint: out[near.m].hint, cost: Math.round(near.d), how: "neighbour", of: near.m };
  }
  return out;
}

function render(snapshot, rows) {
  const how = { measured: 0, neighbour: 0, own: 0 };
  for (const r of Object.values(rows)) how[r.how]++;
  const byHint = {};
  for (const [code, { hint }] of Object.entries(rows).sort()) (byHint[hint] ??= []).push(code);
  const lines = Object.entries(byHint).sort().map(([hint, codes]) => {
    const wrapped = [];
    for (let i = 0; i < codes.length; i += 16) wrapped.push(codes.slice(i, i + 16).join(" "));
    return `  ["${hint}", ${wrapped.map((w) => JSON.stringify(w)).join(" +\n    \" \" + ")}],`;
  });
  const byCountry = {};
  for (const code of Object.keys(rows).sort()) (byCountry[coloCountry(snapshot, code)] ??= []).push(code);
  const countryLines = Object.entries(byCountry).sort().map(([cc, codes]) => `  [${JSON.stringify(cc)}, ${JSON.stringify(codes.join(" "))}],`);
  return `// Generated by tools/do_locations.mjs from tools/do-locations.json (${snapshot.source}, ${snapshot.updated.slice(0, 10)})
// and tools/do-rtt.json. Do not edit: change the data or the tool and run it again (finding 181).
//
// For a Cloudflare location (colo) a room's creator reaches: the location hint whose Durable Objects
// run nearest it. ${how.measured} colos by measured round trips, ${how.neighbour} by a measured colo within ${NEIGHBOUR_KM} km,
// ${how.own} that run objects themselves; a colo not here falls back to the creator's country.
import type { LocationHint } from "./region";

const BY_HINT: [LocationHint, string][] = [
${lines.join("\n")}
];

export const COLO_HINTS: Record<string, LocationHint> = {};
for (const [hint, colos] of BY_HINT) for (const c of colos.split(" ")) COLO_HINTS[c] = hint;

// Where each of those colos is: its country and continent as request.cf gives them for a player
// there ("US NA"), for placeRoom's same-continent rule.
const BY_COUNTRY: [string, string][] = [
${countryLines.join("\n")}
];

export const COLO_COUNTRIES: Record<string, { country: string; continent: string }> = {};
for (const [where, colos] of BY_COUNTRY) {
  const [country, continent] = where.split(" ");
  for (const c of colos.split(" ")) COLO_COUNTRIES[c] = { country, continent };
}
`;
}

const main = process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1];
const mode = main ? process.argv[2] ?? "write" : "none";
if (mode === "none") {
  // imported (a test or an analysis): nothing to do
} else if (mode === "fetch") {
  const snapshot = await fetchSnapshot();
  writeFileSync(SNAPSHOT, JSON.stringify(snapshot, null, 1) + "\n");
  console.log(`${SNAPSHOT}: ${Object.keys(snapshot.colos).length} colos, ${Object.values(snapshot.colos).filter((c) => c.objects).length} run objects (${snapshot.updated})`);
} else if (mode === "report") {
  // The measured colos: the chosen hint and the next two, each the mean of its two quickest colos.
  const snapshot = JSON.parse(readFileSync(SNAPSHOT, "utf8"));
  const measured = JSON.parse(readFileSync(MEASURED, "utf8")).rtt;
  for (const [code, rtt] of Object.entries(measured)) {
    const costs = CANDIDATES.map((h) => {
      const v = Object.keys(snapshot.serves[h]).map((c) => rtt[c]).sort((a, b) => a - b);
      return [h, Math.round((v[0] + v[Math.min(1, v.length - 1)]) / 2)];
    }).sort((a, b) => a[1] - b[1]);
    console.log(`| ${code} | ${costs.slice(0, 3).map(([h, c]) => `${h} ${c}`).join(", ")} |`);
  }
} else if (mode === "write" || mode === "check") {
  const snapshot = JSON.parse(readFileSync(SNAPSHOT, "utf8"));
  const measured = existsSync(MEASURED) ? JSON.parse(readFileSync(MEASURED, "utf8")).rtt : {};
  const rows = table(snapshot, measured);
  const text = render(snapshot, rows);
  if (mode === "check") {
    const now = existsSync(OUT) ? readFileSync(OUT, "utf8").replace(/\r\n/g, "\n") : "";
    if (now !== text) { console.error(`${OUT} is not what the data gives: run node tools/do_locations.mjs`); process.exit(1); }
    console.log("colo-hints.ts: up to date");
  } else {
    writeFileSync(OUT, text);
    const counts = {};
    for (const { hint } of Object.values(rows)) counts[hint] = (counts[hint] ?? 0) + 1;
    console.log(`${OUT}: ${Object.keys(rows).length} colos`, counts);
  }
} else {
  console.error("usage: node tools/do_locations.mjs [fetch|check]");
  process.exit(2);
}
