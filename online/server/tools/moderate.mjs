#!/usr/bin/env node
// The operator's moderation tool (finding 161): reads the moderation log and changes the ban list
// in the server's D1 database through `wrangler d1 execute`, under the operator's own Cloudflare
// login (`npx wrangler login`). The server has no admin endpoint: whoever can run this with the
// account's login can moderate, nobody else.
//
//   node tools/moderate.mjs joins [--name TEXT] [--since WHEN] [--until WHEN] [--room CODE]
//                                 [--like JOIN_ID] [--limit N] [--hashes]
//   node tools/moderate.mjs ban JOIN_ID [--ip-only | --install-only]
//                                 [--days N | --hours N | --until WHEN | --forever] [--reason TEXT]
//   node tools/moderate.mjs bans [--all] [--hashes]
//   node tools/moderate.mjs unban BAN_ID...
//   node tools/moderate.mjs online [on | off [--message TEXT]]
//   node tools/moderate.mjs local-setup      the local store for `npm run dev:guard` (below)
//
// joins lists who entered rooms, newest first: time (UTC), room, member, name, release, country,
// whether the row has an address hash and an install id, and whether a ban covers it. --name
// matches part of a name, without case; --like JOIN_ID lists every row with that row's address
// hash or install id (the same player under other names). WHEN is a date (2026-11-05, UTC), a
// date and time (2026-11-05T18:30), or an age (90m, 6h, 2d). Hashes and install ids are printed
// only with --hashes.
//
// ban bans the address hash and the install id of one join row (by default both; a row from a
// release without an install id has only the address), 30 days unless said otherwise. An address
// can be many players' (a household, carrier-grade NAT): ban says which other names the log has
// from it; --install-only spares them, --ip-only catches a player who resets the install id. Banning
// again changes the expiry. bans lists the bans in force (--all: ended ones too, until the daily
// cleanup removes them); unban lifts bans by their id.
//
// online is the kill switch, no deploy needed: `online off` refuses new rooms, joins and race
// starts (races running finish, rooms' members stay and may rejoin), with --message TEXT on the
// game's ONLINE screen instead of the default (keep it within about 45 characters, the screen's
// footer; at most 120); `online on` lets players in again; `online` alone shows the state. The
// servers keep what they read for up to 30 s, and a ban lookup up to a minute: a change bites
// within that long.
//
// Where: the deployed database by default (--remote); --local is the store of a local
// `npm run dev:guard`. --db NAME (default dhd-moderation) and --config FILE pick another.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const SERVER = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const WRANGLER = path.join(SERVER, "node_modules", "wrangler", "bin", "wrangler.js");
/** The local config `local-setup` writes (gitignored): wrangler.jsonc plus the MODERATION binding. */
export const LOCAL_CONFIG = "wrangler.guard-local.jsonc";
const DEFAULT_DB = "dhd-moderation";
const DEFAULT_BAN_DAYS = 30;

function fail(message) {
  console.error(`moderate: ${message}`);
  process.exit(2);
}

// ---- arguments ----

function parseArgs(argv) {
  const flags = {};
  const rest = [];
  const valued = new Set(["name", "since", "until", "room", "like", "limit", "days", "hours", "reason", "db", "config", "message"]);
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) {
      rest.push(a);
      continue;
    }
    const key = a.slice(2);
    if (valued.has(key)) {
      if (i + 1 >= argv.length) fail(`${a} needs a value`);
      flags[key] = argv[++i];
    } else flags[key] = true;
  }
  return { flags, rest };
}

/** WHEN as ms since the Unix epoch: a UTC date, a UTC date and time, or an age before now. */
export function parseWhen(text, now = Date.now()) {
  const age = /^(\d+)(m|h|d)$/.exec(text);
  if (age) return now - Number(age[1]) * { m: 60_000, h: 3_600_000, d: 86_400_000 }[age[2]];
  if (/^\d{4}-\d{2}-\d{2}(T\d{2}:\d{2}(:\d{2})?)?$/.test(text)) {
    const t = Date.parse(text.length === 10 ? `${text}T00:00:00Z` : `${text}Z`);
    if (Number.isFinite(t)) return t;
  }
  fail(`"${text}" is not a date (2026-11-05), a date and time (2026-11-05T18:30) or an age (90m, 6h, 2d)`);
}

function positiveInt(text, what) {
  if (!/^\d{1,15}$/.test(String(text)) || Number(text) < 1) fail(`${what} must be a whole number above 0`);
  return Number(text);
}

// ---- SQL (wrangler d1 execute takes no parameters: every value is checked or quoted here) ----

/** A SQL string literal. */
export function sqlString(text) {
  if (/\u0000/.test(text)) fail("text with a NUL character");
  return `'${String(text).replaceAll("'", "''")}'`;
}

