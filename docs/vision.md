# MiCo Vision

MiCo, short for Mission Control, is a centralized control panel for agentic programming and operational workflows. It is built for people who already work in terminals, but need better visibility, coordination, safety, and recall as coding agents become long-running collaborators instead of one-off command invocations.

MiCo should feel like a terminal++: monospace, dense, fast, keyboard-first, and direct. The product should keep the power and transparency of the CLI while adding the supervision, authorization, auditability, and orchestration that terminal sessions do not provide on their own.

## Product Thesis

Agentic programming is moving from single interactive sessions to fleets of specialized agents working across repositories, tickets, branches, experiments, and deployment environments. The terminal is still the right primitive for many of these tools, but it is not enough as the control surface.

MiCo exists to answer five questions at all times:

1. What agents are running?
2. What are they doing?
3. What are they allowed to do?
4. What changed because of them?
5. What needs human attention?

The initial product should bias toward local-first power users: engineers running Codex, Claude Code, shell scripts, project-specific harnesses, and future CLI agents against local workspaces.

## Core Principles

### Terminal-Native, Not Terminal-Limited

MiCo should run real CLI tools through real process sessions. It should not hide the underlying agent behind a generic chat abstraction. The user should be able to see output, inspect commands, search logs, replay sessions, and drop into direct control when needed.

The UI can improve the terminal experience with panes, timelines, approval queues, structured metadata, diffs, and coordination tools, but the terminal remains a first-class object.

### Human Authority Is Central

Agents can propose, execute, and coordinate work, but MiCo owns the control plane. Permissions should be visible, adjustable, and scoped. Approval should work at multiple levels:

- Global defaults.
- Workspace policies.
- Harness-specific policies.
- Task-level overrides.
- One-time granular approvals.

The product should make it easy to be permissive in a scratch repo and strict in a production repo.

### Multiple Harnesses, One Control Surface

MiCo should not be tied to one agent vendor, CLI, model, or execution harness. Codex and Claude Code are first-class early targets, but the architecture should support any tool that can be launched, observed, and controlled through a defined adapter.

Harnesses may differ in capabilities. MiCo should normalize common lifecycle events while still preserving harness-specific detail.

### Everything Important Is Recorded

The value of MiCo compounds when runs are inspectable after the fact. A completed session should leave behind a useful record:

- Prompt or task objective.
- Agent and harness used.
- Commands run.
- Files read and modified when observable.
- Diffs and artifacts.
- Approval requests and responses.
- Errors, retries, and interruptions.
- Outcome and follow-up notes.

Audit history is not an enterprise add-on. It is core developer ergonomics.

### Coordination Before Autonomy

MiCo should help humans coordinate agents before it tries to be a fully autonomous manager. The first useful version is not an AI project manager. It is a high-trust operations console for launching, supervising, pausing, redirecting, and comparing agent work.

## Target Users

### Primary User: Agentic Engineer

An engineer who already uses CLI agents and wants a better way to manage several sessions across repositories. They care about speed, control, diffs, logs, and predictable permissions.

### Secondary User: Technical Lead

A lead coordinating multiple streams of work who wants visibility into agent activity, reviewable outputs, and a reliable way to gate risky actions.

### Future User: Automation Operator

A user who wants scheduled or event-driven workflows: update dependencies, triage issues, run migrations, generate reports, or execute repeatable project maintenance tasks.

## Core Workflows

### Start A Mission

The user selects a workspace, chooses a harness, writes or selects a task, configures policy scope, and starts a run.

The first version should support:

- Pick local workspace.
- Pick agent harness.
- Enter prompt or command.
- Choose permission profile.
- Start session in a live terminal pane.

### Supervise Live Work

The user watches output, switches between active runs, responds to approval requests, opens diffs, and pauses or terminates sessions.

The live view should answer:

- Is the run active, blocked, waiting for approval, failed, or complete?
- What was the last meaningful event?
- What changed so far?
- What action is being requested?

### Approve Or Deny Actions

When an agent requests something risky, MiCo should surface the request with context. The user should be able to approve once, approve for a scope, deny, or modify policy.

Early approval categories:

- Shell command execution.
- Filesystem write outside expected scope.
- Network access.
- Dependency installation.
- Git operations.
- Secret or credential access.
- Process termination or background service launch.

### Review Results

After a run completes, the user reviews the terminal transcript, timeline, final diff, artifacts, and any unresolved follow-ups.

