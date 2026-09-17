import Fastify, { type FastifyInstance } from "fastify";
import {
  messageRequestSchema,
  type MessageRequest,
  type MessageResponse,
} from "@mqttchan/contract";
import { PayloadError } from "./device.js";
import type { MessageQueue } from "./queue.js";
import type { Publisher } from "./publisher.js";
import { knownKinds } from "./triage.js";

export function buildServer(
  queue: MessageQueue,
  publisher: Publisher,
): FastifyInstance {
  const app = Fastify({ logger: false });

  app.post<{ Body: MessageRequest; Reply: MessageResponse | { error: string } }>(
    "/v1/messages",
    { schema: { body: messageRequestSchema } },
    async (request, reply) => {
      try {
        const result = queue.submit(request.body);
        // 202: accepted for delivery, not delivered. The device may be busy for
        // half a minute, and the decision tells the feed what actually happened
        // to its message rather than leaving it to assume success.
        return await reply.code(202).send(result);
      } catch (err) {
        if (err instanceof PayloadError) {
          return await reply.code(400).send({ error: err.message });
        }
        throw err;
      }
    },
  );

  app.get("/v1/health", async () => ({
    ok: true,
    broker: publisher.connected ? "connected" : "disconnected",
  }));

  app.get("/v1/status", async () => ({
    ...queue.status(),
    broker: publisher.connected ? "connected" : "disconnected",
    kinds: knownKinds(),
  }));

  return app;
}
