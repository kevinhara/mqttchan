/**
 * Logs a given message once and then stays quiet.
 *
 * For the "not configured yet" case: a feed missing its token should say so
 * once and then poll silently, rather than printing the same line every minute
 * until the logs are useless. Lifted from the Python producers'
 * `_warned_unconfigured` flag.
 */
export class WarnOnce {
  private readonly seen = new Set<string>();

  warn(key: string, message: string): void {
    if (this.seen.has(key)) return;
    this.seen.add(key);
    console.log(message);
  }

  reset(key: string): void {
    this.seen.delete(key);
  }
}
