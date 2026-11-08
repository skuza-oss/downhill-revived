// The staged rollout's verdict (tools/deploy.mjs, finding 184): the canary version's Workers Logs
// against the previous version's, over the same minutes. Pure: no I/O, tested by
// test/deploy-verdict.test.ts.

/** The limits deploy.mjs uses unless its flags say otherwise. */
export const LIMITS = {
  /** At least this many of a kind before the canary can fail on it (a single error is noise). */
  minCount: 3,
  /** The canary fails when its rate is above this many times the previous version's... */
  factor: 2,
  /** ...plus this much (per 1000 log lines for errors and exceptions, per room for racers out). */
  slackPer1000: 1,
  slackPerRoom: 0.5,
  /** Fewer canary log lines than this: too little traffic to tell ("thin"). */
  minEvents: 20,
};

/**
 * One version's numbers over the watch: `events` log lines, `objects` Durable Objects that logged
 * (rooms and the lobby list), `errors` error-level lines (deploy.mjs leaves out a restart's own
 * abort line), `exceptions` invocations that ended in an exception, `outOfRace` "out of the race"
 * lines (a racer taken out by Realtime), `restarts` rooms restarting for new code (each ends its
 * invocation with ctx.abort by design: not counted as an exception).
 */
export function emptyMetrics() {
  return { events: 0, objects: 0, errors: 0, exceptions: 0, outOfRace: 0, restarts: 0 };
}

const rate = (n, of, per) => (of > 0 ? (n * per) / of : 0);

/**
 * "rollback" when the canary is worse than the previous version on errors, exceptions or racers
 * out of the race; "thin" when it saw too little traffic to say; else "promote". `reasons` says why,
 * one line each, for the summary.
 */
export function verdict(canary, previous, limits = LIMITS) {
  const reasons = [];
  const errorsC = canary.errors, errorsP = previous.errors;
  const excC = Math.max(0, canary.exceptions - canary.restarts), excP = Math.max(0, previous.exceptions - previous.restarts);
  const per1000 = (label, c, p) => {
    if (c < limits.minCount) return;
    const rc = rate(c, canary.events, 1000), rp = rate(p, previous.events, 1000);
    if (rc > limits.factor * rp + limits.slackPer1000)
      reasons.push(`${label}: ${c} (${rc.toFixed(1)} per 1000 lines) against ${p} (${rp.toFixed(1)}) on the previous version`);
  };
  per1000("errors", errorsC, errorsP);
  per1000("exceptions", excC, excP);
  if (canary.outOfRace >= limits.minCount) {
    const rc = rate(canary.outOfRace, Math.max(1, canary.objects), 1), rp = rate(previous.outOfRace, Math.max(1, previous.objects), 1);
    if (rc > limits.factor * rp + limits.slackPerRoom)
      reasons.push(`racers out of the race: ${canary.outOfRace} (${rc.toFixed(2)} per room) against ${previous.outOfRace} (${rp.toFixed(2)})`);
  }
  if (reasons.length) return { verdict: "rollback", reasons };
  if (canary.events < limits.minEvents)
    return { verdict: "thin", reasons: [`only ${canary.events} log lines on the new version (${canary.objects} objects): too little traffic to compare`] };
  return { verdict: "promote", reasons: [] };
}

/** The deployment's split as `wrangler versions deploy` takes it. */
export function splitSpecs(next, previous, percent) {
  if (percent >= 100 || !previous || previous === next) return [`${next}@100%`];
  return [`${next}@${percent}%`, `${previous}@${100 - percent}%`];
}

/** The SQL that sets the rollout row (src/rollout.ts): a version id, or "" while a gradual
    deployment runs. wrangler d1 execute takes no parameters: the value is checked here. */
export function rolloutSql(value, now) {
  if (value !== "" && !/^[0-9a-f-]{8,64}$/i.test(value)) throw new Error(`not a version id: ${value}`);
  if (!Number.isSafeInteger(now)) throw new Error("bad time");
  return `INSERT INTO settings (key, value, updated_at) VALUES ('rollout', '${value}', ${now}) ` +
    `ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at`;
}
