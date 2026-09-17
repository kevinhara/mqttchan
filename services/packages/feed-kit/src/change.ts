/**
 * Edge-triggered change tracking.
 *
 * This is deliberately shared rather than left to each feed. The predecessor
 * service had a producer that announced the ISS overhead on *every* poll while
 * it was still overhead, and it got switched off for being noisy. Publishing on
 * change - and only on a change big enough to care about - is the default a
 * feed should have to opt out of, not remember to opt in to.
 */
export class ChangeTracker {
  private readonly last = new Map<string, number>();

  /**
   * True the first time a key is seen, and thereafter only when it has moved by
   * at least `threshold`. Records the new value whenever it returns true, so
   * slow drift accumulates into one eventual report rather than being lost.
   */
  changedBy(key: string, value: number, threshold: number): boolean {
    const previous = this.last.get(key);
    if (previous === undefined || Math.abs(value - previous) >= threshold) {
      this.last.set(key, value);
      return true;
    }
    return false;
  }

  /** True only when the value differs from last time. For non-numeric state. */
  changed(key: string, value: number): boolean {
    return this.changedBy(key, value, Number.MIN_VALUE);
  }

  forget(key: string): void {
    this.last.delete(key);
  }
}
