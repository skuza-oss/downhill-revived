// The room's region picked by its creator, Auto by the colo they reached, and what 0.7.13 games
// still get (src/region.ts, src/colo-hints.ts, finding 181).
import { env, runInDurableObject } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { describe, it, expect } from "vitest";
import { COLO_HINTS } from "../src/colo-hints";
import { COLO_COUNTRIES } from "../src/colo-hints";
import { CHOICES, LOCATION_HINTS, continentOf, placeRoom, regionChoice, roomArea, roomRegion } from "../src/region";
import { Client } from "./helpers";

const POLAND = { colo: "WAW", country: "PL", continent: "EU", longitude: "21.0" };
// A player in Qatar whose ISP reaches Cloudflare in Marseille (Globalping's Doha probe did).
const QATAR_VIA_MRS = { colo: "MRS", country: "QA", continent: "AS" };
// The same country through a Cloudflare location the table doesn't have.
const QATAR_UNKNOWN_COLO = { colo: "ZZZ", country: "QA", continent: "AS" };

async function origin(code: string): Promise<any> {
  let o: any;
  await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (r: any) => { o = r.state.origin; });
  return o;
}

const auto = (colo: string, country = "", continent = "") => placeRoom({ colo, country, continent });

describe("Auto by the creator's colo", () => {
  it("puts rooms where the measured round trips say, the colo on the creator's continent", () => {
    // measured from the colo itself (tools/do-rtt.json)
    expect(auto("JNB", "ZA", "AF")).toEqual({ hint: "weur", how: "colo" });  // weur 141 ms, eeur 181
    expect(auto("KWI", "KW", "AS")).toEqual({ hint: "weur", how: "colo" });  // was me -> eeur by country
    expect(auto("BOM", "IN", "AS")).toEqual({ hint: "apac-se", how: "colo" });  // was apac (all of Asia)
    expect(auto("SIN", "ID", "AS")).toEqual({ hint: "apac-se", how: "colo" });
    expect(auto("GIG", "PE", "SA")).toEqual({ hint: "enam", how: "colo" });  // a Peruvian in Rio: same continent
    expect(auto("GRU", "BR", "SA")).toEqual({ hint: "enam", how: "colo" });
    expect(auto("EZE", "AR", "SA")).toEqual({ hint: "enam", how: "colo" });
    expect(auto("DFW", "MX", "NA")).toEqual({ hint: "wnam", how: "colo" });
    expect(auto("SYD", "AU", "OC")).toEqual({ hint: "oc", how: "colo" });
    expect(auto("FRA", "DE", "EU")).toEqual({ hint: "eeur", how: "colo" });
    expect(auto("CDG", "FR", "EU")).toEqual({ hint: "weur", how: "colo" });
    expect(auto("CDG", "DE", "EU")).toEqual({ hint: "weur", how: "colo" });  // a German in Paris: Europe either way
    // a measured neighbour within 600 km: Rio de Janeiro's choice stands for Sao Paulo's peers
    expect(auto("VCP", "BR", "SA")).toEqual({ hint: "enam", how: "colo" });
    expect(auto("cdg", "fr", "eu").hint).toBe("weur");  // any case
  });

  it("uses the country rule when the colo is on another continent (the owner's rule, 2026-11-08)", () => {
    // Indonesians whose ISP reaches Tokyo, Paris, San Jose (Workers Logs, 2026-11-01..08): their room
    // stays in Southeast Asia, near the other Indonesians, as in 0.7.13
    for (const colo of ["NRT", "CDG", "SJC", "FRA", "LHR", "DFW"])
      expect(auto(colo, "ID", "AS"), colo).toEqual({ hint: "apac-se", how: "colo-other-continent" });
    expect(auto("MRS", "QA", "AS")).toEqual({ hint: "me", how: "colo-other-continent" });  // Doha in Marseille
    expect(auto("MIA", "CO", "SA")).toEqual({ hint: "sam", how: "colo-other-continent" });  // Bogota in Miami: enam's colos anyway
    expect(auto("MRS", "DZ", "AF")).toEqual({ hint: "afr", how: "colo-other-continent" });  // Algiers in Marseille: weur's
    // no country: no continent to compare, so no colo either
    expect(auto("CDG")).toEqual({ how: "none" });
  });

  it("knows each table colo's country, and splits Asia as the country rule does", () => {
    for (const colo of Object.keys(COLO_HINTS)) expect(COLO_COUNTRIES[colo], colo).toBeDefined();
    expect(COLO_COUNTRIES.NRT).toEqual({ country: "JP", continent: "AS" });
    expect(COLO_COUNTRIES.GIG).toEqual({ country: "BR", continent: "SA" });
    expect(continentOf({ country: "ID", continent: "AS" })).toBe("apac-se");
    expect(continentOf({ country: "JP", continent: "AS" })).toBe("apac-ne");
    expect(continentOf({ country: "KW", continent: "AS" })).toBe("me");
    expect(continentOf({ country: "IN", continent: "AS" })).toBe("apac");
    expect(continentOf({ country: "PE", continent: "SA" })).toBe("SA");
    expect(continentOf({ continent: "T1" })).toBe("");
  });

  it("falls back to the country and continent for a colo the table doesn't have, then to none", () => {
    expect(placeRoom(QATAR_UNKNOWN_COLO)).toEqual({ hint: "me", how: "country" });
    // Fortaleza: unmeasured and far from any measured colo, so no row (great circles put it in weur)
    expect(COLO_HINTS.FOR).toBeUndefined();
    expect(placeRoom({ colo: "FOR", country: "BR", continent: "SA" })).toEqual({ hint: "sam", how: "country" });
    expect(placeRoom({ country: "DE", continent: "EU" })).toEqual({ hint: "eeur", how: "country" });
    expect(placeRoom({})).toEqual({ how: "none" });
    expect(placeRoom({ continent: "T1" })).toEqual({ how: "none" });  // Tor
  });

  it("has rows only for colos the data speaks for, each a hint that runs objects", () => {
    expect(Object.keys(COLO_HINTS).length).toBeGreaterThanOrEqual(150);
    const offered = new Set(CHOICES.map((c) => c.hint));
    for (const [colo, hint] of Object.entries(COLO_HINTS)) {
      expect(LOCATION_HINTS, colo).toContain(hint);
      expect(offered.has(hint), `${colo} -> ${hint}`).toBe(true);  // never sam, afr, me or apac
    }
    // every colo that runs objects is placed in its own region
    for (const c of CHOICES) for (const colo of c.colos.split(" ")) expect(COLO_HINTS[colo], colo).toBe(c.hint);
  });
});

