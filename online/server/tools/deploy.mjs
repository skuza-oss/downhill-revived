#!/usr/bin/env node
// Deploys the online server in stages (finding 184): the checks, a new version, about 10% of the
// Durable Objects on it while Workers Logs are compared with the previous version's, then 100% or
// a rollback on its own, and a summary. Rooms adopt the new code only at a safe moment (between
// races: src/rollout.ts); this tool tells them which version is deployed through the rollout row in
// the moderation store (settings key `rollout`).
//
//   node tools/deploy.mjs                         the staged rollout (10% for 10 minutes, then 100%)
//   node tools/deploy.mjs --canary 20 --watch 15  another share and watch
//   node tools/deploy.mjs --emergency             100% at once with immediate updates: every busy
//                                                 room restarts now, races included (urgent fixes)
//   node tools/deploy.mjs --dry-run               says what it would do; changes nothing
//
// Other flags: --config FILE (another wrangler config: a test Worker), --message TEXT,
// --poll SECONDS (60), --require-traffic (a canary with too little traffic rolls back instead of
// going on), --no-checks (skip check.py server: test Workers only), --settle SECONDS (20: the wait
// between a deployment and the rollout row, so the deployment has reached the rooms).
//
// Needs: the operator's wrangler login (`npx wrangler login`), CLOUDFLARE_ACCOUNT_ID in the
// environment, and a read-only Workers Logs token (Workers Observability) in a file named by
// DHD_LOGS_TOKEN_FILE or by `logsTokenFile` in online/server/.deploy.json (gitignored). The token
// is read, never printed. --emergency needs no token (it watches nothing).
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { stripJsonc } from "./moderate.mjs";
import { LIMITS, emptyMetrics, rolloutSql, splitSpecs, verdict } from "./rollout-verdict.mjs";

const SERVER = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const ROOT = path.dirname(path.dirname(SERVER));
const WRANGLER = path.join(SERVER, "node_modules", "wrangler", "bin", "wrangler.js");
const LOGS_API = (account) => `https://api.cloudflare.com/client/v4/accounts/${account}/workers/observability/telemetry/query`;

