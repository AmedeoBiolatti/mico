import { existsSync, mkdirSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, resolve } from "node:path";
import { cwd as processCwd, env as processEnv } from "node:process";
import { DatabaseSync } from "node:sqlite";
import type { ApprovalRecord, ApprovalRule, GitSnapshot, RunEvent, RunRecord, WorkspaceRecord } from "./types.js";

export type RunStoreOptions = {
  path?: string;
};

type RunRow = {
  id: string;
  harness_id: string;
  command: string;
  cwd: string;
  status: RunRecord["status"];
  created_at: string;
  started_at: string | null;
  ended_at: string | null;
  exit_code: number | null;
  signal: string | null;
  worktree: string | null;
};

type EventRow = {
  id: string;
  run_id: string;
  type: RunEvent["type"];
  timestamp: string;
  payload_json: string;
};

type GitSnapshotRow = {
  id: string;
  run_id: string;
  phase: GitSnapshot["phase"];
  cwd: string;
  is_repo: 0 | 1;
  status_text: string;
  created_at: string;
};

export class RunStore {
  readonly path: string;
  readonly #db: DatabaseSync;

  constructor(options: RunStoreOptions = {}) {
    this.path = resolve(options.path ?? processEnv.MICO_DB ?? defaultStorePath());
    mkdirSync(dirname(this.path), { recursive: true });
    this.#db = new DatabaseSync(this.path);
    this.#db.exec("PRAGMA journal_mode = WAL");
    this.#db.exec("PRAGMA foreign_keys = ON");
    this.#db.exec(`
      CREATE TABLE IF NOT EXISTS runs (
        id TEXT PRIMARY KEY,
        harness_id TEXT NOT NULL,
        command TEXT NOT NULL,
        cwd TEXT NOT NULL,
        status TEXT NOT NULL,
        created_at TEXT NOT NULL,
        started_at TEXT,
        ended_at TEXT,
        exit_code INTEGER,
        signal TEXT
      );

      CREATE TABLE IF NOT EXISTS events (
        id TEXT PRIMARY KEY,
        run_id TEXT NOT NULL,
        type TEXT NOT NULL,
        timestamp TEXT NOT NULL,
        payload_json TEXT NOT NULL,
        FOREIGN KEY (run_id) REFERENCES runs(id) ON DELETE CASCADE
      );

      CREATE TABLE IF NOT EXISTS git_snapshots (
        id TEXT PRIMARY KEY,
        run_id TEXT NOT NULL,
        phase TEXT NOT NULL,
        cwd TEXT NOT NULL,
        is_repo INTEGER NOT NULL,
        status_text TEXT NOT NULL,
        created_at TEXT NOT NULL,
        UNIQUE(run_id, phase),
        FOREIGN KEY (run_id) REFERENCES runs(id) ON DELETE CASCADE
      );

      CREATE TABLE IF NOT EXISTS workspaces (
        id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        path TEXT NOT NULL UNIQUE,
        policy TEXT NOT NULL DEFAULT 'balanced',
        created_at TEXT NOT NULL
      );

      CREATE TABLE IF NOT EXISTS approvals (
        id TEXT PRIMARY KEY,
        run_id TEXT NOT NULL,
        tool_name TEXT NOT NULL,
        input_json TEXT NOT NULL,
        state TEXT NOT NULL,
        auto INTEGER NOT NULL DEFAULT 0,
        requested_at TEXT NOT NULL,
        decided_at TEXT
      );

      CREATE TABLE IF NOT EXISTS approval_rules (
        id TEXT PRIMARY KEY,
        workspace_id TEXT,
        tool_name TEXT NOT NULL,
        command_prefix TEXT,
        created_at TEXT NOT NULL
      );

      CREATE INDEX IF NOT EXISTS idx_runs_created_at ON runs(created_at);
      CREATE INDEX IF NOT EXISTS idx_approvals_run_id ON approvals(run_id);
      CREATE INDEX IF NOT EXISTS idx_events_run_id_timestamp ON events(run_id, timestamp);
      CREATE INDEX IF NOT EXISTS idx_git_snapshots_run_id_phase ON git_snapshots(run_id, phase);
    `);
    this.#ensureColumn("workspaces", "policy", "TEXT NOT NULL DEFAULT 'balanced'");
    this.#ensureColumn("runs", "worktree", "TEXT");
    this.#ensureColumn("approvals", "via", "TEXT");
  }

  #ensureColumn(table: string, column: string, definition: string): void {
    try {
      this.#db.exec(`ALTER TABLE ${table} ADD COLUMN ${column} ${definition}`);
    } catch {
      // column already exists
    }
  }

  listRuns(): RunRecord[] {
    const rows = this.#db.prepare("SELECT * FROM runs ORDER BY created_at ASC").all() as RunRow[];
    return rows.map(runFromRow);
  }

  listEvents(runId: string): RunEvent[] {
    // ids are evt_<n>; order numerically, not lexically (evt_10 must follow evt_9)
    const rows = this.#db.prepare(
      "SELECT * FROM events WHERE run_id = ? ORDER BY CAST(SUBSTR(id, 5) AS INTEGER) ASC"
    ).all(runId) as EventRow[];
    return rows.map(eventFromRow);
  }

  /**
   * Collapse a finished run's pty.output chunk rows into a single transcript
   * row so archived runs stay cheap to store and replay.
   */
  compactRunOutput(runId: string): void {
    const rows = this.#db.prepare(
      "SELECT * FROM events WHERE run_id = ? AND type = 'pty.output' ORDER BY CAST(SUBSTR(id, 5) AS INTEGER) ASC"
    ).all(runId) as EventRow[];

    if (rows.length <= 1) {
      return;
    }

    const text = rows
      .map((row) => {
        const payload = JSON.parse(row.payload_json) as { text?: unknown };
        return typeof payload.text === "string" ? payload.text : "";
      })
      .join("");

    const first = rows[0]!;
    this.#db.exec("BEGIN");
    try {
      this.#db.prepare("DELETE FROM events WHERE run_id = ? AND type = 'pty.output'").run(runId);
      this.#db.prepare(`
        INSERT INTO events (id, run_id, type, timestamp, payload_json)
        VALUES (?, ?, ?, ?, ?)
      `).run(first.id, runId, "pty.output", first.timestamp, JSON.stringify({ stream: "pty", text, compacted: true }));
      this.#db.exec("COMMIT");
    } catch (error) {
      this.#db.exec("ROLLBACK");
      throw error;
    }
  }

  listGitSnapshots(runId: string): GitSnapshot[] {
    const rows = this.#db.prepare("SELECT * FROM git_snapshots WHERE run_id = ? ORDER BY created_at ASC").all(runId) as GitSnapshotRow[];
    return rows.map(gitSnapshotFromRow);
  }

  upsertRun(run: RunRecord): void {
    this.#db.prepare(`
      INSERT INTO runs (id, harness_id, command, cwd, status, created_at, started_at, ended_at, exit_code, signal, worktree)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(id) DO UPDATE SET
        harness_id = excluded.harness_id,
        command = excluded.command,
        cwd = excluded.cwd,
        status = excluded.status,
        created_at = excluded.created_at,
        started_at = excluded.started_at,
        ended_at = excluded.ended_at,
        exit_code = excluded.exit_code,
        signal = excluded.signal,
        worktree = excluded.worktree
    `).run(
      run.id,
      run.harnessId,
      run.command,
      run.cwd,
      run.status,
      run.createdAt,
      run.startedAt ?? null,
      run.endedAt ?? null,
      run.exitCode ?? null,
      run.signal ?? null,
      run.worktree ?? null
    );
  }

  appendEvent(event: RunEvent): void {
    this.#db.prepare(`
      INSERT OR IGNORE INTO events (id, run_id, type, timestamp, payload_json)
      VALUES (?, ?, ?, ?, ?)
    `).run(event.id, event.runId, event.type, event.timestamp, JSON.stringify(event.payload));
  }

  upsertGitSnapshot(snapshot: GitSnapshot): void {
    this.#db.prepare(`
      INSERT INTO git_snapshots (id, run_id, phase, cwd, is_repo, status_text, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(run_id, phase) DO UPDATE SET
        cwd = excluded.cwd,
        is_repo = excluded.is_repo,
        status_text = excluded.status_text,
        created_at = excluded.created_at
    `).run(
      snapshot.id,
      snapshot.runId,
      snapshot.phase,
      snapshot.cwd,
      snapshot.isRepo ? 1 : 0,
      snapshot.statusText,
      snapshot.createdAt
    );
  }

  listWorkspaces(): WorkspaceRecord[] {
    const rows = this.#db.prepare("SELECT * FROM workspaces ORDER BY created_at ASC").all() as {
      id: string;
      name: string;
      path: string;
      policy: string | null;
      created_at: string;
    }[];
    return rows.map((row) => ({
      id: row.id,
      name: row.name,
      path: row.path,
      policy: (row.policy ?? "balanced") as WorkspaceRecord["policy"],
      createdAt: row.created_at
    }));
  }

  upsertWorkspace(workspace: WorkspaceRecord): void {
    this.#db.prepare(`
      INSERT INTO workspaces (id, name, path, policy, created_at)
      VALUES (?, ?, ?, ?, ?)
      ON CONFLICT(id) DO UPDATE SET
        name = excluded.name,
        path = excluded.path,
        policy = excluded.policy,
        created_at = excluded.created_at
    `).run(workspace.id, workspace.name, workspace.path, workspace.policy, workspace.createdAt);
  }

  deleteWorkspace(workspaceId: string): void {
    this.#db.prepare("DELETE FROM workspaces WHERE id = ?").run(workspaceId);
  }

  listApprovals(): ApprovalRecord[] {
    const rows = this.#db.prepare("SELECT * FROM approvals ORDER BY requested_at ASC").all() as {
      id: string;
      run_id: string;
      tool_name: string;
      input_json: string;
      state: string;
      auto: number;
      via: string | null;
      requested_at: string;
      decided_at: string | null;
    }[];
    return rows.map((row) => ({
      id: row.id,
      runId: row.run_id,
      toolName: row.tool_name,
      input: JSON.parse(row.input_json) as unknown,
      state: row.state as ApprovalRecord["state"],
      auto: row.auto === 1,
      ...(row.via !== null ? { via: row.via as "policy" | "rule" } : {}),
      requestedAt: row.requested_at,
      ...(row.decided_at !== null ? { decidedAt: row.decided_at } : {})
    }));
  }

  upsertApproval(approval: ApprovalRecord): void {
    this.#db.prepare(`
      INSERT INTO approvals (id, run_id, tool_name, input_json, state, auto, via, requested_at, decided_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(id) DO UPDATE SET
        state = excluded.state,
        auto = excluded.auto,
        via = excluded.via,
        decided_at = excluded.decided_at
    `).run(
      approval.id,
      approval.runId,
      approval.toolName,
      JSON.stringify(approval.input ?? null),
      approval.state,
      approval.auto ? 1 : 0,
      approval.via ?? null,
      approval.requestedAt,
      approval.decidedAt ?? null
    );
  }

  listApprovalRules(): ApprovalRule[] {
    const rows = this.#db.prepare("SELECT * FROM approval_rules ORDER BY created_at ASC").all() as {
      id: string;
      workspace_id: string | null;
      tool_name: string;
      command_prefix: string | null;
      created_at: string;
    }[];
    return rows.map((row) => ({
      id: row.id,
      workspaceId: row.workspace_id,
      toolName: row.tool_name,
      commandPrefix: row.command_prefix,
      createdAt: row.created_at
    }));
  }

  upsertApprovalRule(rule: ApprovalRule): void {
    this.#db.prepare(`
      INSERT INTO approval_rules (id, workspace_id, tool_name, command_prefix, created_at)
      VALUES (?, ?, ?, ?, ?)
      ON CONFLICT(id) DO UPDATE SET
        workspace_id = excluded.workspace_id,
        tool_name = excluded.tool_name,
        command_prefix = excluded.command_prefix
    `).run(rule.id, rule.workspaceId, rule.toolName, rule.commandPrefix, rule.createdAt);
  }

  deleteApprovalRule(ruleId: string): void {
    this.#db.prepare("DELETE FROM approval_rules WHERE id = ?").run(ruleId);
  }

  nextApprovalRuleNumber(): number {
    const rows = this.#db.prepare("SELECT id FROM approval_rules").all() as { id: string }[];
    return maxNumericSuffix(rows.map((row) => row.id), "rule_") + 1;
  }

  nextApprovalNumber(): number {
    const rows = this.#db.prepare("SELECT id FROM approvals").all() as { id: string }[];
    return maxNumericSuffix(rows.map((row) => row.id), "ap_") + 1;
  }

  nextWorkspaceNumber(): number {
    const rows = this.#db.prepare("SELECT id FROM workspaces").all() as { id: string }[];
    return maxNumericSuffix(rows.map((row) => row.id), "ws_") + 1;
  }

  deleteRun(runId: string): void {
    this.#db.prepare("DELETE FROM approvals WHERE run_id = ?").run(runId);
    this.#db.prepare("DELETE FROM runs WHERE id = ?").run(runId);
  }

  nextRunNumber(): number {
    const rows = this.listRuns();
    return maxNumericSuffix(rows.map((run) => run.id), "run_") + 1;
  }

  nextEventNumber(): number {
    const rows = this.#db.prepare("SELECT id FROM events").all() as { id: string }[];
    return maxNumericSuffix(rows.map((row) => row.id), "evt_") + 1;
  }
}

function defaultStorePath(): string {
  // keep using a repo-local store when one already exists; otherwise go global
  const local = resolve(`${processCwd()}/.mico/supervisor.sqlite`);
  if (existsSync(local)) {
    return local;
  }

  return resolve(`${homedir()}/.mico/supervisor.sqlite`);
}

function runFromRow(row: RunRow): RunRecord {
  const run: RunRecord = {
    id: row.id,
    harnessId: row.harness_id,
    command: row.command,
    cwd: row.cwd,
    status: row.status,
    createdAt: row.created_at
  };

  if (row.started_at !== null) {
    run.startedAt = row.started_at;
  }

  if (row.ended_at !== null) {
    run.endedAt = row.ended_at;
  }

  if (row.exit_code !== null) {
    run.exitCode = row.exit_code;
  }

  if (row.signal !== null) {
    run.signal = row.signal as NodeJS.Signals;
  }

  if (row.worktree !== null) {
    run.worktree = row.worktree;
  }

  return run;
}

function eventFromRow(row: EventRow): RunEvent {
  return {
    id: row.id,
    runId: row.run_id,
    type: row.type,
    timestamp: row.timestamp,
    payload: JSON.parse(row.payload_json) as Record<string, unknown>
  };
}

function gitSnapshotFromRow(row: GitSnapshotRow): GitSnapshot {
  return {
    id: row.id,
    runId: row.run_id,
    phase: row.phase,
    cwd: row.cwd,
    isRepo: row.is_repo === 1,
    statusText: row.status_text,
    createdAt: row.created_at
  };
}

function maxNumericSuffix(ids: string[], prefix: string): number {
  let max = 0;

  for (const id of ids) {
    if (!id.startsWith(prefix)) {
      continue;
    }

    const value = Number.parseInt(id.slice(prefix.length), 10);
    if (Number.isFinite(value)) {
      max = Math.max(max, value);
    }
  }

  return max;
}