/** A LIKE pattern for `part` anywhere in the text (ESCAPE '\'). */
export function sqlContains(part) {
  return sqlString(`%${part.replace(/[\\%_]/g, (c) => `\\${c}`)}%`);
}

const utc = (col) => `strftime('%Y-%m-%d %H:%M', ${col} / 1000, 'unixepoch')`;
const activeBan = (now) =>
  `EXISTS (SELECT 1 FROM bans b WHERE ((b.kind = 'ip' AND b.value = j.ip_hash) OR (b.kind = 'install' AND b.value = j.install_id))` +
  ` AND (b.expires_at IS NULL OR b.expires_at > ${now}))`;

export function joinsSql(flags, now = Date.now()) {
  const where = [];
  if (flags.name !== undefined) where.push(`j.name LIKE ${sqlContains(String(flags.name))} ESCAPE '\\'`);
  if (flags.since !== undefined) where.push(`j.at >= ${parseWhen(String(flags.since), now)}`);
  if (flags.until !== undefined) where.push(`j.at < ${parseWhen(String(flags.until), now)}`);
  if (flags.room !== undefined) {
    if (!/^[A-Za-z0-9]{1,12}$/.test(String(flags.room))) fail("--room takes a join code");
    where.push(`j.room = ${sqlString(String(flags.room).toUpperCase())}`);
  }
  if (flags.like !== undefined) {
    const id = positiveInt(flags.like, "--like");
    where.push(
      `EXISTS (SELECT 1 FROM joins o WHERE o.id = ${id} AND ((o.ip_hash <> '' AND o.ip_hash = j.ip_hash) OR (o.install_id IS NOT NULL AND o.install_id = j.install_id)))`,
    );
  }
  const limit = flags.limit !== undefined ? positiveInt(flags.limit, "--limit") : 50;
  const hashes = flags.hashes ? ", j.ip_hash AS ip_hash, j.install_id AS install_id" : "";
  return (
    `SELECT j.id, ${utc("j.at")} AS time_utc, j.room, j.member, j.kind, j.name, j.client_version AS version, j.country,` +
    ` CASE WHEN j.ip_hash <> '' THEN 'yes' ELSE '' END AS addr, CASE WHEN j.install_id IS NOT NULL THEN 'yes' ELSE '' END AS install,` +
    ` CASE WHEN ${activeBan(now)} THEN 'BANNED' ELSE '' END AS ban${hashes}` +
    ` FROM joins j${where.length ? ` WHERE ${where.join(" AND ")}` : ""} ORDER BY j.at DESC, j.id DESC LIMIT ${limit}`
  );
}

export function banSql(joinId, flags, now = Date.now()) {
  const id = positiveInt(joinId, "the join row's id");
  let expires;
  if (flags.forever) expires = "NULL";
  else if (flags.until !== undefined) expires = String(parseWhen(String(flags.until), now));
  else if (flags.hours !== undefined) expires = String(now + positiveInt(flags.hours, "--hours") * 3_600_000);
  else expires = String(now + (flags.days !== undefined ? positiveInt(flags.days, "--days") : DEFAULT_BAN_DAYS) * 86_400_000);
  if (expires !== "NULL" && Number(expires) <= now) fail("the ban would have ended already");
  const reason = sqlString(String(flags.reason ?? "").slice(0, 200));
  const insert = (kind, column, present) =>
    `INSERT INTO bans (kind, value, created_at, expires_at, reason, name)` +
    ` SELECT '${kind}', ${column}, ${now}, ${expires}, ${reason}, name FROM joins WHERE id = ${id} AND ${present}` +
    ` ON CONFLICT (kind, value) DO UPDATE SET created_at = excluded.created_at, expires_at = excluded.expires_at,` +
    ` reason = excluded.reason, name = excluded.name`;
  const parts = [];
  if (!flags["install-only"]) parts.push(insert("ip", "ip_hash", "ip_hash <> ''"));
  if (!flags["ip-only"]) parts.push(insert("install", "install_id", "install_id IS NOT NULL"));
  if (!parts.length) fail("--ip-only and --install-only together ban nothing");
  return parts;
}

export function bansSql(flags, now = Date.now()) {
  const value = flags.hashes ? ", value" : "";
  return (
    `SELECT id, kind, ${utc("created_at")} AS since_utc,` +
    ` CASE WHEN expires_at IS NULL THEN 'until lifted' ELSE ${utc("expires_at")} END AS until_utc,` +
    ` CASE WHEN expires_at IS NOT NULL AND expires_at <= ${now} THEN 'ended' ELSE '' END AS ended, name, reason${value}` +
    ` FROM bans${flags.all ? "" : ` WHERE expires_at IS NULL OR expires_at > ${now}`} ORDER BY id`
  );
}