function fail(message) {
  console.error(`deploy: ${message}`);
  process.exit(2);
}
const say = (line) => console.log(`[${new Date().toISOString().slice(11, 19)}] ${line}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- arguments and setup ----

function parseArgs(argv) {
  const flags = { canary: 10, watch: 10, poll: 60, settle: 20 };
  const numeric = new Set(["canary", "watch", "poll", "settle"]);
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) fail(`unknown argument ${a} (see the top of tools/deploy.mjs)`);
    const key = a.slice(2);
    if (["emergency", "dry-run", "require-traffic", "no-checks", "help"].includes(key)) flags[key] = true;
    else if (["config", "message", ...numeric].includes(key)) {
      const v = argv[++i];
      if (v === undefined) fail(`${a} needs a value`);
      flags[key] = numeric.has(key) ? Number(v) : v;
      if (numeric.has(key) && !(Number.isFinite(flags[key]) && flags[key] >= 0)) fail(`${a} takes a number`);
    } else fail(`unknown flag ${a}`);
  }
  if (!(flags.canary > 0 && flags.canary < 100)) fail("--canary takes a share between 1 and 99");
  return flags;
}

function readConfig(file) {
  const config = JSON.parse(stripJsonc(readFileSync(path.resolve(SERVER, file), "utf8")));
  const db = (config.d1_databases ?? []).find((d) => d.binding === "MODERATION")?.database_name ?? null;
  const maxDelay = config.durable_objects?.code_update_strategy?.max_delay ?? 86400;
  if (!config.name) fail(`${file} names no Worker`);
  return { name: config.name, db, maxDelay };
}

function logsToken() {
  let file = process.env.DHD_LOGS_TOKEN_FILE;
  const local = path.join(SERVER, ".deploy.json");
  if (!file && existsSync(local)) file = JSON.parse(readFileSync(local, "utf8")).logsTokenFile;
  if (!file) fail("no Workers Logs token: set DHD_LOGS_TOKEN_FILE or logsTokenFile in online/server/.deploy.json");
  if (!existsSync(file)) fail("the Workers Logs token file named by DHD_LOGS_TOKEN_FILE or .deploy.json does not exist");
  const token = readFileSync(file, "utf8").trim();
  if (!token) fail("the Workers Logs token file is empty");
  return token;
}

// ---- wrangler ----

/** Runs wrangler with the config; returns stdout and its structured output entries. */
function wrangler(cfg, args, { quiet = false } = {}) {
  const dir = mkdtempSync(path.join(os.tmpdir(), "dhd-deploy-"));
  const outFile = path.join(dir, "out.ndjson");
  try {
    const run = spawnSync(process.execPath, [WRANGLER, ...args, "--config", cfg.file], {
      cwd: SERVER, encoding: "utf8", maxBuffer: 64 << 20,
      env: { ...process.env, WRANGLER_OUTPUT_FILE_PATH: outFile, WRANGLER_SEND_METRICS: "false" },
    });
    if (run.status !== 0) {
      process.stderr.write(run.stdout ?? "");
      process.stderr.write(run.stderr ?? "");
      fail(`wrangler ${args.slice(0, 2).join(" ")} failed (exit ${run.status})`);
    }
    if (!quiet) process.stdout.write((run.stdout ?? "").split("\n").filter((l) => l.trim()).map((l) => `    ${l}\n`).join(""));
    const entries = existsSync(outFile)
      ? readFileSync(outFile, "utf8").split("\n").filter(Boolean).map((l) => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean)
      : [];
    return { stdout: run.stdout ?? "", entries };
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

/** The versions deployed now and their shares. */
function currentDeployment(cfg) {
  const { stdout } = wrangler(cfg, ["deployments", "status", "--json"], { quiet: true });
  const at = stdout.indexOf("{");
  if (at < 0) fail("wrangler deployments status printed no JSON");
  const d = JSON.parse(stdout.slice(at));
  return (d.versions ?? []).map((v) => ({ id: v.version_id, percent: Number(v.percentage) }));
}

function upload(cfg, message) {
  const { entries, stdout } = wrangler(cfg, ["versions", "upload", "--message", message]);
  const id = entries.find((e) => e.type === "version-upload")?.version_id ?? stdout.match(/Worker Version ID:\s*([0-9a-f-]{36})/i)?.[1];
  if (!id) fail("could not read the uploaded version's id");
  return id;
}

function deploy(cfg, specs, mode, message) {
  wrangler(cfg, ["versions", "deploy", ...specs, "--yes", "--message", message, "--durable-objects-code-update-mode", mode]);
}

function setRollout(cfg, value) {
  if (!cfg.db) return;
  wrangler(cfg, ["d1", "execute", cfg.db, "--remote", "--command", rolloutSql(value, Date.now())], { quiet: true });
  say(value ? `rollout row: ${value} (rooms on another version restart at their next safe moment)` : "rollout row: hold (no room restarts during the gradual deployment)");
}

// ---- Workers Logs ----

async function query(cfg, from, to, filters, calculations) {
  const body = {
    queryId: "dhd-deploy", timeframe: { from, to }, view: "calculations", limit: 100,
    parameters: {
      filters: [{ key: "$metadata.service", operation: "eq", type: "string", value: cfg.name }, ...filters],
      calculations, groupBys: [{ type: "string", value: "$workers.scriptVersion.id" }],
    },
  };
  for (let attempt = 0; ; attempt++) {
    try {
      const r = await fetch(LOGS_API(cfg.account), {
        method: "POST", headers: { Authorization: `Bearer ${cfg.token}`, "Content-Type": "application/json" }, body: JSON.stringify(body),
      });
      const d = await r.json();
      if (!d.success) throw new Error(JSON.stringify(d.errors ?? r.status).slice(0, 200));
      const out = new Map();
      for (const calc of d.result?.calculations ?? [])
        for (const a of calc.aggregates ?? []) {
          const version = a.groups?.find((g) => g.key === "$workers.scriptVersion.id")?.value;
          if (!version) continue;
          out.set(version, { ...(out.get(version) ?? {}), [calc.alias]: Number(a.value) || 0 });
        }
      return out;
    } catch (e) {
      if (attempt >= 2) throw e;
      await sleep(5000);
    }
  }
}

const has = (text) => ({ key: "$metadata.message", operation: "includes", type: "string", value: text });

/** Each version's numbers (tools/rollout-verdict.mjs emptyMetrics) from `from` to `to`. */
async function metrics(cfg, from, to) {
  const [all, errors, exceptions, out, restarts] = await Promise.all([
    query(cfg, from, to, [], [{ operator: "count", alias: "events" }, { operator: "uniq", key: "$workers.durableObjectId", alias: "objects" }]),
    query(cfg, from, to, [{ key: "$metadata.level", operation: "eq", type: "string", value: "error" },
      { key: "$metadata.message", operation: "not_includes", type: "string", value: "code update at a safe moment" }],
      [{ operator: "count", alias: "errors" }]),
    query(cfg, from, to, [{ key: "$workers.outcome", operation: "eq", type: "string", value: "exception" }],
      [{ operator: "uniq", key: "$metadata.requestId", alias: "exceptions" }]),
    query(cfg, from, to, [has("out of the race")], [{ operator: "count", alias: "outOfRace" }]),
    query(cfg, from, to, [has("restarting for new code")], [{ operator: "count", alias: "restarts" }]),
  ]);
  const versions = new Set([...all.keys(), ...errors.keys(), ...exceptions.keys(), ...out.keys(), ...restarts.keys()]);
  const result = new Map();
  for (const v of versions)
    result.set(v, { ...emptyMetrics(), ...all.get(v), ...errors.get(v), ...exceptions.get(v), ...out.get(v), ...restarts.get(v) });
  return result;
}

const show = (m) => `${m.events} lines, ${m.objects} objects, ${m.errors} errors, ${m.exceptions} exceptions, ` +
  `${m.outOfRace} out of the race, ${m.restarts} room restarts`;

// ---- the rollout ----

async function main(argv) {
  const flags = parseArgs(argv);
  if (flags.help) {
    console.log(readFileSync(fileURLToPath(import.meta.url), "utf8").split("\nimport ")[0].replace(/^#!.*\n/, "").replace(/^\/\/ ?/gm, ""));
    return 0;
  }
  if (!existsSync(WRANGLER)) fail("wrangler is not installed: run npm install in online/server");
  const account = process.env.CLOUDFLARE_ACCOUNT_ID;
  if (!account || !/^[0-9a-f]{32}$/.test(account)) fail("set CLOUDFLARE_ACCOUNT_ID to the account that runs the server");
  const file = flags.config ?? "wrangler.jsonc";
  const cfg = { file, account, ...readConfig(file), token: flags.emergency ? "" : logsToken() };
  const message = flags.message ?? `deploy.mjs ${new Date().toISOString().slice(0, 16)}Z`;
  const deferred = `deferred ${cfg.maxDelay}s`;
  const started = Date.now();

  const current = currentDeployment(cfg);
  const previous = current.length === 1 && current[0].percent === 100 ? current[0].id : null;
  say(`${cfg.name}: deployed now ${current.map((v) => `${v.id}@${v.percent}%`).join(", ") || "nothing"}`);
  if (!previous && current.length && !flags.emergency)
    fail("a gradual deployment is still split: finish it (wrangler versions deploy <id>@100%) or use --emergency");
  if (!cfg.db && !flags.emergency)
    fail(`${file} has no MODERATION database: without the rollout row a gradual deployment would restart rooms for nothing; use --emergency, or wrangler deploy (rooms then follow the newer Worker version)`);
  const plan = flags.emergency
    ? `upload, then 100% with immediate updates (every busy room restarts now, races included)`
    : `upload, then ${flags.canary}% of objects (deferred ${cfg.maxDelay} s) for ${flags.watch} min against ${previous}, then 100% or back`;
  if (flags["dry-run"]) {
    say(`dry run: ${flags["no-checks"] ? "" : "check.py server, "}${plan}`);
    return 0;
  }

  if (!flags["no-checks"]) {
    say("python tools/check.py server");
    const run = spawnSync(process.env.PYTHON ?? "python", ["tools/check.py", "server"], { cwd: ROOT, stdio: "inherit" });
    if (run.status !== 0) fail("check.py server failed: nothing deployed");
  }
  const next = upload(cfg, message);
  say(`uploaded ${next}`);

  if (flags.emergency) {
    deploy(cfg, [`${next}@100%`], "immediate", `${message} (emergency)`);
    say(`deployed ${next} at 100% with immediate updates`);
    await sleep(flags.settle * 1000);
    setRollout(cfg, next);
    summary({ cfg, previous, next, started, outcome: "emergency: 100% at once", rows: null });
    return 0;
  }

  // The canary. The row first: no room restarts while the deployment is split.
  setRollout(cfg, "");
  let finished = false;
  const back = async (why) => {
    deploy(cfg, [`${previous}@100%`], deferred, `${message} (rolled back)`);
    say(`rolled back to ${previous} (${why})`);
    await sleep(flags.settle * 1000);
    setRollout(cfg, previous);
  };
  process.on("SIGINT", () => {
    if (finished) process.exit(130);
    finished = true;
    say("interrupted: rolling back");
    back("interrupted").then(() => process.exit(130));
  });
  deploy(cfg, splitSpecs(next, previous, flags.canary), deferred, `${message} (canary ${flags.canary}%)`);
  const since = Date.now();
  say(`canary: ${next}@${flags.canary}%, ${previous}@${100 - flags.canary}%; watching Workers Logs for ${flags.watch} min`);

  let rows = null, decision = null;
  const until = since + flags.watch * 60_000;
  while (!finished) {
    await sleep(Math.min(flags.poll * 1000, Math.max(1000, until - Date.now())));
    try {
      rows = await metrics(cfg, since, Date.now());
    } catch (e) {
      say(`Workers Logs query failed: ${String(e.message ?? e).slice(0, 120)}`);
      if (Date.now() < until) continue;
      decision = { verdict: "rollback", reasons: ["Workers Logs could not be read: nothing to compare"] };
      break;
    }
    const c = rows.get(next) ?? emptyMetrics(), p = rows.get(previous) ?? emptyMetrics();
    say(`new ${show(c)} | previous ${show(p)}`);
    decision = verdict(c, p, LIMITS);
    if (decision.verdict === "rollback" || Date.now() >= until) break;
  }
  if (finished) return 130;
  finished = true;

  if (decision.verdict === "thin" && flags["require-traffic"]) decision = { verdict: "rollback", reasons: decision.reasons };
  if (decision.verdict === "rollback") {
    for (const r of decision.reasons) say(`canary failed: ${r}`);
    await back(decision.reasons[0]);
    summary({ cfg, previous, next, started, outcome: `rolled back to ${previous}`, rows, decision });
    return 1;
  }
  for (const r of decision.reasons) say(`note: ${r}`);
  deploy(cfg, [`${next}@100%`], deferred, `${message} (100%)`);
  say(`${next} at 100%`);
  await sleep(flags.settle * 1000);
  setRollout(cfg, next);
  summary({ cfg, previous, next, started, outcome: `promoted ${next} to 100%`, rows, decision });
  return 0;
}

function summary({ cfg, previous, next, started, outcome, rows, decision }) {
  console.log("\nSummary");
  console.log(`  Worker:    ${cfg.name}`);
  console.log(`  Previous:  ${previous ?? "(none)"}`);
  console.log(`  New:       ${next}`);
  console.log(`  Outcome:   ${outcome} after ${Math.round((Date.now() - started) / 1000)} s`);
  if (rows) {
    console.log(`  New version over the canary:      ${show(rows.get(next) ?? emptyMetrics())}`);
    console.log(`  Previous version over the canary: ${show(rows.get(previous) ?? emptyMetrics())}`);
  }
  if (decision?.reasons?.length) for (const r of decision.reasons) console.log(`  Why: ${r}`);
  console.log("  Rooms: each busy room moves at its next safe moment (between races; its players see");
  console.log("  \"Server updated, reconnecting...\" for about a second and keep their seats). Follow it");
  console.log("  in Workers Logs: \"restarting for new code\", \"all N members back\".");
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  main(process.argv.slice(2)).then((code) => process.exit(code), (e) => fail(String(e?.stack ?? e)));
