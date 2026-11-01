// Rooms near their creator, regions players see, and the list's order (src/region.ts, finding 135).
import { env, runInDurableObject } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { describe, it, expect } from "vitest";
import {
  LOCATION_HINTS,
  locationHintFor,
  regionOfPlace,
  roomRegion,
  sortForRegion,
  traceColo,
} from "../src/region";
import { Client } from "./helpers";

describe("location hints", () => {
  it("map the creator's continent and country to Cloudflare's hints", () => {
    const cases: [Record<string, string | number>, string | undefined][] = [
      [{ continent: "EU", country: "GB" }, "weur"],
      [{ continent: "EU", country: "FR" }, "weur"],
      [{ continent: "EU", country: "ES" }, "weur"],
      [{ continent: "EU", country: "DE" }, "eeur"],
      [{ continent: "EU", country: "PL" }, "eeur"],
      [{ continent: "EU", country: "SE" }, "eeur"],
      [{ continent: "EU", country: "" }, "eeur"],
      [{ continent: "NA", country: "US", longitude: "-122.4" }, "wnam"],
      [{ continent: "NA", country: "US", longitude: "-74.0" }, "enam"],
      [{ continent: "NA", country: "US", regionCode: "WA" }, "wnam"],
      [{ continent: "NA", country: "US", regionCode: "TX" }, "enam"],
      [{ continent: "NA", country: "CA", regionCode: "BC" }, "wnam"],
      [{ continent: "NA", country: "CA", regionCode: "ON" }, "enam"],
      [{ continent: "NA", country: "MX" }, "wnam"],
      [{ continent: "SA", country: "BR" }, "sam"],
      [{ continent: "AS", country: "JP" }, "apac-ne"],
      [{ continent: "AS", country: "KR" }, "apac-ne"],
      [{ continent: "AS", country: "SG" }, "apac-se"],
      [{ continent: "AS", country: "IN" }, "apac"],
      [{ continent: "AS", country: "AE" }, "me"],
      [{ continent: "OC", country: "AU" }, "oc"],
      [{ continent: "AF", country: "ZA" }, "afr"],
      [{ continent: "T1", country: "T1" }, undefined],
      [{}, undefined],
    ];
    for (const [place, hint] of cases) {
      expect(locationHintFor(place), JSON.stringify(place)).toBe(hint);
      if (hint) expect(LOCATION_HINTS).toContain(hint);
    }
  });

  it("name short regions; a known Durable Object colo wins over the hint", () => {
    expect(regionOfPlace({ continent: "EU", country: "PL" })).toBe("EU");
    expect(regionOfPlace({ continent: "AS", country: "JP" })).toBe("ASIA");
    expect(regionOfPlace({ continent: "AS", country: "AE" })).toBe("ME");
    expect(regionOfPlace({ continent: "SA", country: "BR" })).toBe("SA");
    expect(regionOfPlace({})).toBe("");
    expect(roomRegion("FRA", "wnam")).toBe("EU");
    expect(roomRegion("IAD", "sam")).toBe("NA");
    expect(roomRegion("XYZ", "apac-se")).toBe("ASIA");
    expect(roomRegion(undefined, "oc")).toBe("OC");
    expect(roomRegion("", "")).toBe("");
  });

  it("read the colo out of a Cloudflare trace", () => {
    const trace = "fl=12f34\nh=cloudflare.com\nip=192.0.2.1\nts=1793570000.1\nvisit_scheme=https\nuag=\ncolo=WAW\nsliver=none\nhttp=http/1.1\nloc=PL\ntls=TLSv1.3\n";
    expect(traceColo(trace)).toBe("WAW");
    expect(traceColo("colo=\n")).toBe("");
    expect(traceColo("<html>")).toBe("");
  });

  it("order the list by the asker's region, busiest first within each part", () => {
    const rooms = [
      { code: "A", players: 6, region: "NA", createdIn: "NA" },
      { code: "B", players: 1, region: "EU", createdIn: "EU" },
      { code: "C", players: 3, region: "EU", createdIn: "NA" },
      { code: "D", players: 4, region: "EU", createdIn: "EU" },
      { code: "E", players: 2, region: "NA", createdIn: "SA" },
    ];
    expect(sortForRegion(rooms, "EU").map((x) => x.code)).toEqual(["D", "B", "C", "A", "E"]);
    expect(sortForRegion(rooms, "SA").map((x) => x.code)).toEqual(["E", "A", "D", "C", "B"]);
    expect(sortForRegion(rooms, "").map((x) => x.code)).toEqual(["A", "D", "C", "E", "B"]);
  });
});

