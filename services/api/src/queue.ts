/**
 * Triage, rate limiting and ordering - everything that stops a chatty feed
 * from burying the device.
 *
 * Why this exists at all: the device shows one message at a time and holds it
 * for ~30s after typing it out, and its own queue is an unbounded std::deque
 * that never drains once you overrun it. A feed that publishes faster than the
 * device can display builds a permanent backlog. So the API paces publishes to
 * the device's real speed and decides what to throw away, rather than letting
 * the device accept everything and fall behind forever.
 */

import type {
  Decision,
  MessageRequest,
  MessageResponse,
  Priority,
} from "@mqttchan/contract";
import { config } from "./config.js";
import { finalisePayload, type DevicePayload } from "./device.js";
import { presentationFor } from "./triage.js";

const RANK: Record<Priority, number> = { high: 0, normal: 1, low: 2 };

interface QueuedItem {
  seq: number;
  source: string;
  kind: string;
  priority: Priority;
  dedupeKey: string | undefined;
  payload: DevicePayload;
  enqueuedAt: number;
  expiresAt: number;
}

/**
 * Classic token bucket, per source. The global gap below is the real limiter;
 * this exists so one misbehaving feed cannot monopolise the device by keeping
 * the queue permanently full of its own messages.
 */
class TokenBucket {
  private tokens: number;
  private lastRefill: number;

  constructor(
    private readonly capacity: number,
    private readonly refillPerMinute: number,
    now: number,
  ) {
    this.tokens = capacity;
    this.lastRefill = now;
  }

  tryTake(now: number): boolean {
    const elapsedMinutes = (now - this.lastRefill) / 60_000;
    if (elapsedMinutes > 0) {
      this.tokens = Math.min(
        this.capacity,
        this.tokens + elapsedMinutes * this.refillPerMinute,
      );
      this.lastRefill = now;
    }
    if (this.tokens < 1) return false;
    this.tokens -= 1;
    return true;
  }
}

export interface SourceStats {
  submitted: number;
  queued: number;
  deduped: number;
  droppedRateLimited: number;
  droppedExpired: number;
  published: number;
}

export interface QueueStatus {
  queueDepth: number;
  nextPublishInMs: number;
  lastPublishedAt: string | null;
  lastPublishedText: string | null;
  totals: SourceStats;
  bySource: Record<string, SourceStats>;
}

function emptyStats(): SourceStats {
  return {
    submitted: 0,
    queued: 0,
    deduped: 0,
    droppedRateLimited: 0,
    droppedExpired: 0,
    published: 0,
  };
}

export type PublishFn = (payload: DevicePayload) => Promise<void>;

export class MessageQueue {
  private items: QueuedItem[] = [];
  private buckets = new Map<string, TokenBucket>();
  private stats = new Map<string, SourceStats>();
  private seq = 0;
  private nextPublishAt = 0;
  private publishing = false;
  private lastPublishedAt: number | null = null;
  private lastPublishedText: string | null = null;

  constructor(
    private readonly publish: PublishFn,
    private readonly now: () => number = () => Date.now(),
  ) {}

  /**
   * How long the device will be busy with this message, and therefore how long
   * to wait before sending the next one. Deliberately derived from the text
   * rather than a flat constant: the bubble reveals at ~45ms/char, so a long
   * message legitimately needs more room than a short one. Mirrors the
   * firmware's own display() timing - jingle, then a settling gap, then typing,
   * then the hold.
   */
  estimateDisplayMs(payload: DevicePayload): number {
    const { holdSeconds, charMs, jingleMs, safetyMs } = config.device;
    const jingle = payload.jingle === "" ? 0 : jingleMs;
    return jingle + payload.text.length * charMs + holdSeconds * 1000 + safetyMs;
  }

