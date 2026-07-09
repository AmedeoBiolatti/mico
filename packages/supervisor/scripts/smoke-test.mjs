const base = process.env.MICO_SUPERVISOR_URL ?? "http://127.0.0.1:4317";

async function json(path, options) {
  const response = await fetch(`${base}${path}`, options);
  const body = await response.json();

  if (!response.ok) {
    throw new Error(`${response.status} ${JSON.stringify(body)}`);
  }

  return body;
}

const health = await json("/health");
const harnesses = await json("/api/harnesses");
const started = await json("/api/runs", {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ command: "printf smoke-test", cols: 80, rows: 24 })
});
await new Promise((resolve) => setTimeout(resolve, 250));
const detail = await json(`/api/runs/${started.run.id}`);

console.log(JSON.stringify({
  health,
  harnesses: harnesses.harnesses.map((harness) => harness.id),
  run: detail.run,
  events: detail.events.map((event) => event.type),
  changedFiles: detail.git.changedFiles
}, null, 2));
