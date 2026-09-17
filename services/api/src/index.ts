import { config } from "./config.js";
import { buildServer } from "./server.js";
import { MessageQueue } from "./queue.js";
import { Publisher } from "./publisher.js";

const publisher = new Publisher();
publisher.connect();

const queue = new MessageQueue((payload) => publisher.publish(payload));

// One drain tick. Errors are logged rather than thrown: a broker blip must not
// take the timer (and with it every future publish) down with it.
const timer = setInterval(() => {
  queue.drain().catch((err) => console.log(`drain: ${String(err)}`));
}, config.drainIntervalMs);

const app = buildServer(queue, publisher);

async function shutdown(signal: string): Promise<void> {
  console.log(`${signal}: shutting down`);
  clearInterval(timer);
  await app.close();
  await publisher.end();
  process.exit(0);
}

process.on("SIGTERM", () => void shutdown("SIGTERM"));
process.on("SIGINT", () => void shutdown("SIGINT"));

app
  .listen({ port: config.port, host: config.host })
  .then(() => console.log(`api: listening on ${config.host}:${config.port}`))
  .catch((err) => {
    console.log(`api: failed to start: ${String(err)}`);
    process.exit(1);
  });
