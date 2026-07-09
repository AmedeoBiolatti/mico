import { statSync } from "node:fs";
import { basename, resolve } from "node:path";
import { cwd as processCwd, env as processEnv } from "node:process";
import { defaultHarnesses, type HarnessAdapter } from "./harnesses.js";
import { EventLog } from "./events.js";
import { parseClaudeStreamLine } from "./agent-events.js";
import { addWorktree, captureGitSnapshot, changedFilesFromSnapshots, diffText, mergeWorktree, removeWorktree } from "./git.js";
import type { SessionProcess } from "./process.js";
import type { RunStore } from "./store.js";
import type { AddWorkspaceRequest, ApprovalRecord, ApprovalRule, ApprovalState, GitSnapshot, HarnessDescriptor, RunGitDetails, RunRecord, StartRunRequest, UpdateWorkspaceRequest, WorkspaceRecord } from "./types.js";

export type SessionManagerOptions = {
  store?: RunStore;
  harnesses?: HarnessAdapter[];
};

export class SessionManager {
  readonly events: EventLog;
  readonly #runs = new Map<string, RunRecord>();
  readonly #processes = new Map<string, SessionProcess>();
  readonly #harnesses = new Map<string, HarnessAdapter>();
  readonly #store: RunStore | undefined;
  readonly #workspaces = new Map<string, WorkspaceRecord>();
  readonly #approvals = new Map<string, ApprovalRecord>();
  readonly #approvalWaiters = new Map<string, Set<(approval: ApprovalRecord) => void>>();
  readonly #approvalRules = new Map<string, ApprovalRule>();
  readonly #outputBuffers = new Map<string, { text: string; timer: NodeJS.Timeout | null }>();
  #nextRunId: number;
  #nextWorkspaceId: number;
  #nextApprovalId: number;
  #nextRuleId: number;

  constructor(options: SessionManagerOptions = {}) {
    this.#store = options.store;
    const storedRuns = this.#store?.listRuns() ?? [];
    for (const run of storedRuns) {
      this.#runs.set(run.id, normalizeStoredRun(run));
    }

    this.events = new EventLog({
      ...(this.#store ? { store: this.#store } : {}),
      nextId: this.#store?.nextEventNumber() ?? 1
    });

    // compact any runs that ended before compaction existed (or before a crash)
    if (this.#store) {
      for (const run of storedRuns) {
        try {
          this.#store.compactRunOutput(run.id);
        } catch {
          // best effort — a broken row must not block boot
        }
      }
    }
    this.#nextRunId = this.#store?.nextRunNumber() ?? 1;

    for (const harness of options.harnesses ?? defaultHarnesses()) {
      this.#harnesses.set(harness.descriptor.id, harness);
    }

    for (const workspace of this.#store?.listWorkspaces() ?? []) {
      this.#workspaces.set(workspace.id, workspace);
    }
    this.#nextWorkspaceId = this.#store?.nextWorkspaceNumber() ?? 1;
    if (this.#workspaces.size === 0) {
      this.addWorkspace({ path: processCwd() });
    }

    for (const approval of this.#store?.listApprovals() ?? []) {
      // pending approvals cannot survive a restart — their requester is gone
      this.#approvals.set(approval.id, approval.state === "pending"
        ? { ...approval, state: "denied", decidedAt: new Date().toISOString() }
        : approval);
    }
    this.#nextApprovalId = this.#store?.nextApprovalNumber() ?? 1;

    for (const rule of this.#store?.listApprovalRules() ?? []) {
      this.#approvalRules.set(rule.id, rule);
    }
    this.#nextRuleId = this.#store?.nextApprovalRuleNumber() ?? 1;
  }

