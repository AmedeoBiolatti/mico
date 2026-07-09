# MiCo — Mission Control

MiCo is a terminal-native control panel for agentic programming workflows: run Codex,
Claude Code, and shell sessions across workspaces, watch what agents are doing, approve
what they're allowed to do, and review what changed — all from one monospace cockpit.

See [docs/vision.md](docs/vision.md) for the product thesis and
[docs/architecture.md](docs/architecture.md) for the technical layout.

## Layout

- `packages/render-core` — monospace rendering primitives (fonts, themes, ANSI parsing, viewports)
- `packages/render-dom` — React components on the cell grid (panels, menus, status lines)
- `packages/supervisor` — the control plane: PTY runs, workspaces, policies, worktrees, approval broker
- `apps/mico` — the web cockpit

## Development

```sh
npm install
npm run dev:supervisor   # build + start the supervisor on 127.0.0.1:4317
npm run dev:demo         # start the cockpit dev server on 127.0.0.1:5173
npm run check            # typecheck everything
```

## Feature notes

- **Workspaces** carry a policy (`permissive` / `balanced` / `strict`) that maps to harness
  sandbox flags and drives the approval broker: permissive auto-allows, others queue for a human.
- **Isolated sessions** run in a git worktree on a `mico/<run>` branch; review the diff in the
  cockpit, then merge or discard.
- **Claude Task (brokered)** runs headless Claude Code with `--permission-prompt-tool` pointed at
  `packages/supervisor/scripts/approval-mcp.mjs`, and parses `stream-json` output into the
  activity feed.
