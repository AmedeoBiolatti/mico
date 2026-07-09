import type { Terminal } from "@xterm/xterm";
import type { FitAddon } from "@xterm/addon-fit";

export type SupervisorRun = {
  id: string;
  harnessId: string;
  command: string;
  cwd: string;
  status: string;
  createdAt: string;
  startedAt?: string;
  endedAt?: string;
  exitCode?: number | null;
  signal?: string | null;
  worktree?: string;
  title?: string;
};

export type SupervisorEvent = {
  id: string;
  runId: string;
  type: string;
  timestamp: string;
  payload: Record<string, unknown>;
};

export type RunGitDetails = {
  changedFiles: string[];
};

export type PolicyId = "permissive" | "balanced" | "strict";

export type Workspace = {
  id: string;
  name: string;
  path: string;
  policy: PolicyId;
};

export type HarnessDescriptor = {
  id: string;
  displayName: string;
  command?: string;
  available: boolean;
};

export type Approval = {
  id: string;
  runId: string;
  toolName: string;
  input: unknown;
  state: "pending" | "allowed" | "denied";
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

export type SessionMeta = {
  status: string;
  exitCode: number | null;
  lastOutputAt: number;
  acknowledged: boolean;
};

export type Session = {
  runId: string;
  harnessId: string;
  terminal: Terminal;
  fit: FitAddon;
  host: HTMLDivElement;
  events: EventSource | null;
  lastEventNum: number;
  lastAgentNum: number;
  opened: boolean;
  /** Output received before the terminal is opened+fitted; parsed only once the real size is known. */
  pendingOutput: string[] | null;
};

export type ActivityEntry = {
  id: string;
  kind: string;
  label: string;
  detail: string | null;
};

export type Attention = "working" | "waiting" | "done" | "failed" | null;
