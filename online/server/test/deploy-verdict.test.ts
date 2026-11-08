// The staged rollout's verdict (tools/rollout-verdict.mjs, finding 184).
import { describe, expect, it } from "vitest";
// @ts-expect-error: a plain ES module of the operator's tools, without types
import { LIMITS, emptyMetrics, rolloutSql, splitSpecs, verdict } from "../tools/rollout-verdict.mjs";

const m = (x: Record<string, number>) => ({ ...emptyMetrics(), ...x });

describe("the canary's verdict", () => {
  it("promotes a canary no worse than the previous version", () => {
    expect(verdict(m({ events: 500, objects: 3, errors: 1 }), m({ events: 5000, objects: 30, errors: 9 })).verdict).toBe("promote");
    // a similar rate is fine, a single error is noise
    expect(verdict(m({ events: 500, objects: 3, errors: 2 }), m({ events: 5000, objects: 30, errors: 0 })).verdict).toBe("promote");
  });
  it("rolls back on errors, exceptions or racers out of the race well above the previous rate", () => {
    const errors = verdict(m({ events: 500, objects: 3, errors: 8 }), m({ events: 5000, objects: 30, errors: 4 }));
    expect(errors.verdict).toBe("rollback");
    expect(errors.reasons[0]).toMatch(/^errors: 8/);
    expect(verdict(m({ events: 500, objects: 3, exceptions: 5 }), m({ events: 5000, objects: 30 })).verdict).toBe("rollback");
    const out = verdict(m({ events: 900, objects: 2, outOfRace: 4 }), m({ events: 9000, objects: 20, outOfRace: 2 }));
    expect(out.verdict).toBe("rollback");
    expect(out.reasons[0]).toMatch(/out of the race/);
  });
  it("doesn't count a room's own restart for new code as an exception", () => {
    expect(verdict(m({ events: 500, objects: 3, exceptions: 4, restarts: 4 }), m({ events: 5000, objects: 30 })).verdict).toBe("promote");
  });
  it("says when the canary saw too little traffic", () => {
    const thin = verdict(m({ events: 3, objects: 0 }), m({ events: 5000, objects: 30 }));
    expect(thin.verdict).toBe("thin");
    expect(LIMITS.minEvents).toBeGreaterThan(3);
  });
});

describe("the deployment's commands", () => {
  it("splits the traffic, or deploys alone", () => {
    expect(splitSpecs("b", "a", 10)).toEqual(["b@10%", "a@90%"]);
    expect(splitSpecs("b", "a", 100)).toEqual(["b@100%"]);
    expect(splitSpecs("b", null, 10)).toEqual(["b@100%"]);
  });
  it("writes only a version id or the hold into the rollout row", () => {
    expect(rolloutSql("625b5b91-53bc-4ce0-a3cd-147cc78a542e", 5)).toContain("'625b5b91-53bc-4ce0-a3cd-147cc78a542e', 5");
    expect(rolloutSql("", 5)).toContain("'rollout', '', 5");
    expect(() => rolloutSql("x'); DROP TABLE bans; --", 5)).toThrow();
  });
});
