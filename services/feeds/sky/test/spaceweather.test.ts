import { describe, expect, it } from "vitest";
import { auroraEdge } from "../src/spaceweather.js";

describe("auroraEdge", () => {
  const ON = 5;
  const OFF = 4;

  it("alerts once when Kp crosses up through the on-threshold", () => {
    const first = auroraEdge(5.33, false, ON, OFF);
    expect(first).toEqual({ shouldAlert: true, alerted: true });
  });

  it("does not re-alert while Kp stays elevated", () => {
    const result = auroraEdge(6.0, true, ON, OFF);
    expect(result).toEqual({ shouldAlert: false, alerted: true });
  });

  it("does not flap when Kp dips between the on- and off-thresholds", () => {
    // Already alerted; 4.67 is below ON but not below OFF.
    const result = auroraEdge(4.67, true, ON, OFF);
    expect(result).toEqual({ shouldAlert: false, alerted: true });
  });

  it("clears once Kp drops below the off-threshold, ready to alert again", () => {
    const cleared = auroraEdge(3.0, true, ON, OFF);
    expect(cleared).toEqual({ shouldAlert: false, alerted: false });

    const second = auroraEdge(5.0, cleared.alerted, ON, OFF);
    expect(second).toEqual({ shouldAlert: true, alerted: true });
  });

  it("stays quiet below the on-threshold when not already alerted", () => {
    const result = auroraEdge(2.0, false, ON, OFF);
    expect(result).toEqual({ shouldAlert: false, alerted: false });
  });
});