/** The footer of the game's ONLINE screen holds about this many characters (src/guard.ts
    OFF_MESSAGE); the server shows at most MESSAGE_MAX. */
const FOOTER_CHARS = 45;
const MESSAGE_MAX = 120;

/** The kill switch's rows (migrations/0002_settings.sql): `online` and its message ('' when off
    without --message, so an old message never comes back; the server's default then). */
export function onlineSql(state, flags, now = Date.now()) {
  const message = state === "off" ? String(flags.message ?? "").trim() : null;
  if (message !== null && message.length > MESSAGE_MAX) fail(`--message holds at most ${MESSAGE_MAX} characters`);
  const rows = [["online", state]];
  if (message !== null) rows.push(["online_message", message]);
  return rows.map(
    ([key, value]) =>
      `INSERT INTO settings (key, value, updated_at) VALUES (${sqlString(key)}, ${sqlString(value)}, ${now})` +
      ` ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at`,
  );
}

export const ONLINE_SHOW_SQL =
  `SELECT key, value, ${utc("updated_at")} AS since_utc FROM settings WHERE key IN ('online', 'online_message') ORDER BY key`;

// ---- wrangler ----

function wranglerArgs(flags) {
  if (!existsSync(WRANGLER)) fail("wrangler is not installed: run npm install in online/server");
  const db = String(flags.db ?? DEFAULT_DB);
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(db)) fail("--db takes a database name");
  const config = flags.config ?? (flags.local ? LOCAL_CONFIG : undefined);
  if (flags.local && !flags.config && !existsSync(path.join(SERVER, LOCAL_CONFIG)))
    fail(`no local store yet: run "node tools/moderate.mjs local-setup" (or npm run dev:guard) first`);
  return { db, where: flags.local ? "--local" : "--remote", config: config ? ["--config", String(config)] : [] };
}

/** Runs SQL statements (joined with ;) and returns each one's result rows. */
function execute(flags, statements) {
  const { db, where, config } = wranglerArgs(flags);
  const run = spawnSync(
    process.execPath,
    [WRANGLER, "d1", "execute", db, where, "--json", "--command", statements.join(";\n"), ...config],
    { cwd: SERVER, encoding: "utf8", maxBuffer: 64 << 20 },
  );
  const out = run.stdout ?? "";
  const at = out.indexOf("[");
  if (run.status !== 0 || at < 0) {
    process.stderr.write(run.stderr ?? "");
    process.stderr.write(out);
    fail(`wrangler d1 execute failed (exit ${run.status})`);
  }
  return JSON.parse(out.slice(at)).map((r) => r.results ?? []);
}

function printTable(rows) {
  if (!rows.length) {
    console.log("(none)");
    return;
  }
  const cols = Object.keys(rows[0]);
  const text = (v) => (v === null || v === undefined ? "" : String(v));
  const width = cols.map((c) => Math.max(c.length, ...rows.map((r) => text(r[c]).length)));
  const line = (vals) => vals.map((v, i) => v.padEnd(width[i])).join("  ").trimEnd();
  console.log(line(cols));
  console.log(line(width.map((w) => "-".repeat(w))));
  for (const r of rows) console.log(line(cols.map((c) => text(r[c]))));
}

// ---- the local store ----

/** JSONC to JSON: drops // and /* *\/ comments outside strings, and trailing commas. */
export function stripJsonc(text) {
  let out = "";
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"') {
      let j = i + 1;
      while (j < text.length && text[j] !== '"') j += text[j] === "\\" ? 2 : 1;
      out += text.slice(i, j + 1);
      i = j;
    } else if (c === "/" && text[i + 1] === "/") {
      while (i < text.length && text[i] !== "\n") i++;
      out += "\n";
    } else if (c === "/" && text[i + 1] === "*") {
      i = text.indexOf("*/", i + 2) + 1;
      if (i === 0) break;
    } else out += c;
  }
  return out.replace(/,(\s*[}\]])/g, "$1");
}

/** wrangler.guard-local.jsonc: wrangler.jsonc with the MODERATION binding to a local database, no
    custom domain; then the migrations into that local store. */
