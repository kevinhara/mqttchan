import type { MessageRequest, MessageResponse } from "@mqttchan/contract";

export interface ClientOptions {
  /** e.g. "http://api:8420" */
  baseUrl: string;
  /** Feed name, stamped onto every message as `source`. */
  source: string;
  timeoutMs?: number;
  retries?: number;
  /** Log but do not send. Wire this to a --dry-run flag. */
  dryRun?: boolean;
}

/** Thin typed wrapper over POST /v1/messages. */
export class ApiClient {
  private readonly baseUrl: string;
  private readonly source: string;
  private readonly timeoutMs: number;
  private readonly retries: number;
  private readonly dryRun: boolean;

  constructor(opts: ClientOptions) {
    this.baseUrl = opts.baseUrl.replace(/\/+$/, "");
    this.source = opts.source;
    this.timeoutMs = opts.timeoutMs ?? 5000;
    this.retries = opts.retries ?? 2;
    this.dryRun = opts.dryRun ?? false;
  }

  async send(
    message: Omit<MessageRequest, "source">,
  ): Promise<MessageResponse | null> {
    const body: MessageRequest = { ...message, source: this.source };

    if (this.dryRun) {
      console.log(`${this.source}: [dry-run] ${JSON.stringify(body)}`);
      return null;
    }

    let lastError: unknown;
    for (let attempt = 0; attempt <= this.retries; attempt++) {
      try {
        const res = await fetch(`${this.baseUrl}/v1/messages`, {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify(body),
          signal: AbortSignal.timeout(this.timeoutMs),
        });
        if (!res.ok) {
          // A 4xx is our bug, not a blip - retrying will not fix it.
          const text = await res.text();
          if (res.status < 500) {
            throw new Error(`api rejected message (${res.status}): ${text}`);
          }
          lastError = new Error(`api error ${res.status}: ${text}`);
        } else {
          return (await res.json()) as MessageResponse;
        }
      } catch (err) {
        lastError = err;
        if (err instanceof Error && err.message.startsWith("api rejected")) throw err;
      }
      if (attempt < this.retries) {
        await new Promise((r) => setTimeout(r, 500 * (attempt + 1)));
      }
    }
    throw lastError instanceof Error ? lastError : new Error(String(lastError));
  }
}
