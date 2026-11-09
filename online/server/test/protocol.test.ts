import { describe, expect, it } from "vitest";
import {
  CODE_ALPHABET,
  clientVersionOf,
  DEFAULT_MIN_VERSION,
  isOutdated,
  minVersionOf,
  parseVersion,
  isValidCode,
  measuredRtt,
  randomCode,
} from "../src/protocol";

describe("join codes", () => {
  it("are 6 characters with no confusable ones", () => {
    for (const c of "01ILO") expect(CODE_ALPHABET).not.toContain(c);
    for (let i = 0; i < 200; i++) {
      const code = randomCode();
      expect(code).toHaveLength(6);
      expect(isValidCode(code)).toBe(true);
    }
    expect(isValidCode("ABC0EF")).toBe(false);
  });
});

describe("measuredRtt", () => {
  it("uses the first ping only until a later one arrives", () => {
    expect(measuredRtt(null, [])).toBeNull();
    expect(measuredRtt(140, [])).toBe(140);
    // the warm-up ping reads high: the later ones' median
    expect(measuredRtt(140, [55, 60, 53])).toBe(55);
  });
});

describe("version gate", () => {
  it("reads X.Y.Z, with an optional v, patch and suffix", () => {
    expect(parseVersion("0.2.0")).toEqual([0, 2, 0]);
    expect(parseVersion("v1.10.3")).toEqual([1, 10, 3]);
    expect(parseVersion("1.2")).toEqual([1, 2, 0]);
    expect(parseVersion("0.3.0-rc1")).toEqual([0, 3, 0]);
    for (const bad of ["", "dhd-port 274f8c46", "1", "a.b.c", 2, null, undefined]) expect(parseVersion(bad)).toBeNull();
  });

  it("takes a hello without clientVersion as outdated", () => {
    expect(clientVersionOf({ version: "dhd-port 274f8c46f1af52d6" })).toBe("");
    expect(isOutdated(clientVersionOf({}), "0.1.0")).toBe(true);
    expect(clientVersionOf({ clientVersion: "0.2.0" })).toBe("0.2.0");
  });

  it("refuses releases older than the minimum and unreadable ones", () => {
    expect(isOutdated("0.1.0", "0.1.0")).toBe(false);
    expect(isOutdated("0.1.0", "0.2.0")).toBe(true);
    expect(isOutdated("0.2.0", "0.2.0")).toBe(false);
    expect(isOutdated("0.10.0", "0.9.0")).toBe(false); // numbers, not text
    expect(isOutdated("1.0.0", "0.2.0")).toBe(false);
    expect(isOutdated("0.1.9", "0.2.0")).toBe(true);
    expect(isOutdated("junk", "0.1.0")).toBe(true);
  });

  it("falls back to the default minimum when MIN_VERSION isn't a version", () => {
    expect(minVersionOf("0.2.0")).toBe("0.2.0");
    expect(minVersionOf(undefined)).toBe(DEFAULT_MIN_VERSION);
    expect(minVersionOf("latest")).toBe(DEFAULT_MIN_VERSION);
  });
});
