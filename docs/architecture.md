# MiCo Architecture

MiCo is a local-first control plane for running, supervising, authorizing, and auditing agentic programming workflows. The architecture should keep CLI agents as real processes while adding a structured supervisor layer around them.

The initial system is composed of:

- A user interface for mission control.
- A supervisor daemon for process, policy, and state management.
- Harness adapters for launching and observing agents.
- A persistent event store.
- A policy engine for authorization decisions.

## Architecture Goals

1. Run real CLI tools in PTY-backed sessions.
2. Support multiple harnesses without hard-coding one agent model.
3. Persist enough state to inspect and replay runs.
4. Gate risky actions through a visible policy and approval model.
5. Keep local workspaces and credentials under user control.
6. Allow incremental hardening: start local, add stronger isolation later.

## High-Level Components

```text
+-------------------+        websocket/events        +---------------------+
| MiCo UI           | <-----------------------------> | Supervisor Daemon   |
|                   |                                |                     |
| - dashboard       |                                | - session manager   |
| - terminal panes  |                                | - PTY manager       |
| - approvals       |                                | - policy engine     |
| - diffs/history   |                                | - adapter runtime   |
+-------------------+                                | - event recorder    |
                                                     +----------+----------+
                                                                |
                                                                | launch/control
                                                                v
                                                     +---------------------+
                                                     | Harness Adapters    |
                                                     |                     |
                                                     | - codex             |
                                                     | - claude-code       |
                                                     | - shell             |
                                                     | - custom            |
                                                     +----------+----------+
                                                                |
                                                                | process/PTY
                                                                v
                                                     +---------------------+
                                                     | Local Workspace     |
                                                     |                     |
                                                     | - repo files        |
                                                     | - git state         |
                                                     | - tools/env         |
                                                     +---------------------+
```

## Process Model

The supervisor daemon owns all launched sessions. The UI should not spawn agents directly.

Each run has:

- A workspace path.
- A harness adapter.
- A command or structured launch request.
- A PTY session.
- A permission profile.
- An event stream.
- A persisted run record.

The supervisor should expose a small API for:

- Starting a run.
- Sending input to a PTY.
- Streaming output.
- Interrupting or terminating a run.
- Creating approval requests.
- Recording approval decisions.
- Reporting status and exit codes.

## UI Layer

The UI is an operational control surface. A local web app is the recommended first implementation because it gives strong terminal rendering, pane management, and fast iteration without committing to desktop packaging too early.

Recommended stack:

- React or equivalent component UI.
- xterm.js for terminal panes.
- WebSocket or Server-Sent Events for live streams.
- HTTP API for commands and state queries.
- CSS variables for terminal themes.

The UI should not contain business-critical policy logic. It presents state and sends user decisions; the supervisor enforces them.

## Supervisor Daemon

The supervisor is the core of MiCo. It is responsible for turning user intent into controlled local execution.

Responsibilities:

- Manage process lifecycle.
- Allocate and multiplex PTYs.
- Load harness adapters.
- Apply policy before sensitive actions when MiCo can mediate them.
- Persist events and state.
- Inspect git state before and after runs.
- Track changed files and diffs.
- Maintain active approval requests.
- Expose a local API to the UI.

The supervisor should be designed as a long-running local process, but the first version can be started with the app.

## Harness Adapter Model

A harness adapter describes how MiCo launches, observes, and controls a tool.

Initial adapters:

- `codex`: launches Codex CLI sessions.
- `claude-code`: launches Claude Code sessions.
- `shell`: launches arbitrary commands.
- `custom`: user-defined command templates.

Adapters should expose capabilities, because not all harnesses can support the same operations.

Example capability fields:

```json
{
  "id": "codex",
  "displayName": "Codex",
  "supportsPty": true,
  "supportsStructuredEvents": false,
  "supportsPause": false,
  "supportsResume": false,
  "supportsPromptInjection": true,
  "supportsApprovalHooks": "partial",
  "supportsDiffTracking": "workspace"
}
```

Adapter responsibilities:

- Build the launch command.
- Prepare environment variables.
- Define working directory behavior.
- Parse known output patterns when useful.
- Report capability metadata.
- Optionally translate harness-specific events into MiCo events.

The adapter should not own global policy. It can request actions; the supervisor decides.

## Event Model

MiCo should use an append-only event stream for each run. State can be projected from events, while raw history remains inspectable.

Core event types:

- `run.created`
- `run.started`
- `run.status_changed`
- `pty.output`
- `pty.input`
- `process.spawned`
- `process.exited`
- `approval.requested`
- `approval.granted`
- `approval.denied`
- `policy.changed`
- `git.snapshot`
- `git.diff_recorded`
- `artifact.created`
- `run.note_added`
- `run.completed`
- `run.failed`
- `run.cancelled`

Example event:

```json
{
  "id": "evt_01",
  "runId": "run_01",
  "type": "approval.requested",
  "timestamp": "2026-07-08T18:00:00Z",
  "actor": "harness:codex",
  "payload": {
    "category": "shell.command",
    "command": "npm install",
    "risk": "medium",
    "scope": "workspace",
    "reason": "Install project dependencies"
  }
}
```

## State Store

SQLite is the recommended initial store. It is local, inspectable, portable, and enough for the MVP.

Suggested tables:

- `workspaces`
- `harnesses`
- `missions`
- `tasks`
- `runs`
- `events`
- `approvals`
- `policies`
- `artifacts`
- `git_snapshots`
- `settings`

The event table should preserve raw event payloads as JSON. Derived tables can make common UI queries fast.

## Core Entities

### Workspace

A local project directory MiCo can run tools against.

Fields:

- `id`
- `name`
- `path`
- `trust_level`
- `default_policy_id`
- `created_at`
- `last_opened_at`

### Harness

A launchable agent or command environment.

Fields:

- `id`
- `display_name`
- `adapter_type`
- `command_template`
- `capabilities`
- `enabled`

### Mission

A group of related tasks and runs.

Fields:

- `id`
- `title`
- `workspace_id`
- `status`
- `created_at`
- `completed_at`

### Task

A unit of work that can be assigned to one or more runs.

Fields:

- `id`
- `mission_id`
- `title`
- `objective`
- `status`
- `parent_task_id`

### Run

One execution of one harness against one task or prompt.

Fields:

- `id`
- `workspace_id`
- `mission_id`
- `task_id`
- `harness_id`
- `status`
- `launch_command`
- `permission_profile_id`
- `started_at`
- `ended_at`
- `exit_code`

### Approval

A human authorization decision.

Fields:

- `id`
- `run_id`
- `category`
- `requested_scope`
- `payload`
- `status`
- `decision`
- `decided_by`
- `created_at`
- `decided_at`

## Policy Engine

The policy engine evaluates requested actions against configured rules.

Decision outcomes:

- `allow`
- `deny`
- `ask`
- `allow_once`
- `allow_for_run`
- `allow_for_workspace`

Policy dimensions:

- Workspace.
- Harness.
- Task.
- Command category.
- Path scope.
- Network scope.
- Git operation.
- Credential scope.
- Risk level.

Initial policy profiles:

- `open`: low friction, mostly allow within workspace.
- `balanced`: ask for writes outside workspace, installs, network, and git push.
- `strict`: ask for most writes and commands, deny unknown network and credential access.
- `readonly`: deny writes and side effects.

Important distinction: MiCo should label whether a decision is enforced, observed, or advisory. If a harness can bypass a control, the UI should not imply enforcement.

## Permission Mediation

The strongest model is to run agents inside a controlled execution environment where filesystem, network, process, and credential access can be mediated. That can come later.

MVP mediation should focus on controls MiCo can reliably provide:

- Launch command approval.
- Workspace path selection.
- Environment variable scoping.
- Git diff tracking.
- Process lifecycle control.
- Explicit user approvals surfaced by compatible harnesses.
- Blocking MiCo-initiated actions before they execute.

Future stronger controls:

- Containerized per-run sandboxes.
- Filesystem allow lists.
- Network allow lists.
- Secret broker.
- Command proxy.
- Per-run git worktrees.

## Git Integration

Git is a central observability layer for coding agents.

MVP git behavior:

- Detect whether workspace is a git repo.
- Capture status before run.
- Capture status after run.
- Record changed files.
- Store final diff.
- Link diff to run.

Possible later behavior:

- Create per-run branches.
- Create per-run worktrees.
- Compare agent attempts.
- Promote accepted run.
- Discard or archive rejected run.
- Open PRs.

MiCo should never silently discard user changes.

## Isolation Strategy

The MVP can start with local process isolation only, but the architecture should leave room for stricter execution.

Isolation levels:

1. `none`: run directly in workspace.
2. `workspace`: constrain MiCo-managed operations to workspace.
3. `worktree`: run in a dedicated git worktree.
4. `container`: run in a container with mounted workspace.
5. `sandbox`: enforce filesystem and network policy.