function localSetup() {
  const config = JSON.parse(stripJsonc(readFileSync(path.join(SERVER, "wrangler.jsonc"), "utf8")));
  delete config.routes;
  config.d1_databases = [{ binding: "MODERATION", database_name: DEFAULT_DB, database_id: "local-moderation", migrations_dir: "migrations" }];
  writeFileSync(
    path.join(SERVER, LOCAL_CONFIG),
    `// Written by tools/moderate.mjs local-setup for a local test server: wrangler.jsonc with the\n` +
      `// moderation store bound to a local database. Not for deploying.\n${JSON.stringify(config, null, 2)}\n`,
  );
  const run = spawnSync(process.execPath, [WRANGLER, "d1", "migrations", "apply", DEFAULT_DB, "--local", "--config", LOCAL_CONFIG], {
    cwd: SERVER,
    encoding: "utf8",
    env: { ...process.env, CI: "1" },
  });
  if (run.status !== 0) {
    process.stderr.write(run.stdout + run.stderr);
    fail("wrangler d1 migrations apply failed");
  }
  console.log(`local store ready (${LOCAL_CONFIG}, .wrangler/state); start it with: npx wrangler dev --config ${LOCAL_CONFIG}`);
}

// ---- commands ----

function main(argv) {
  const [command, ...more] = argv;
  const { flags, rest } = parseArgs(more);
  switch (command) {
    case "joins":
      printTable(execute(flags, [joinsSql(flags)])[0]);
      return;
    case "ban": {
      if (rest.length !== 1) fail("ban takes one join row id (the first column of joins)");
      const results = execute(flags, [
        ...banSql(rest[0], flags),
        `SELECT b.id, b.kind, CASE WHEN b.expires_at IS NULL THEN 'until lifted' ELSE ${utc("b.expires_at")} END AS until_utc, b.name, b.reason` +
          ` FROM bans b JOIN joins j ON j.id = ${positiveInt(rest[0], "the join row's id")}` +
          ` AND ((b.kind = 'ip' AND b.value = j.ip_hash) OR (b.kind = 'install' AND b.value = j.install_id))` +
          (flags["ip-only"] ? " AND b.kind = 'ip'" : flags["install-only"] ? " AND b.kind = 'install'" : ""),
      ]);
      const now = results[results.length - 1];
      if (!now.length) fail(`join row ${rest[0]} is not in the log (or has nothing to ban)`);
      console.log("Banned:");
      printTable(now);
      if (!flags["install-only"]) {
        // An address can be many players' (a household, a LAN party, carrier-grade NAT).
        const id = positiveInt(rest[0], "the join row's id");
        const [others] = execute(flags, [
          `SELECT DISTINCT o.name FROM joins o JOIN joins j ON j.id = ${id} AND j.ip_hash <> '' AND o.ip_hash = j.ip_hash` +
            ` AND (o.install_id IS NOT j.install_id OR j.install_id IS NULL) AND o.name <> j.name ORDER BY o.name LIMIT 20`,
        ]);
        if (others.length)
          console.log(`Note: the address ban also covers other names seen from that address: ${others.map((r) => r.name).join(", ")}.` +
            ` If they are other players, lift the address ban (unban) and ban with --install-only.`);
      }
      return;
    }
    case "bans":
      printTable(execute(flags, [bansSql(flags)])[0]);
      return;
    case "unban": {
      if (!rest.length) fail("unban takes ban ids (the first column of bans)");
      const ids = rest.map((id) => positiveInt(id, "a ban id"));
      const results = execute(flags, [`DELETE FROM bans WHERE id IN (${ids.join(", ")}) RETURNING id, kind, name`]);
      console.log("Lifted:");
      printTable(results[0]);
      return;
    }
    case "online": {
      if (rest.length > 1 || (rest.length && !["on", "off"].includes(rest[0]))) fail("online takes on, off or nothing");
      if (flags.message !== undefined && rest[0] !== "off") fail("--message goes with online off");
      if (rest[0] === "off" && String(flags.message ?? "").trim().length > FOOTER_CHARS)
        console.warn(`moderate: the message is longer than the ONLINE screen's footer (about ${FOOTER_CHARS} characters) and may be cut`);
      const results = execute(flags, [...(rest.length ? onlineSql(rest[0], flags) : []), ONLINE_SHOW_SQL]);
      const rows = results[results.length - 1];
      const get = (key) => rows.find((r) => r.key === key);
      const off = get("online")?.value.trim().toLowerCase() === "off";
      const message = get("online_message")?.value.trim();
      console.log(
        off
          ? `Online is OFF since ${get("online").since_utc} UTC, message: ${message ? `"${message}"` : "the default"}`
          : `Online is on${get("online") ? ` since ${get("online").since_utc} UTC` : ""}.`,
      );
      if (rest.length) console.log("The servers keep the old state for up to 30 s.");
      return;
    }
    case "local-setup":
      localSetup();
      return;
    default:
      console.log(readFileSync(fileURLToPath(import.meta.url), "utf8").split("\nimport ")[0].replace(/^#!.*\n/, "").replace(/^\/\/ ?/gm, ""));
      process.exit(command ? 2 : 0);
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main(process.argv.slice(2));
