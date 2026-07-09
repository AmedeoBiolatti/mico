export type HarnessId = "shell" | "codex" | "claude-code" | string;

export type RunStatus = "created" | "running" | "exited" | "failed" | "terminated";

export type RunEventType =
  | "run.created"
  | "run.started"
  | "pty.output"
  | "pty.input"
  | "process.exited"
  | "git.snapshot"
  | "run.failed"
  | "run.terminated"
  | "approval.requested"
  | "approval.decided"
  | "agent.event"
  | "worktree.merged"
  | "worktree.discarded";

export type ApprovalState = "pending" | "allowed" | "denied";

export type ApprovalRecord = {
  id: string;
  runId: string;
  toolName: string;
  input: unknown;
  state: ApprovalState;
  auto: boolean;
  via?: "policy" | "rule";
  requestedAt: string;
  decidedAt?: string;
};

export type ApprovalRule = {
  id: string;
  workspaceId: string | null;
  toolName: string;
  commandPrefix: string | null;
  createdAt: string;
};

export type RunEvent = {
  id: string;
  runId: string;
  type: RunEventType;
  timestamp: string;
  payload: Record<string, unknown>;
};

export type PolicyId = "permissive" | "balanced" | "strict";

export type WorkspaceRecord = {
  id: string;
  name: string;
  path: string;
  policy: PolicyId;
  createdAt: string;
};

export type AddWorkspaceRequest = {
  name?: string;
  path: string;
  policy?: PolicyId;
};

export type UpdateWorkspaceRequest = {
  name?: string;
  policy?: PolicyId;
};

export type RunRecord = {
  id: string;
  harnessId: HarnessId;
  command: string;
  cwd: string;
  status: RunStatus;
  createdAt: string;
  startedAt?: string;
  endedAt?: string;
  exitCode?: number | null;
  signal?: NodeJS.Signals | null;
  worktree?: string;
};


export type GitSnapshot = {
  id: string;
  runId: string;
  phase: "before" | "after";
  cwd: string;
  isRepo: boolean;
  statusText: string;
  createdAt: string;
};

export type RunGitDetails = {
  before: GitSnapshot | null;
  after: GitSnapshot | null;
  changedFiles: string[];
};

export type StartRunRequest = {
  harnessId?: HarnessId;
  command: string;
  cwd?: string;
  env?: Record<string, string>;
  cols?: number;
  rows?: number;
  isolate?: boolean;
  policy?: PolicyId;
  runId?: string;
};

export type StartRunResponse = {
  run: RunRecord;
};

export type WriteRunInputRequest = {
  input: string;
};

export type ResizeRunRequest = {
  cols: number;
  rows: number;
};

export type HarnessDescriptor = {
  id: HarnessId;
  displayName: string;
  command?: string;
  available: boolean;
  supportsInput: boolean;
  supportsResize: boolean;
  supportsPty: boolean;
};