  approvalRules(): ApprovalRule[] {
    return [...this.#approvalRules.values()];
  }

  addApprovalRule(request: { workspaceId?: string | null; toolName: string; commandPrefix?: string | null }): ApprovalRule {
    const workspaceId = request.workspaceId ?? null;
    const commandPrefix = request.commandPrefix?.trim() || null;
    const existing = [...this.#approvalRules.values()].find(
      (rule) => rule.workspaceId === workspaceId && rule.toolName === request.toolName && rule.commandPrefix === commandPrefix
    );
    if (existing) {
      return existing;
    }

    const rule: ApprovalRule = {
      id: `rule_${this.#nextRuleId++}`,
      workspaceId,
      toolName: request.toolName,
      commandPrefix,
      createdAt: new Date().toISOString()
    };
    this.#approvalRules.set(rule.id, rule);
    this.#store?.upsertApprovalRule(rule);
    return rule;
  }

  removeApprovalRule(ruleId: string): void {
    if (!this.#approvalRules.has(ruleId)) {
      throw new Error(`Unknown rule: ${ruleId}`);
    }
    this.#approvalRules.delete(ruleId);
    this.#store?.deleteApprovalRule(ruleId);
  }

  #ruleMatches(workspaceId: string | null, toolName: string, input: unknown): boolean {
    const command = input && typeof input === "object" && typeof (input as { command?: unknown }).command === "string"
      ? (input as { command: string }).command
      : null;

    for (const rule of this.#approvalRules.values()) {
      if (rule.toolName !== toolName) {
        continue;
      }
      if (rule.workspaceId !== null && rule.workspaceId !== workspaceId) {
        continue;
      }
      if (rule.commandPrefix !== null && (command === null || !command.startsWith(rule.commandPrefix))) {
        continue;
      }
      return true;
    }
    return false;
  }

