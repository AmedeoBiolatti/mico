import { mkdirSync, readdirSync } from "node:fs";
import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { homedir } from "node:os";
import { dirname, resolve } from "node:path";
import { parse as parseUrl } from "node:url";
import type { SessionManager } from "./session-manager.js";
import type { AddWorkspaceRequest, ResizeRunRequest, StartRunRequest, UpdateWorkspaceRequest, WriteRunInputRequest } from "./types.js";

export type SupervisorServerOptions = {
  host?: string;
  port?: number;
};

export function createSupervisorServer(manager: SessionManager, options: SupervisorServerOptions = {}) {
  const host = options.host ?? "127.0.0.1";
  const port = options.port ?? 4317;
  const server = createServer(async (request, response) => {
    try {
      await route(manager, request, response);
    } catch (error) {
      sendJson(response, 500, { error: error instanceof Error ? error.message : String(error) });
    }
  });

  return {
    server,
    listen() {
      server.listen(port, host);
      return { host, port };
    }
  };
}

async function route(manager: SessionManager, request: IncomingMessage, response: ServerResponse): Promise<void> {
  const url = parseUrl(request.url ?? "/", true);
  const pathname = url.pathname ?? "/";
  const method = request.method ?? "GET";

  if (method === "OPTIONS") {
    sendJson(response, 204, {});
    return;
  }

  if (method === "GET" && pathname === "/health") {
    sendJson(response, 200, { ok: true });
    return;
  }

  if (method === "GET" && pathname === "/api/harnesses") {
    sendJson(response, 200, { harnesses: manager.harnesses() });
    return;
  }

  if (method === "GET" && pathname === "/api/fs") {
    const requested = typeof url.query.path === "string" && url.query.path.length > 0 ? url.query.path : homedir();
    try {
      sendJson(response, 200, listDirectory(requested));
    } catch (error) {
      sendJson(response, 400, { error: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  if (method === "POST" && pathname === "/api/fs/mkdir") {
    const body = await readJson<{ path?: string }>(request);
    if (typeof body.path !== "string" || body.path.trim().length === 0) {
      sendJson(response, 400, { error: "path is required" });
      return;
    }
    try {
      const created = resolveFsPath(body.path);
      mkdirSync(created, { recursive: true });
      sendJson(response, 201, { path: created });
    } catch (error) {
      sendJson(response, 400, { error: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  if (method === "GET" && pathname === "/api/workspaces") {
    sendJson(response, 200, { workspaces: manager.workspaces() });
    return;
  }

  if (method === "POST" && pathname === "/api/workspaces") {
    const body = await readJson<AddWorkspaceRequest>(request);
    try {
      const workspace = manager.addWorkspace(body);
      sendJson(response, 201, { workspace });
    } catch (error) {
      sendJson(response, 400, { error: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  const workspaceMatch = pathname.match(/^\/api\/workspaces\/([^/]+)$/);
  if (method === "PATCH" && workspaceMatch?.[1]) {
    const body = await readJson<UpdateWorkspaceRequest>(request);
    const workspace = manager.updateWorkspace(workspaceMatch[1], body);
    sendJson(response, 200, { workspace });
    return;
  }

  if (method === "DELETE" && workspaceMatch?.[1]) {
    manager.removeWorkspace(workspaceMatch[1]);
    sendJson(response, 200, { ok: true });
    return;
  }

  if (method === "GET" && pathname === "/api/runs") {
    sendJson(response, 200, { runs: manager.runs() });
    return;
  }

  if (method === "POST" && pathname === "/api/runs") {
    const body = await readJson<StartRunRequest>(request);
    const run = manager.start(body);
    sendJson(response, 201, { run });
    return;
  }

  const runMatch = pathname.match(/^\/api\/runs\/([^/]+)$/);
  if (method === "GET" && runMatch?.[1]) {
    const run = manager.run(runMatch[1]);
    if (!run) {
      sendJson(response, 404, { error: "run not found" });
      return;
    }
    sendJson(response, 200, { run, events: manager.events.list(run.id), git: manager.gitForRun(run.id) });
    return;
  }

  if (method === "DELETE" && runMatch?.[1]) {
    manager.remove(runMatch[1]);
    sendJson(response, 200, { ok: true });
    return;
  }

  if (method === "GET" && pathname === "/api/approvals") {
    sendJson(response, 200, { approvals: manager.approvals() });
    return;
  }

  if (method === "GET" && pathname === "/api/approval-rules") {
    sendJson(response, 200, { rules: manager.approvalRules() });
    return;
  }

  if (method === "POST" && pathname === "/api/approval-rules") {
    const body = await readJson<{ workspaceId?: string | null; toolName?: string; commandPrefix?: string | null }>(request);
    if (typeof body.toolName !== "string" || body.toolName.length === 0) {
      sendJson(response, 400, { error: "toolName is required" });
      return;
    }
    const rule = manager.addApprovalRule({ workspaceId: body.workspaceId ?? null, toolName: body.toolName, commandPrefix: body.commandPrefix ?? null });
    sendJson(response, 201, { rule });
    return;
  }

  const ruleMatch = pathname.match(/^\/api\/approval-rules\/([^/]+)$/);
  if (method === "DELETE" && ruleMatch?.[1]) {
    manager.removeApprovalRule(ruleMatch[1]);
    sendJson(response, 200, { ok: true });
    return;
  }

  const approvalRequestMatch = pathname.match(/^\/api\/runs\/([^/]+)\/approvals$/);
  if (method === "POST" && approvalRequestMatch?.[1]) {
    const body = await readJson<{ toolName?: string; input?: unknown }>(request);
    const approval = manager.requestApproval(approvalRequestMatch[1], body.toolName ?? "unknown", body.input ?? null);
    sendJson(response, 201, { approval });
    return;
  }

  const approvalWaitMatch = pathname.match(/^\/api\/approvals\/([^/]+)\/wait$/);
  if (method === "GET" && approvalWaitMatch?.[1]) {
    const timeoutRaw = Number(url.query.timeout);
    const timeout = Number.isFinite(timeoutRaw) ? Math.min(Math.max(timeoutRaw, 1000), 60000) : 25000;
    const approval = await manager.waitForApproval(approvalWaitMatch[1], timeout);
    sendJson(response, 200, { approval });
    return;
  }

  const approvalDecisionMatch = pathname.match(/^\/api\/approvals\/([^/]+)\/decision$/);
  if (method === "POST" && approvalDecisionMatch?.[1]) {
    const body = await readJson<{ state?: string; always?: boolean }>(request);
    if (body.state !== "allowed" && body.state !== "denied") {
      sendJson(response, 400, { error: "state must be allowed or denied" });
      return;
    }
    const approval = manager.decideApproval(approvalDecisionMatch[1], body.state, body.always === true);
    sendJson(response, 200, { approval });
    return;
  }

  const eventMatch = pathname.match(/^\/api\/runs\/([^/]+)\/events$/);
  if (method === "GET" && eventMatch?.[1]) {
    streamEvents(manager, eventMatch[1], response);
    return;
  }

  const diffMatch = pathname.match(/^\/api\/runs\/([^/]+)\/diff$/);
  if (method === "GET" && diffMatch?.[1]) {
    const file = typeof url.query.file === "string" && url.query.file.length > 0 ? url.query.file : undefined;
    try {
      sendJson(response, 200, { diff: manager.runDiff(diffMatch[1], file) });
    } catch (error) {
      sendJson(response, 400, { error: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  const mergeMatch = pathname.match(/^\/api\/runs\/([^/]+)\/merge$/);
  if (method === "POST" && mergeMatch?.[1]) {
    try {
      sendJson(response, 200, { run: manager.mergeRun(mergeMatch[1]) });
    } catch (error) {
      sendJson(response, 400, { error: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  const discardMatch = pathname.match(/^\/api\/runs\/([^/]+)\/discard$/);
  if (method === "POST" && discardMatch?.[1]) {
    try {
      sendJson(response, 200, { run: manager.discardRun(discardMatch[1]) });
    } catch (error) {
      sendJson(response, 400, { error: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  const inputMatch = pathname.match(/^\/api\/runs\/([^/]+)\/input$/);
  if (method === "POST" && inputMatch?.[1]) {
    const body = await readJson<WriteRunInputRequest>(request);
    manager.write(inputMatch[1], body.input);
    sendJson(response, 200, { ok: true });
    return;
  }


  const resizeMatch = pathname.match(/^\/api\/runs\/([^/]+)\/resize$/);
  if (method === "POST" && resizeMatch?.[1]) {
    const body = await readJson<ResizeRunRequest>(request);
    manager.resize(resizeMatch[1], body.cols, body.rows);
    sendJson(response, 200, { ok: true });
    return;
  }

  const terminateMatch = pathname.match(/^\/api\/runs\/([^/]+)\/terminate$/);
  if (method === "POST" && terminateMatch?.[1]) {
    manager.terminate(terminateMatch[1]);
    sendJson(response, 200, { ok: true });
    return;
  }

  sendJson(response, 404, { error: "not found" });
}

function streamEvents(manager: SessionManager, runId: string, response: ServerResponse): void {
  response.writeHead(200, {
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache, no-transform",
    Connection: "keep-alive",
    "Access-Control-Allow-Origin": "*"
  });

  for (const event of manager.events.list(runId)) {
    response.write(`event: ${event.type}\n`);
    response.write(`data: ${JSON.stringify(event)}\n\n`);
  }

  const unsubscribe = manager.events.subscribe((event) => {
    if (event.runId !== runId) {
      return;
    }

    response.write(`event: ${event.type}\n`);
    response.write(`data: ${JSON.stringify(event)}\n\n`);
  });

  response.on("close", unsubscribe);
}

function resolveFsPath(path: string): string {
  const expanded = path === "~" || path.startsWith("~/") ? `${homedir()}${path.slice(1)}` : path;
  return resolve(expanded);
}

function listDirectory(path: string): { path: string; parent: string | null; dirs: string[] } {
  const resolved = resolveFsPath(path);
  const entries = readdirSync(resolved, { withFileTypes: true });
  const dirs = entries
    .filter((entry) => entry.isDirectory() && !entry.name.startsWith("."))
    .map((entry) => entry.name)
    .sort((a, b) => a.localeCompare(b));
  const parent = dirname(resolved);

  return {
    path: resolved,
    parent: parent === resolved ? null : parent,
    dirs
  };
}

async function readJson<T>(request: IncomingMessage): Promise<T> {
  const chunks: Buffer[] = [];

  for await (const chunk of request) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }

  const raw = Buffer.concat(chunks).toString("utf8");
  return raw.length > 0 ? JSON.parse(raw) as T : {} as T;
}

function sendJson(response: ServerResponse, status: number, body: unknown): void {
  response.writeHead(status, {
    "Content-Type": "application/json",
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Headers": "content-type",
    "Access-Control-Allow-Methods": "GET,POST,PATCH,DELETE,OPTIONS"
  });
  response.end(JSON.stringify(body));
}