describe("the creator's pick", () => {
  it("takes the offered hints only; anything else is Auto", () => {
    for (const c of CHOICES) expect(regionChoice(c.hint)).toBe(c.hint);
    expect(regionChoice("WEUR")).toBe("weur");
    for (const v of [null, undefined, "", "auto", "AUTO", "sam", "afr", "me", "apac", "mars", "weur;drop"])
      expect(regionChoice(v), String(v)).toBeUndefined();
    expect(placeRoom(POLAND, "oc")).toEqual({ hint: "oc", how: "chosen" });
  });

  it("names where a room runs: its colo's area, else its hint's", () => {
    expect(roomArea("LHR", "eeur")).toBe("EU WEST");  // the colo wins
    expect(roomArea("SIN", undefined)).toBe("ASIA SE");
    expect(roomArea("", "apac-ne")).toBe("ASIA NE");
    expect(roomArea(undefined, "sam")).toBe("NA EAST");  // where sam's rooms land
    expect(roomArea(undefined, "afr")).toBe("EU WEST");
    expect(roomArea(undefined, "me")).toBe("EU EAST");
    expect(roomArea(undefined, "apac")).toBe("ASIA");
    expect(roomArea("XYZ", "")).toBe("");
    // the area always agrees with the coarse region
    const coarse: Record<string, string> = { "EU WEST": "EU", "EU EAST": "EU", "NA EAST": "NA", "NA WEST": "NA", "ASIA NE": "ASIA", "ASIA SE": "ASIA", OCEANIA: "OC" };
    for (const c of CHOICES) for (const colo of c.colos.split(" ")) expect(coarse[roomArea(colo, "")], colo).toBe(roomRegion(colo, ""));
  });

  it("makes the room with the picked hint and says so", async () => {
    const host = await Client.connect("/v1/rooms/new?region=apac-se", POLAND);
    host.hello("Pole", { features: ["session", "countdown"] });
    const welcome = await host.json("welcome");
    // before the room's trace answers, the area is the hint's
    expect(welcome).toMatchObject({ edgeColo: "WAW", roomColo: "", roomRegion: "ASIA", roomArea: "ASIA SE", roomChosen: true });
    expect(await origin(welcome.code)).toEqual({ hint: "apac-se", how: "chosen", createdIn: "EU", createdFrom: "WAW" });
    host.sendJson({ type: "settings", public: true });
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(welcome.code)), async (r: any) => { r.state.colo = "HKG"; });
    const guest = await Client.connect(`/v1/rooms/${welcome.code}`, POLAND);
    guest.hello("Guest");
    expect(await guest.json("welcome")).toMatchObject({ roomColo: "HKG", roomRegion: "ASIA", roomArea: "ASIA SE", roomChosen: true });
    const where = JSON.parse(await host.next((f) => typeof f === "string" && JSON.parse(f).type === "where" && JSON.parse(f).members.length === 2) as string);
    expect(where).toMatchObject({ region: "ASIA", area: "ASIA SE", colo: "HKG" });
    await expect.poll(async () => {
      const list = (await (await exports.default.fetch("https://dhd.test/v1/lobbies", { cf: POLAND })).json<any>()).lobbies;
      return list.find((l: any) => l.code === welcome.code);
    }).toMatchObject({ region: "ASIA", area: "ASIA SE", colo: "HKG", createdIn: "EU" });
  });

  it("makes an Auto room for an unknown or bad pick", async () => {
    for (const q of ["?region=auto", "?region=sam", "?region=%3Cx%3E", "?region=weur&region=oc"]) {
      const host = await Client.connect(`/v1/rooms/new${q}`, QATAR_VIA_MRS);
      host.hello("Doha");
      const welcome = await host.json("welcome");
      const o = await origin(welcome.code);
      if (q.startsWith("?region=weur")) expect(o, q).toMatchObject({ hint: "weur", how: "chosen" });  // the first one
      else expect(o, q).toMatchObject({ hint: "me", how: "colo-other-continent" });
      expect(welcome.roomChosen, q).toBe(q.startsWith("?region=weur"));
    }
  });
});