  approvals(): ApprovalRecord[] {
    return [...this.#approvals.values()];
  }

  approval(id: string): ApprovalRecord | undefined {
    return this.#approvals.get(id);
  }

  requestApproval(runId: string, toolName: string, input: unknown): ApprovalRecord {
    const run = this.#runs.get(runId);

    if (!run) {
      throw new Error(`Unknown run: ${runId}`);
    }

    const workspace = this.workspaceForCwd(run.cwd);
    const policy = workspace?.policy ?? "balanced";
    let via: ApprovalRecord["via"] | undefined;
    if (policy === "permissive") {
      via = "policy";
    } else if (this.#ruleMatches(workspace?.id ?? null, toolName, input)) {
      via = "rule";
    }
    const auto = via !== undefined;
    const requestedAt = new Date().toISOString();
    const approval: ApprovalRecord = {
      id: `ap_${this.#nextApprovalId++}`,
      runId,
      toolName,
      input,
      state: auto ? "allowed" : "pending",
      auto,
      ...(via !== undefined ? { via } : {}),
      requestedAt,
      ...(auto ? { decidedAt: requestedAt } : {})
    };
    this.#setApproval(approval);
    this.events.append(runId, "approval.requested", { approvalId: approval.id, toolName, policy, auto, via: via ?? null });
    if (auto) {
      this.events.append(runId, "approval.decided", { approvalId: approval.id, toolName, state: "allowed", auto: true, via });
    }
    return approval;
  }

  decideApproval(id: string, state: Exclude<ApprovalState, "pending">, always = false): ApprovalRecord {
    const approval = this.#approvals.get(id);

    if (!approval) {
      throw new Error(`Unknown approval: ${id}`);
    }

    if (approval.state !== "pending") {
      return approval;
    }

    if (always && state === "allowed") {
      const run = this.#runs.get(approval.runId);
      const workspace = run ? this.workspaceForCwd(run.cwd) : undefined;
      const command = approval.input && typeof approval.input === "object" && typeof (approval.input as { command?: unknown }).command === "string"
        ? (approval.input as { command: string }).command
        : null;
      this.addApprovalRule({
        workspaceId: workspace?.id ?? null,
        toolName: approval.toolName,
        commandPrefix: command ? command.trim().split(/\s+/)[0] ?? null : null
      });
    }

    const decided: ApprovalRecord = { ...approval, state, auto: false, decidedAt: new Date().toISOString() };
    this.#setApproval(decided);
    this.events.append(approval.runId, "approval.decided", { approvalId: id, toolName: approval.toolName, state, auto: false });

    const waiters = this.#approvalWaiters.get(id);
    if (waiters) {
      this.#approvalWaiters.delete(id);
      for (const waiter of waiters) {
        waiter(decided);
      }
    }

    return decided;
  }

  waitForApproval(id: string, timeoutMs: number): Promise<ApprovalRecord> {
    const approval = this.#approvals.get(id);

    if (!approval) {
      return Promise.reject(new Error(`Unknown approval: ${id}`));
    }

    if (approval.state !== "pending") {
      return Promise.resolve(approval);
    }

    return new Promise((resolve) => {
      const waiters = this.#approvalWaiters.get(id) ?? new Set();
      this.#approvalWaiters.set(id, waiters);

      const timer = setTimeout(() => {
        waiters.delete(waiter);
        resolve(this.#approvals.get(id) ?? approval);
      }, timeoutMs);

      const waiter = (decided: ApprovalRecord) => {
        clearTimeout(timer);
        resolve(decided);
      };
      waiters.add(waiter);
    });
  }

  #setApproval(approval: ApprovalRecord): void {
    this.#approvals.set(approval.id, approval);
    this.#store?.upsertApproval(approval);
  }

  workspaces(): WorkspaceRecord[] {
    return [...this.#workspaces.values()];
  }

  addWorkspace(request: AddWorkspaceRequest): WorkspaceRecord {
    const path = resolveWorkspacePath(request.path);
    const stats = statSync(path, { throwIfNoEntry: false });

    if (!stats?.isDirectory()) {
      throw new Error(`Not a directory: ${path}`);
    }

    const existing = [...this.#workspaces.values()].find((workspace) => workspace.path === path);
    if (existing) {
      return existing;
    }

    const workspace: WorkspaceRecord = {
      id: `ws_${this.#nextWorkspaceId++}`,
      name: request.name?.trim() || basename(path),
      path,
      policy: request.policy ?? "balanced",
      createdAt: new Date().toISOString()
    };
    this.#workspaces.set(workspace.id, workspace);
    this.#store?.upsertWorkspace(workspace);
    return workspace;
  }

  updateWorkspace(workspaceId: string, request: UpdateWorkspaceRequest): WorkspaceRecord {
    const workspace = this.#workspaces.get(workspaceId);

    if (!workspace) {
      throw new Error(`Unknown workspace: ${workspaceId}`);
    }

    const updated: WorkspaceRecord = {
      ...workspace,
      ...(request.name !== undefined ? { name: request.name } : {}),
      ...(request.policy !== undefined ? { policy: request.policy } : {})
    };
    this.#workspaces.set(workspaceId, updated);
    this.#store?.upsertWorkspace(updated);
    return updated;
  }

  workspaceForCwd(cwd: string): WorkspaceRecord | undefined {
    let best: WorkspaceRecord | undefined;
    for (const workspace of this.#workspaces.values()) {
      if (cwd === workspace.path || cwd.startsWith(`${workspace.path}/`)) {
        if (!best || workspace.path.length > best.path.length) {
          best = workspace;
        }
      }
    }
    return best;
  }

  removeWorkspace(workspaceId: string): void {
    if (!this.#workspaces.has(workspaceId)) {
      throw new Error(`Unknown workspace: ${workspaceId}`);
    }

    this.#workspaces.delete(workspaceId);
    this.#store?.deleteWorkspace(workspaceId);
  }

  harnesses(): HarnessDescriptor[] {
    return [...this.#harnesses.values()].map((harness) => harness.descriptor);
  }

  runs(): RunRecord[] {
    return [...this.#runs.values()];
  }

  run(id: string): RunRecord | undefined {
    return this.#runs.get(id);
  }

  gitForRun(id: string): RunGitDetails {
    const snapshots = this.#store?.listGitSnapshots(id) ?? [];
    const before = snapshots.find((snapshot) => snapshot.phase === "before") ?? null;
    const after = snapshots.find((snapshot) => snapshot.phase === "after") ?? null;

    return {
      before,
      after,
      changedFiles: changedFilesFromSnapshots(before, after)
    };
  }

  start(request: StartRunRequest): RunRecord {
    const harnessId = request.harnessId ?? "shell";
    const harness = this.#harnesses.get(harnessId);

    if (!harness) {
      throw new Error(`Unknown harness: ${harnessId}`);
    }

    const id = `run_${this.#nextRunId++}`;
    const createdAt = new Date().toISOString();
    const baseCwd = request.cwd ?? processCwd();
    const workspace = this.workspaceForCwd(baseCwd);
    const policy = request.policy ?? workspace?.policy ?? "balanced";

    let cwd = baseCwd;
    let worktree: string | undefined;
    if (request.isolate) {
      worktree = addWorktree(workspace?.path ?? baseCwd, id);
      cwd = worktree;
    }

    const run: RunRecord = {
      id,
      harnessId,
      command: request.command,
      cwd,
      status: "created",
      createdAt,
      ...(worktree !== undefined ? { worktree } : {})
    };
    this.#setRun(run);
    this.events.append(id, "run.created", { command: run.command, cwd: run.cwd, harnessId, policy, worktree: worktree ?? null });
    this.#captureGit(id, "before", run.cwd);

    try {
      const launch = harness.launch({ ...request, cwd, policy, runId: id });
      const startedAt = new Date().toISOString();
      const updated: RunRecord = {
        ...run,
        command: launch.command,
        cwd: launch.cwd,
        status: "running",
        startedAt
      };
      this.#setRun(updated);
      this.#processes.set(id, launch.process);
      this.events.append(id, "run.started", { pid: launch.process.pid ?? null, processKind: launch.process.kind });

      if (launch.structured === "claude-stream-json") {
        let lineBuffer = "";
        launch.process.onData((text) => {
          lineBuffer += text;
          const lines = lineBuffer.split("\n");
          lineBuffer = lines.pop() ?? "";
          for (const rawLine of lines) {
            const line = rawLine.replace(/\r$/, "");
            if (line.trim().length === 0) {
              continue;
            }
            const parsed = parseClaudeStreamLine(line.trim());
            if (parsed === null) {
              this.events.append(id, "pty.output", { stream: "pty", text: `${line}\n` });
              continue;
            }
            for (const event of parsed) {
              this.events.append(id, "agent.event", { kind: event.kind, label: event.label, detail: event.detail });
              this.events.append(id, "pty.output", { stream: "pty", text: event.text });
            }
          }
        });
      } else {
        launch.process.onData((text) => {
          this.#queueOutput(id, text);
        });
      }
      launch.process.onError((error) => {
        this.#flushOutput(id);
        this.#setRun({
          ...this.#runs.get(id)!,
          status: "failed",
          endedAt: new Date().toISOString()
        });
        this.events.append(id, "run.failed", { message: error.message });
      });
      launch.process.onExit(({ exitCode, signal }) => {
        this.#flushOutput(id);
        const previous = this.#runs.get(id)!;
        const status = previous.status === "terminated" ? "terminated" : "exited";
        this.#setRun({
          ...previous,
          status,
          endedAt: new Date().toISOString(),
          exitCode,
          signal
        });
        this.#processes.delete(id);
        this.#captureGit(id, "after", previous.cwd);
        this.events.append(id, "process.exited", { exitCode, signal });
        // archived: collapse output chunks in the store, release memory
        try {
          this.#store?.compactRunOutput(id);
        } catch {
          // best effort
        }
        this.events.evict(id);
      });

      return updated;
    } catch (error) {
      this.#setRun({
        ...run,
        status: "failed",
        endedAt: new Date().toISOString()
      });
      this.#captureGit(id, "after", run.cwd);
      this.events.append(id, "run.failed", { message: error instanceof Error ? error.message : String(error) });
      throw error;
    }
  }

  write(runId: string, input: string): void {
    const process = this.#processes.get(runId);

    if (!process) {
      throw new Error(`Run is not active: ${runId}`);
    }

    process.write(input);
    this.events.append(runId, "pty.input", { text: input });
  }

  resize(runId: string, cols: number, rows: number): void {
    const process = this.#processes.get(runId);

    if (!process) {
      throw new Error(`Run is not active: ${runId}`);
    }

    process.resize(cols, rows);
    this.events.append(runId, "pty.input", { resize: { cols, rows } });
  }

  terminate(runId: string): void {
    const process = this.#processes.get(runId);
    const run = this.#runs.get(runId);

    if (!process || !run) {
      throw new Error(`Run is not active: ${runId}`);
    }

    this.#setRun({
      ...run,
      status: "terminated",
      endedAt: new Date().toISOString()
    });
    process.kill("SIGTERM");
    this.events.append(runId, "run.terminated", {});
  }

  runDiff(runId: string, file?: string): string {
    const run = this.#runs.get(runId);

    if (!run) {
      throw new Error(`Unknown run: ${runId}`);
    }

    return diffText(run.cwd, file);
  }

  mergeRun(runId: string): RunRecord {
    const run = this.#requireIdleWorktreeRun(runId);
    const workspace = this.workspaceForCwd(run.worktree!);
    mergeWorktree(workspace?.path ?? run.worktree!, run.worktree!, runId);
    const { worktree: _worktree, ...rest } = run;
    const updated: RunRecord = rest;
    this.#setRun(updated);
    this.events.append(runId, "worktree.merged", { worktree: run.worktree });
    return updated;
  }

  discardRun(runId: string): RunRecord {
    const run = this.#requireIdleWorktreeRun(runId);
    const workspace = this.workspaceForCwd(run.worktree!);
    removeWorktree(workspace?.path ?? run.worktree!, run.worktree!, runId);
    const { worktree: _worktree, ...rest } = run;
    const updated: RunRecord = rest;
    this.#setRun(updated);
    this.events.append(runId, "worktree.discarded", { worktree: run.worktree });
    return updated;
  }

  #requireIdleWorktreeRun(runId: string): RunRecord {
    const run = this.#runs.get(runId);

    if (!run) {
      throw new Error(`Unknown run: ${runId}`);
    }

    if (!run.worktree) {
      throw new Error(`Run has no worktree: ${runId}`);
    }

    if (this.#processes.has(runId)) {
      throw new Error(`Run is still active: ${runId}`);
    }

    return run;
  }

  remove(runId: string): void {
    const run = this.#runs.get(runId);

    if (!run) {
      throw new Error(`Unknown run: ${runId}`);
    }

    if (this.#processes.has(runId)) {
      throw new Error(`Cannot remove a running run: ${runId}`);
    }

    if (run.worktree) {
      const workspace = this.workspaceForCwd(run.worktree);
      removeWorktree(workspace?.path ?? run.worktree, run.worktree, runId);
    }

    for (const approval of this.#approvals.values()) {
      if (approval.runId === runId) {
        this.#approvals.delete(approval.id);
      }
    }
    this.#runs.delete(runId);
    this.events.drop(runId);
    this.#store?.deleteRun(runId);
  }

  // Coalesce PTY chunks into ~50ms batches so interactive sessions don't
  // write one store row (and one SSE event) per keystroke echo.
  #queueOutput(runId: string, text: string): void {
    const entry = this.#outputBuffers.get(runId) ?? { text: "", timer: null };
    entry.text += text;
    this.#outputBuffers.set(runId, entry);

    if (entry.text.length >= 16384) {
      this.#flushOutput(runId);
      return;
    }

    if (!entry.timer) {
      entry.timer = setTimeout(() => this.#flushOutput(runId), 50);
    }
  }

  #flushOutput(runId: string): void {
    const entry = this.#outputBuffers.get(runId);
    if (!entry) {
      return;
    }

    if (entry.timer) {
      clearTimeout(entry.timer);
    }
    this.#outputBuffers.delete(runId);

    if (entry.text.length > 0) {
      this.events.append(runId, "pty.output", { stream: "pty", text: entry.text });
    }
  }

  #setRun(run: RunRecord): void {
    this.#runs.set(run.id, run);
    this.#store?.upsertRun(run);
  }

  #captureGit(runId: string, phase: GitSnapshot["phase"], cwd: string): void {
    const snapshot = captureGitSnapshot({ runId, phase, cwd });
    this.#store?.upsertGitSnapshot(snapshot);
    this.events.append(runId, "git.snapshot", { phase, isRepo: snapshot.isRepo, statusText: snapshot.statusText });
  }
}

function resolveWorkspacePath(path: string): string {
  const expanded = path === "~" || path.startsWith("~/")
    ? `${processEnv.HOME ?? ""}${path.slice(1)}`
    : path;
  return resolve(expanded);
}

function normalizeStoredRun(run: RunRecord): RunRecord {
  if (run.status === "running" || run.status === "created") {
    return {
      ...run,
      status: "failed",
      endedAt: run.endedAt ?? new Date().toISOString()
    };
  }

  return run;
}
