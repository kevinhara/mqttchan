/**
 * A polling loop with per-tick error isolation.
 *
 * The isolation is the point: one failing tick - an API that 500s, a sensor
 * that vanishes - must never take the process down and stop every future poll.
 * The Python service this replaces learned the same lesson with a try/except
 * around each producer.
 */
export interface PollOptions {
  name: string;
  intervalMs: number;
  /** Run one tick immediately instead of waiting a full interval first. */
  immediate?: boolean;
}

export function startPolling(
  opts: PollOptions,
  tick: () => Promise<void>,
): () => void {
  let stopped = false;

  const run = async (): Promise<void> => {
    if (stopped) return;
    try {
      await tick();
    } catch (err) {
      console.log(`${opts.name}: poll failed: ${String(err)}`);
    }
  };

  if (opts.immediate === true) void run();
  const timer = setInterval(() => void run(), opts.intervalMs);

  return () => {
    stopped = true;
    clearInterval(timer);
  };
}