const POLAND = { colo: "WAW", country: "PL", continent: "EU", longitude: "21.0" };
const OHIO = { colo: "ORD", country: "US", continent: "NA", regionCode: "OH", longitude: "-83.0" };

describe("rooms and their region", () => {
  it("tell a player where they connect and where the room runs; members show their country", async () => {
    const host = await Client.connect("/v1/rooms/new", POLAND);
    host.hello("Pole", { features: ["session", "countdown"] });
    const welcome = await host.json("welcome");
    expect(welcome).toMatchObject({ edgeColo: "WAW", country: "PL", continent: "EU", roomColo: "", roomRegion: "EU" });
    const guest = await Client.connect(`/v1/rooms/${welcome.code}`, OHIO);
    guest.hello("Yank");
    expect(await guest.json("welcome")).toMatchObject({ edgeColo: "ORD", country: "US", continent: "NA", roomRegion: "EU" });
    const where = JSON.parse(await host.next((f) => typeof f === "string" && JSON.parse(f).type === "where" && JSON.parse(f).members.length === 2) as string);
    expect(where).toEqual({ type: "where", region: "EU", colo: "", members: [{ id: 0, country: "PL" }, { id: 1, country: "US" }] });
    // the hint and the creator are kept with the room
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(welcome.code)), async (r: any) => {
      expect(r.state.origin).toEqual({ hint: "eeur", createdIn: "EU", createdFrom: "WAW" });
    });
  });

  it("show the colo the room learns from its trace, and its region from it", async () => {
    const host = await Client.connect("/v1/rooms/new", OHIO);
    host.hello("Host");
    const { code } = await host.json("welcome");
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (r: any) => {
      r.state.colo = "FRA";  // as if Cloudflare had put it in Frankfurt despite the hint
    });
    host.sendJson({ type: "settings", public: true });
    const guest = await Client.connect(`/v1/rooms/${code}`, POLAND);
    guest.hello("Guest");
    expect(await guest.json("welcome")).toMatchObject({ roomColo: "FRA", roomRegion: "EU" });
    const where = JSON.parse(await host.next((f) => typeof f === "string" && JSON.parse(f).type === "where" && JSON.parse(f).members.length === 2) as string);
    expect(where).toMatchObject({ region: "EU", colo: "FRA" });
    const list = (await (await exports.default.fetch("https://dhd.test/v1/lobbies")).json<any>()).lobbies;
    expect(list.find((l: any) => l.code === code)).toMatchObject({ region: "EU", colo: "FRA", createdIn: "NA" });
  });

  it("list each room's region, the asker's region's rooms first", async () => {
    const eu = await Client.connect("/v1/rooms/new", POLAND);
    eu.hello("Euro");
    const euCode = (await eu.json("welcome")).code;
    const na = await Client.connect("/v1/rooms/new", OHIO);
    na.hello("Amer");
    const naCode = (await na.json("welcome")).code;
    eu.sendJson({ type: "settings", public: true });
    na.sendJson({ type: "settings", public: true });
    // the busier room is the American one, so only the region puts the European one first
    await Client.join(naCode, "Second");
    const list = async (cf: Record<string, unknown>) => {
      const r = await (await exports.default.fetch("https://dhd.test/v1/lobbies", { cf })).json<any>();
      return { region: r.region, codes: r.lobbies.map((l: any) => l.code).filter((c: string) => c === euCode || c === naCode), rows: r.lobbies };
    };
    await expect.poll(async () => (await list(POLAND)).codes.length).toBe(2);
    const fromPoland = await list(POLAND);
    expect(fromPoland.region).toBe("EU");
    expect(fromPoland.codes).toEqual([euCode, naCode]);
    expect(fromPoland.rows.find((l: any) => l.code === euCode)).toMatchObject({ region: "EU", createdIn: "EU" });
    expect(fromPoland.rows.find((l: any) => l.code === naCode)).toMatchObject({ region: "NA", createdIn: "NA" });
    const fromOhio = await list(OHIO);
    expect(fromOhio.region).toBe("NA");
    expect(fromOhio.codes).toEqual([naCode, euCode]);
  });
});
