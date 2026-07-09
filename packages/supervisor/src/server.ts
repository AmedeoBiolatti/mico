import { createSupervisorServer } from "./http.js";
import { SessionManager } from "./session-manager.js";
import { RunStore } from "./store.js";

const store = new RunStore(process.env.MICO_SUPERVISOR_DB ? { path: process.env.MICO_SUPERVISOR_DB } : {});
const manager = new SessionManager({ store });
const port = Number.parseInt(process.env.MICO_SUPERVISOR_PORT ?? "4317", 10);
const host = process.env.MICO_SUPERVISOR_HOST ?? "127.0.0.1";
const app = createSupervisorServer(manager, { host, port });
const address = app.listen();

console.log(`MiCo supervisor listening on http://${address.host}:${address.port}`);
console.log(`SQLite store: ${store.path}`);
console.log("Available harnesses:", manager.harnesses().map((harness) => harness.id).join(", "));