describe("games up to 0.7.13", () => {
  it("create and join as before: no region sent, Auto, every field they read still there", async () => {
    // 0.7.13's hello (port/net/client.c) and its create path
    const hello = { protocol: 4, clientVersion: "0.7.13", features: ["countdown", "session", "race_rejoin"] };
    const host = await Client.connect("/v1/rooms/new", QATAR_VIA_MRS);
    host.hello("Old", hello);
    const welcome = await host.json("welcome");
    expect(welcome).toMatchObject({
      type: "welcome", host: true, memberId: 0, edgeColo: "MRS", country: "QA", continent: "AS",
      roomColo: "", roomRegion: "ME", sfu: "setup",
    });
    expect(typeof welcome.code).toBe("string");
    expect(typeof welcome.serverTime).toBe("number");
    expect(typeof welcome.rejoinToken).toBe("string");
    expect(await origin(welcome.code)).toEqual({ hint: "me", how: "colo-other-continent", createdIn: "ME", createdFrom: "MRS" });
    host.sendJson({ type: "settings", public: true });
    const guest = await Client.connect(`/v1/rooms/${welcome.code}`, POLAND);
    guest.hello("Old2", hello);
    expect(await guest.json("welcome")).toMatchObject({ host: false, roomRegion: "ME" });
    // the list keeps every key 0.7.13 reads
    await expect.poll(async () => {
      const r = await (await exports.default.fetch("https://dhd.test/v1/lobbies", { cf: POLAND })).json<any>();
      const row = r.lobbies.find((l: any) => l.code === welcome.code);
      return row && Object.keys(row).sort();
    }).toEqual(["area", "code", "colo", "createdIn", "full", "hostName", "maxPlayers", "nextRaceAt", "phase", "players",
      "raceStartedAt", "region", "roomName", "track", "version", "finishEndsAt"].sort());
  });
});
