#!/usr/bin/env node
// Minimal MCP stdio server exposing one tool: `approve`.
// Claude Code invokes it via --permission-prompt-tool mcp__mico__approve.
// Each call is forwarded to the MiCo supervisor as an approval request,
// then blocks until a human (or policy) decides.

const base = process.env.MICO_BASE ?? "http://127.0.0.1:4317";
const runId = process.env.MICO_RUN_ID ?? "";

let buffer = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffer += chunk;
  let index;
  while ((index = buffer.indexOf("\n")) !== -1) {
    const line = buffer.slice(0, index).trim();
    buffer = buffer.slice(index + 1);
    if (line.length > 0) {
      void handleMessage(line);
    }
  }
});

function send(message) {
  process.stdout.write(`${JSON.stringify(message)}\n`);
}

async function handleMessage(line) {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }

  if (message.method === "initialize") {
    send({
      jsonrpc: "2.0",
      id: message.id,
      result: {
        protocolVersion: message.params?.protocolVersion ?? "2024-11-05",
        capabilities: { tools: {} },
        serverInfo: { name: "mico", version: "0.1.0" }
      }
    });
    return;
  }

  if (message.method === "tools/list") {
    send({
      jsonrpc: "2.0",
      id: message.id,
      result: {
        tools: [
          {
            name: "approve",
            description: "Ask the MiCo supervisor for permission to use a tool.",
            inputSchema: {
              type: "object",
              properties: {
                tool_name: { type: "string" },
                input: { type: "object" },
                tool_use_id: { type: "string" }
              },
              required: ["tool_name", "input"]
            }
          }
        ]
      }
    });
    return;
  }

  if (message.method === "tools/call") {
    const args = message.params?.arguments ?? {};
    let decision;
    try {
      decision = await requestDecision(args.tool_name ?? "unknown", args.input ?? {});
    } catch (error) {
      decision = { behavior: "deny", message: `mico approval broker error: ${error?.message ?? error}` };
    }
    send({
      jsonrpc: "2.0",
      id: message.id,
      result: { content: [{ type: "text", text: JSON.stringify(decision) }] }
    });
    return;
  }

  // respond to unknown requests (not notifications) so the client doesn't hang
  if (message.id !== undefined) {
    send({ jsonrpc: "2.0", id: message.id, error: { code: -32601, message: "method not found" } });
  }
}

async function requestDecision(toolName, input) {
  const created = await fetchJson(`${base}/api/runs/${runId}/approvals`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ toolName, input })
  });

  let approval = created.approval;
  const deadline = Date.now() + 10 * 60 * 1000;
  while (approval.state === "pending" && Date.now() < deadline) {
    const waited = await fetchJson(`${base}/api/approvals/${approval.id}/wait?timeout=25000`);
    approval = waited.approval;
  }

  if (approval.state === "allowed") {
    return { behavior: "allow", updatedInput: input };
  }

  return {
    behavior: "deny",
    message: approval.state === "pending" ? "mico approval timed out" : "denied via mico"
  };
}

async function fetchJson(url, init) {
  const response = await fetch(url, init);
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return response.json();
}