Useful result states:

- Completed.
- Completed with warnings.
- Failed.
- Cancelled.
- Waiting for user.
- Superseded by another run.

### Coordinate Multiple Agents

The user can run several agents against related tasks and compare their progress. Coordination starts simple:

- Assign tasks to agents.
- Track dependencies.
- Label runs.
- Group sessions into a mission.
- Compare branches or diffs.
- Mark one run as accepted and others as discarded.

## Product Surfaces

### Mission Dashboard

The top-level operational view. It shows active missions, active runs, blocked approvals, recent completions, workspace status, and high-level health.

This should be dense and scannable, closer to a terminal multiplexer plus process monitor than a marketing-style dashboard.

### Terminal Workspace

The primary work surface. It should support multiple panes:

- Live terminal.
- Event timeline.
- Approval queue.
- File tree or changed-files list.
- Diff viewer.
- Notes or task objective.

The terminal pane should remain central, not decorative.

### Permissions Center

The place to configure and inspect policy:

- Permission profiles.
- Workspace rules.
- Harness capabilities.
- Approval history.
- Temporary grants.
- Deny lists.

This should make the current safety posture obvious without requiring the user to read raw config.

### Run History

A searchable archive of past sessions and outcomes. It should support filtering by workspace, agent, harness, status, changed file, command, approval, and time.

### Harness Registry

A lightweight registry of available tools MiCo can launch. Early entries can be local adapter definitions for Codex, Claude Code, shell, and custom commands.

## MVP

The first useful version of MiCo should do less, but do it honestly.

### MVP Goals

1. Launch and supervise local CLI agent sessions.
2. Support Codex, Claude Code, and generic shell command harnesses through adapters.
3. Render live PTY output in a terminal-like UI.
4. Track session state and persist logs.
5. Capture basic run metadata: workspace, harness, command, prompt, start time, end time, status.
6. Provide an approval queue for high-risk actions that MiCo can observe or mediate.
7. Track changed files and show diffs for git workspaces.
8. Provide workspace-level permission profiles.
9. Let the user pause, resume when supported, interrupt, or terminate sessions.

### MVP Non-Goals

- Cloud-hosted multi-user teams.
- Enterprise RBAC.
- Fully autonomous task planning.
- Deep model-provider abstraction.
- Browser automation management.
- Remote worker fleets.
- Payment, billing, or organization administration.
- Perfect semantic understanding of every harness transcript.

## Design Direction

MiCo should look and feel like a professional terminal operations console:

- Monospace throughout.
- Compact spacing.
- High information density.
- Keyboard-first navigation.
- Split panes and resizable panels.
- Command palette.
- Clear status colors used sparingly.
- No decorative cards inside cards.
- No marketing hero layout as the main experience.

Visual references are closer to:

- Terminal multiplexers.
- Process monitors.
- Database consoles.
- Observability timelines.
- Git diff tools.
- Command palettes.

The first screen should be the actual control panel, not a landing page.

## Product Risks

### Harness Differences

Different agents expose different control and permission models. MiCo should avoid pretending every tool has the same capabilities. The adapter model needs capability detection and graceful degradation.

### Permission Blind Spots

Some CLIs may perform work MiCo cannot fully observe unless they run inside a controlled supervisor. The product should distinguish between actions it mediates, actions it observes, and actions it cannot see.

### UI Complexity

The product can become overwhelming if every event, log, and policy appears at the same level. The UI needs progressive disclosure while preserving terminal-grade detail.

### Trust

MiCo becomes a trusted control surface. It must be conservative with credentials, explicit with approvals, and clear about what is running.

## Success Criteria

MiCo is succeeding when a power user prefers it over manually opening several terminal tabs because it gives them:

- Faster switching between agent sessions.
- Better awareness of blocked work.
- Safer permission handling.
- Easier review of agent changes.
- Searchable run history.
- Confidence running more than one agent at a time.

## Open Questions

1. Should the first app be web-based local UI, desktop app, or terminal UI?
2. Should MiCo wrap existing CLIs directly, or require adapters to expose structured hooks?
3. How strict should the initial sandbox be?
4. Which approval events can be mediated reliably across Codex and Claude Code?
5. Should MiCo own git branch creation for each run?
6. Should runs be isolated by worktree, container, or just workspace policy in the MVP?
7. What is the minimum useful plugin API for custom harnesses?