  submit(req: MessageRequest): MessageResponse {
    const now = this.now();
    const stats = this.statsFor(req.source);
    stats.submitted += 1;

    const priority: Priority = req.priority ?? "normal";

    let bucket = this.buckets.get(req.source);
    if (bucket === undefined) {
      bucket = new TokenBucket(
        config.rateLimit.capacity,
        config.rateLimit.refillPerMinute,
        now,
      );
      this.buckets.set(req.source, bucket);
    }
    if (!bucket.tryTake(now)) {
      stats.droppedRateLimited += 1;
      return this.decide("dropped", "rate limited for this source");
    }

    // Throws PayloadError for anything the device could not render; the route
    // turns that into a 400 so the feed hears about it.
    const payload = finalisePayload(
      { ...presentationFor(req.kind, priority), text: req.text },
      config.mqtt.maxPayloadBytes,
    );

    const ttlSeconds = req.ttlSeconds ?? config.defaultTtlSeconds;
    const expiresAt = now + ttlSeconds * 1000;

    if (req.dedupeKey !== undefined) {
      const existing = this.items.find((i) => i.dedupeKey === req.dedupeKey);
      if (existing !== undefined) {
        // Replace in place rather than re-queueing at the back: the feed is
        // updating a value it already has pending, not asking for a new turn.
        existing.payload = payload;
        existing.priority = priority;
        existing.expiresAt = expiresAt;
        existing.kind = req.kind;
        this.sort();
        stats.deduped += 1;
        return this.decide("deduped", "replaced a queued message with the same dedupeKey");
      }
    }

    this.items.push({
      seq: this.seq++,
      source: req.source,
      kind: req.kind,
      priority,
      dedupeKey: req.dedupeKey,
      payload,
      enqueuedAt: now,
      expiresAt,
    });
    this.sort();
    stats.queued += 1;
    return this.decide("queued");
  }

  /** Called on a timer. Publishes at most one message per due window. */
  async drain(): Promise<void> {
    if (this.publishing) return;
    const now = this.now();

    // Expire first, so a stale message never occupies the one slot that is due.
    this.items = this.items.filter((item) => {
      if (item.expiresAt > now) return true;
      this.statsFor(item.source).droppedExpired += 1;
      return false;
    });

    if (this.items.length === 0 || now < this.nextPublishAt) return;

    const next = this.items.shift();
    if (next === undefined) return;

    this.publishing = true;
    try {
      await this.publish(next.payload);
      this.statsFor(next.source).published += 1;
      this.lastPublishedAt = now;
      this.lastPublishedText = next.payload.text;
      this.nextPublishAt = now + this.estimateDisplayMs(next.payload);
    } catch (err) {
      // Put it back at the front: a broker blip should delay a message, not
      // silently eat it. It can still expire on a later pass.
      this.items.unshift(next);
      throw err;
    } finally {
      this.publishing = false;
    }
  }

  status(): QueueStatus {
    const now = this.now();
    const totals = emptyStats();
    const bySource: Record<string, SourceStats> = {};
    for (const [source, s] of this.stats) {
      bySource[source] = { ...s };
      totals.submitted += s.submitted;
      totals.queued += s.queued;
      totals.deduped += s.deduped;
      totals.droppedRateLimited += s.droppedRateLimited;
      totals.droppedExpired += s.droppedExpired;
      totals.published += s.published;
    }
    return {
      queueDepth: this.items.length,
      nextPublishInMs: Math.max(0, this.nextPublishAt - now),
      lastPublishedAt:
        this.lastPublishedAt === null
          ? null
          : new Date(this.lastPublishedAt).toISOString(),
      lastPublishedText: this.lastPublishedText,
      totals,
      bySource,
    };
  }

  /** Highest priority first, FIFO within a priority. */
  private sort(): void {
    this.items.sort(
      (a, b) => RANK[a.priority] - RANK[b.priority] || a.seq - b.seq,
    );
  }

  private statsFor(source: string): SourceStats {
    let s = this.stats.get(source);
    if (s === undefined) {
      s = emptyStats();
      this.stats.set(source, s);
    }
    return s;
  }

  private decide(decision: Decision, reason?: string): MessageResponse {
    return reason === undefined ? { decision } : { decision, reason };
  }
}