The UI should show the active isolation level for every run.

## Local API Sketch

The supervisor can expose a local HTTP and WebSocket API.

Example endpoints:

```text
GET    /api/workspaces
POST   /api/workspaces
GET    /api/harnesses
GET    /api/missions
POST   /api/missions
GET    /api/runs
POST   /api/runs
GET    /api/runs/:id
POST   /api/runs/:id/input
POST   /api/runs/:id/interrupt
POST   /api/runs/:id/terminate
GET    /api/runs/:id/events
GET    /api/runs/:id/diff
GET    /api/approvals
POST   /api/approvals/:id/decision
GET    /api/policies
PUT    /api/policies/:id
```

Live streams:

```text
WS /api/stream
WS /api/runs/:id/pty
WS /api/runs/:id/events
```

## Adapter API Sketch

Adapters can start as local modules loaded by the supervisor.

```ts
interface HarnessAdapter {
  id: string;
  displayName: string;
  capabilities(): HarnessCapabilities;
  buildLaunch(request: LaunchRequest): LaunchSpec;
  parseOutput?(chunk: string, context: RunContext): AdapterEvent[];
  onExit?(result: ProcessExit, context: RunContext): AdapterEvent[];
}

interface LaunchRequest {
  workspacePath: string;
  prompt?: string;
  command?: string;
  args?: string[];
  env?: Record<string, string>;
  policyProfileId: string;
}

interface LaunchSpec {
  command: string;
  args: string[];
  cwd: string;
  env: Record<string, string>;
  pty: boolean;
}
```

Custom adapters can later be distributed as plugins with a manifest:

```json
{
  "id": "my-agent",
  "displayName": "My Agent",
  "command": "my-agent",
  "args": ["--workspace", "{{workspace}}"],
  "supportsPty": true
}
```

## Logging And Replay

MiCo should store terminal output in a way that supports:

- Exact replay when possible.
- Search.
- Timestamped event correlation.
- Truncation or compaction for very long runs.

Raw PTY output can be stored as chunked events initially. Later, MiCo can add compressed transcript storage and derived searchable text.

## Security Model

MiCo should assume agents and harnesses may make mistakes. It should also assume local machines contain sensitive data.

Security rules:

- Do not pass all environment variables by default.
- Never display or persist raw secrets unless explicitly configured.
- Keep credential access scoped and auditable.
- Make policy decisions explicit.
- Prefer deny or ask for unknown sensitive actions.
- Clearly show what MiCo can enforce versus only observe.

Local API security:

- Bind to localhost by default.
- Use an unguessable session token for UI-to-daemon requests.
- Avoid exposing the daemon to the network in the MVP.

## MVP Implementation Plan

### Phase 1: Skeleton

- Create local app shell.
- Create supervisor process.
- Add SQLite store.
- Add `shell` harness adapter.
- Launch PTY session and stream output to UI.

### Phase 2: Agent Adapters

- Add Codex adapter.
- Add Claude Code adapter.
- Normalize run statuses.
- Persist transcripts and run metadata.

### Phase 3: Git Tracking

- Capture pre-run git status.
- Capture post-run git status.
- Store changed files and final diff.
- Show diff in UI.

### Phase 4: Approvals

- Add policy profiles.
- Add approval request model.
- Add approval queue UI.
- Gate MiCo-mediated actions.

### Phase 5: Missions

- Add missions and tasks.
- Group runs.
- Add dashboard view.
- Add run history search.

## Technology Defaults

These are initial defaults, not permanent commitments:

- UI: TypeScript, React, xterm.js.
- Supervisor: TypeScript/Node.js or Rust.
- Store: SQLite.
- Transport: local HTTP plus WebSocket.
- Terminal: PTY library appropriate to supervisor language.
- Diff: git CLI integration first.

Node.js may be faster for an MVP because the UI, server, and adapter layer can share TypeScript types. Rust may be better later for a hardened supervisor. A pragmatic path is TypeScript first, with a clean supervisor boundary that can be replaced or split later.

## Open Architecture Questions

1. Should the supervisor be embedded in the app process or run as a separate daemon?
2. Should the first UI be local web, desktop shell, or terminal UI?
3. Should every run get its own git worktree by default?
4. How should MiCo represent actions requested by harnesses that do not emit structured events?
5. What is the minimum viable sandbox that is useful without breaking common developer workflows?
6. How should credentials be brokered across agents?
7. Should plugin adapters be trusted code, declarative manifests, or both?
