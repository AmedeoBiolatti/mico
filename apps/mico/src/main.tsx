import { StrictMode, useEffect, useMemo, useRef, useState, type KeyboardEvent, type MouseEvent as ReactMouseEvent, type WheelEvent } from "react";
import { createRoot } from "react-dom/client";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import {
  bufferFromPlainText,
  createViewport,
  defaultFontProfiles,
  micoDarkTheme,
  parseAnsiText,
  scrollViewport,
  scrollViewportToTail,
  type TextBuffer,
  type ViewportState
} from "@mico/render-core";
import { ContextMenu, MicoRoot, Panel, Resizable, SplitPane, StatusLine, TextBufferView, Viewport, type MenuEntry } from "@mico/render-dom";
import "../../../packages/render-dom/src/styles.css";
import "./styles.css";

import { ansi, attentionTone, errorMessageFromResponse, eventNumber, formatRunStatus, summarizeApprovalInput, supervisorBase, xtermTheme } from "./support";
import type { ActivityEntry, Approval, ApprovalRule, Attention, HarnessDescriptor, PolicyId, RunGitDetails, Session, SessionMeta, SupervisorEvent, SupervisorRun, Workspace } from "./types";

const waitingAfterMs = 5000;

function App() {
  const [fontId, setFontId] = useState(defaultFontProfiles[0]!.id);
  const [selectedRunId, setSelectedRunId] = useState<string | null>(null);
  const [runs, setRuns] = useState<SupervisorRun[]>([]);
  const [harnesses, setHarnesses] = useState<HarnessDescriptor[]>([]);
  const [selectedHarnessId, setSelectedHarnessId] = useState("codex-interactive");
  const [stdinText, setStdinText] = useState("");
  const [changedFiles, setChangedFiles] = useState<string[]>([]);
  const [sessionMeta, setSessionMeta] = useState<Record<string, SessionMeta>>({});
  const [sessionEpoch, setSessionEpoch] = useState(0);
  const [lastError, setLastError] = useState<string | null>(null);
  const [menu, setMenu] = useState<{ x: number; y: number; title?: string; entries: MenuEntry[] } | null>(null);
  const [selectedRunIds, setSelectedRunIds] = useState<ReadonlySet<string>>(new Set());
  const selectionAnchorRef = useRef<string | null>(null);
  const [workspaces, setWorkspaces] = useState<Workspace[]>([]);
  const [activeWorkspaceId, setActiveWorkspaceId] = useState(() => window.localStorage.getItem("mico.workspace") ?? "all");
  const [wsBrowser, setWsBrowser] = useState<{ path: string; parent: string | null; dirs: string[] } | null>(null);
  const [newFolderName, setNewFolderName] = useState("");
  const [approvals, setApprovals] = useState<Approval[]>([]);
  const [approvalRules, setApprovalRules] = useState<ApprovalRule[]>([]);
  const [activity, setActivity] = useState<Record<string, ActivityEntry[]>>({});
  const activityListRef = useRef<HTMLDivElement | null>(null);
  const [diffView, setDiffView] = useState<{ runId: string; file: string | null; text: string } | null>(null);
  const [taskPrompt, setTaskPrompt] = useState("");
  const [renamingRunId, setRenamingRunId] = useState<string | null>(null);
  const [now, setNow] = useState(() => Date.now());
  const [viewport, setViewport] = useState<ViewportState>(() => createViewport({ x: 0, y: 0, width: 100, height: 13 }));

  const sessionsRef = useRef(new Map<string, Session>());
  const termContainerRef = useRef<HTMLDivElement | null>(null);
  const selectedRunIdRef = useRef<string | null>(null);
  const sessionMetaRef = useRef(sessionMeta);

  const font = defaultFontProfiles.find((profile) => profile.id === fontId) ?? defaultFontProfiles[0]!;
  const xtermFontFamily = useMemo(() => [font.family, ...font.fallbackFamilies].map((family) => family.includes(" ") ? `"${family}"` : family).join(", "), [font]);
  const logBuffer = useMemo<TextBuffer>(() => {
    const lines = [
      `${ansi("36", "[mission]")} mico supervisor @ ${supervisorBase}`,
      ...(harnesses.length === 0
        ? [`${ansi("31", "[error]")} supervisor offline or unreachable`]
        : harnesses.map((harness) =>
            harness.available
              ? `${ansi("32", "[ok]")} harness ${harness.id} :: ${harness.displayName}`
              : `${ansi("33", "[missing]")} harness ${harness.id} :: ${harness.displayName}`
          )),
      `${ansi("35", "[channel]")} no session attached`,
      `${ansi("35", "[channel]")} select a run on the left, or open a new session`
    ];
    return { lines: parseAnsiText(lines.join("\n"), micoDarkTheme) };
  }, [harnesses]);
  const selectedRun = runs.find((run) => run.id === selectedRunId) ?? null;
  const diffBuffer = useMemo<TextBuffer>(
    () => ({ lines: parseAnsiText(diffView && diffView.text.length > 0 ? diffView.text : "no changes against HEAD", micoDarkTheme) }),
    [diffView]
  );
  const viewBuffer = diffView ? diffBuffer : logBuffer;
  const detailsBuffer = useMemo(() => {
    const lines = selectedRun
      ? [
          `run:      ${selectedRun.id}`,
          `harness:  ${selectedRun.harnessId}`,
          `command:  ${selectedRun.command.length > 0 ? selectedRun.command : "(interactive)"}`,
          `cwd:      ${selectedRun.cwd}`,
          `created:  ${selectedRun.createdAt}`,
          `started:  ${selectedRun.startedAt ?? "-"}`,
          `ended:    ${selectedRun.endedAt ?? "-"}`,
          `status:   ${formatRunStatus(selectedRun)}`,
          ...(selectedRun.worktree ? [`worktree: ${selectedRun.worktree} (branch mico/${selectedRun.id})`] : [])
        ]
      : ["no run selected"];
    return bufferFromPlainText(lines.join("\n"), { foreground: micoDarkTheme.foreground });
  }, [selectedRun]);
  const selectedMeta = selectedRunId ? sessionMeta[selectedRunId] : undefined;
  const activeWorkspace = workspaces.find((workspace) => workspace.id === activeWorkspaceId) ?? null;
  const visibleRuns = activeWorkspace
    ? runs.filter((run) => run.cwd === activeWorkspace.path || run.cwd.startsWith(`${activeWorkspace.path}/`))
    : runs;

  useEffect(() => {
    selectedRunIdRef.current = selectedRunId;
  }, [selectedRunId]);

  useEffect(() => {
    sessionMetaRef.current = sessionMeta;
  }, [sessionMeta]);

  useEffect(() => {
    window.localStorage.setItem("mico.workspace", activeWorkspaceId);
  }, [activeWorkspaceId]);

  useEffect(() => {
    if (!wsBrowser) {
      return;
    }
    const onKey = (event: globalThis.KeyboardEvent) => {
      if (event.key === "Escape") {
        setWsBrowser(null);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [wsBrowser !== null]);

  useEffect(() => {
    void refreshRuns();
    void refreshHarnesses();
    void refreshWorkspaces();
    void refreshApprovals();
    const runsInterval = window.setInterval(() => {
      void refreshRuns();
      void refreshApprovals();
    }, 1500);
    const tickInterval = window.setInterval(() => setNow(Date.now()), 1000);

    return () => {
      window.clearInterval(runsInterval);
      window.clearInterval(tickInterval);
      for (const session of sessionsRef.current.values()) {
        session.events?.close();
        session.terminal.dispose();
      }
      sessionsRef.current.clear();
    };
  }, []);

  // Swap the selected session's terminal into the panel.
  useEffect(() => {
    const container = termContainerRef.current;
    if (!container) {
      return;
    }

    while (container.firstChild) {
      container.removeChild(container.firstChild);
    }

    const session = selectedRunId ? sessionsRef.current.get(selectedRunId) : undefined;
    if (!session) {
      return;
    }

    container.append(session.host);
    if (!session.opened) {
      session.terminal.open(session.host);
      session.opened = true;
    }

    const frame = window.requestAnimationFrame(() => {
      session.fit.fit();
      // Replay buffered output only now that the terminal has its real size;
      // parsing it at the default 80x24 mangles full-screen TUI frames.
      if (session.pendingOutput) {
        const pending = session.pendingOutput;
        session.pendingOutput = null;
        for (const text of pending) {
          session.terminal.write(text);
        }
      }
      session.terminal.focus();
      if (sessionMetaRef.current[session.runId]?.status === "running") {
        void resizeRun(session.runId, session.terminal.cols, session.terminal.rows);
      }
    });

    const resizeObserver = new ResizeObserver(() => {
      if (!session.opened || container.clientHeight === 0) {
        return;
      }
      session.fit.fit();
      if (sessionMetaRef.current[session.runId]?.status === "running") {
        void resizeRun(session.runId, session.terminal.cols, session.terminal.rows);
      }
    });
    resizeObserver.observe(container);

    return () => {
      window.cancelAnimationFrame(frame);
      resizeObserver.disconnect();
    };
  }, [selectedRunId, sessionEpoch]);

  useEffect(() => {
    for (const session of sessionsRef.current.values()) {
      session.terminal.options.fontFamily = xtermFontFamily;
      session.terminal.options.fontSize = font.sizePx;
      if (session.opened) {
        session.fit.fit();
      }
    }
  }, [font, xtermFontFamily]);

  function bumpMeta(runId: string, patch: Partial<SessionMeta>) {
    setSessionMeta((current) => {
      const existing = current[runId];
      if (!existing) {
        return current;
      }
      return { ...current, [runId]: { ...existing, ...patch } };
    });
  }

  function ensureSession(run: { id: string; harnessId: string; status?: string; exitCode?: number | null }): Session {
    const existing = sessionsRef.current.get(run.id);
    if (existing) {
      return existing;
    }

    const host = document.createElement("div");
    host.className = "session-host";
    const terminal = new Terminal({
      allowProposedApi: false,
      convertEol: true,
      cursorBlink: true,
      fontFamily: xtermFontFamily,
      fontSize: font.sizePx,
      fontWeight: 400,
      letterSpacing: 0,
      theme: xtermTheme
    });
    const fit = new FitAddon();
    terminal.loadAddon(fit);
    // Ctrl+Shift+C / Ctrl+Shift+V copy and paste instead of reaching the pty.
    terminal.attachCustomKeyEventHandler((event) => {
      if (event.type !== "keydown" || !event.ctrlKey || !event.shiftKey) {
        return true;
      }
      if (event.code === "KeyC" && terminal.hasSelection()) {
        void navigator.clipboard.writeText(terminal.getSelection());
        return false;
      }
      if (event.code === "KeyV") {
        void navigator.clipboard.readText().then((text) => {
          if (text.length > 0) {
            terminal.paste(text);
          }
        });
        return false;
      }
      return true;
    });
    // Classic X11 behavior: copy on select, paste on middle click.
    terminal.onSelectionChange(() => {
      const selection = terminal.getSelection();
      if (selection.length > 0) {
        void navigator.clipboard.writeText(selection);
      }
    });
    host.addEventListener("mousedown", (event) => {
      if (event.button === 1) {
        event.preventDefault();
      }
    });
    host.addEventListener("auxclick", (event) => {
      if (event.button !== 1) {
        return;
      }
      event.preventDefault();
      if (sessionMetaRef.current[run.id]?.status !== "running") {
        return;
      }
      void navigator.clipboard.readText().then((text) => {
        if (text.length > 0) {
          terminal.paste(text);
        }
      });
    });
    terminal.onData((data) => {
      if (sessionMetaRef.current[run.id]?.status === "running") {
        void writeRunInput(run.id, data);
      }
    });

    const session: Session = {
      runId: run.id,
      harnessId: run.harnessId,
      terminal,
      fit,
      host,
      events: null,
      lastEventNum: 0,
      lastAgentNum: 0,
      opened: false,
      pendingOutput: []
    };
    sessionsRef.current.set(run.id, session);

    const ended = run.status !== undefined && run.status !== "running" && run.status !== "created";
    setSessionMeta((current) => ({
      ...current,
      [run.id]: {
        status: run.status ?? "running",
        exitCode: run.exitCode ?? null,
        lastOutputAt: Date.now(),
        acknowledged: ended
      }
    }));

    attachSessionEvents(session);
    setSessionEpoch((epoch) => epoch + 1);
    return session;
  }

  function writeToSession(session: Session, text: string) {
    if (session.pendingOutput) {
      session.pendingOutput.push(text);
      return;
    }
    session.terminal.write(text);
  }

  function attachSessionEvents(session: Session) {
    const events = new EventSource(`${supervisorBase}/api/runs/${session.runId}/events`);
    session.events = events;

    events.addEventListener("pty.output", (event) => {
      const runEvent = JSON.parse((event as MessageEvent).data) as SupervisorEvent;
      const num = eventNumber(runEvent.id);
      if (num !== null) {
        if (num <= session.lastEventNum) {
          return;
        }
        session.lastEventNum = num;
      }
      if (typeof runEvent.payload.text === "string" && runEvent.payload.text.length > 0) {
        writeToSession(session, runEvent.payload.text);
        bumpMeta(session.runId, { lastOutputAt: Date.now() });
      }
    });

    events.addEventListener("agent.event", (event) => {
      const runEvent = JSON.parse((event as MessageEvent).data) as SupervisorEvent;
      const num = eventNumber(runEvent.id);
      if (num !== null) {
        if (num <= session.lastAgentNum) {
          return;
        }
        session.lastAgentNum = num;
      }
      const entry: ActivityEntry = {
        id: runEvent.id,
        kind: typeof runEvent.payload.kind === "string" ? runEvent.payload.kind : "system",
        label: typeof runEvent.payload.label === "string" ? runEvent.payload.label : "",
        detail: typeof runEvent.payload.detail === "string" ? runEvent.payload.detail : null
      };
      setActivity((current) => ({
        ...current,
        [session.runId]: [...(current[session.runId] ?? []).slice(-199), entry]
      }));
    });

    events.addEventListener("process.exited", (event) => {
      const runEvent = JSON.parse((event as MessageEvent).data) as SupervisorEvent;
      const exitCode = typeof runEvent.payload.exitCode === "number" ? runEvent.payload.exitCode : null;
      writeToSession(session, `\r\n\u001b[36m[process]\u001b[0m exited ${exitCode ?? "signal"}\r\n`);
      events.close();
      session.events = null;
      setSessionMeta((current) => {
        const existing = current[session.runId];
        if (!existing) {
          return current;
        }
        return {
          ...current,
          [session.runId]: {
            ...existing,
            status: "exited",
            exitCode,
            acknowledged: selectedRunIdRef.current === session.runId
          }
        };
      });
      void refreshRuns();
      if (selectedRunIdRef.current === session.runId) {
        void refreshRunGit(session.runId);
      }
    });

    events.addEventListener("run.failed", (event) => {
      const runEvent = JSON.parse((event as MessageEvent).data) as SupervisorEvent;
      const message = typeof runEvent.payload.message === "string" ? runEvent.payload.message : "run failed";
      writeToSession(session, `\r\n\u001b[31m[error]\u001b[0m ${message}\r\n`);
      events.close();
      session.events = null;
      setSessionMeta((current) => {
        const existing = current[session.runId];
        if (!existing) {
          return current;
        }
        return {
          ...current,
          [session.runId]: {
            ...existing,
            status: "failed",
            acknowledged: selectedRunIdRef.current === session.runId
          }
        };
      });
      void refreshRuns();
    });
  }

  async function refreshRuns() {
    try {
      const response = await fetch(`${supervisorBase}/api/runs`);
      const body = await response.json() as { runs: SupervisorRun[] };
      const sortedRuns = [...body.runs].sort((a, b) => b.createdAt.localeCompare(a.createdAt));
      setRuns(sortedRuns);
      for (const run of sortedRuns) {
        if (run.status === "running") {
          ensureSession(run);
        }
      }
      // Reconcile: a supervisor restart can end runs without emitting exit events.
      setSessionMeta((current) => {
        let changed = false;
        const next = { ...current };
        for (const run of sortedRuns) {
          const meta = next[run.id];
          if (meta && meta.status === "running" && run.status !== "running" && run.status !== "created") {
            next[run.id] = { ...meta, status: run.status, exitCode: run.exitCode ?? null };
            changed = true;
          }
        }
        return changed ? next : current;
      });
    } catch {
      setRuns([]);
    }
  }

  async function refreshWorkspaces() {
    try {
      const response = await fetch(`${supervisorBase}/api/workspaces`);
      const body = await response.json() as { workspaces: Workspace[] };
      setWorkspaces(body.workspaces);
    } catch {
      setWorkspaces([]);
    }
  }

  async function refreshApprovals() {
    try {
      const [approvalsResponse, rulesResponse] = await Promise.all([
        fetch(`${supervisorBase}/api/approvals`),
        fetch(`${supervisorBase}/api/approval-rules`)
      ]);
      const approvalsBody = await approvalsResponse.json() as { approvals: Approval[] };
      const rulesBody = await rulesResponse.json() as { rules: ApprovalRule[] };
      setApprovals([...approvalsBody.approvals].sort((a, b) => b.requestedAt.localeCompare(a.requestedAt)));
      setApprovalRules(rulesBody.rules);
    } catch {
      setApprovals([]);
      setApprovalRules([]);
    }
  }

  async function decideApproval(id: string, state: "allowed" | "denied", always = false) {
    const response = await fetch(`${supervisorBase}/api/approvals/${id}/decision`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ state, always })
    });
    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
    }
    void refreshApprovals();
  }

  async function removeApprovalRule(id: string) {
    const response = await fetch(`${supervisorBase}/api/approval-rules/${id}`, { method: "DELETE" });
    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
    }
    void refreshApprovals();
  }

  async function openWsBrowser(path?: string) {
    const query = path ? `?path=${encodeURIComponent(path)}` : "";
    try {
      const response = await fetch(`${supervisorBase}/api/fs${query}`);
      if (!response.ok) {
        setLastError(await errorMessageFromResponse(response));
        return;
      }
      setWsBrowser(await response.json() as { path: string; parent: string | null; dirs: string[] });
    } catch (error) {
      setLastError(error instanceof Error ? error.message : String(error));
    }
  }

  async function makeFolder() {
    const name = newFolderName.trim();
    if (!wsBrowser || name.length === 0) {
      return;
    }

    const response = await fetch(`${supervisorBase}/api/fs/mkdir`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ path: `${wsBrowser.path}/${name}` })
    });

    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
      return;
    }

    const body = await response.json() as { path: string };
    setNewFolderName("");
    void openWsBrowser(body.path);
  }

  async function addWorkspaceAt(path: string) {
    try {
      const response = await fetch(`${supervisorBase}/api/workspaces`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ path })
      });

      if (!response.ok) {
        setLastError(await errorMessageFromResponse(response));
        return;
      }

      const body = await response.json() as { workspace: Workspace };
      setActiveWorkspaceId(body.workspace.id);
      setWsBrowser(null);
      void refreshWorkspaces();
    } catch (error) {
      setLastError(error instanceof Error ? error.message : String(error));
    }
  }

  async function refreshHarnesses() {
    try {
      const response = await fetch(`${supervisorBase}/api/harnesses`);
      const body = await response.json() as { harnesses: HarnessDescriptor[] };
      setHarnesses(body.harnesses);
      const selected = body.harnesses.find((harness) => harness.id === selectedHarnessId);
      if (!selected?.available) {
        setSelectedHarnessId(body.harnesses.find((harness) => harness.available)?.id ?? "shell");
      }
    } catch {
      setHarnesses([]);
    }
  }

  function selectRun(run: SupervisorRun) {
    const session = ensureSession(run);
    setSelectedRunId(run.id);
    // Selecting the already-selected run leaves focus on the tab/row button,
    // so keystrokes never reach the pty. Hand it back once the swap settles.
    window.requestAnimationFrame(() => {
      if (session.opened) {
        session.terminal.focus();
      }
    });
    setDiffView(null);
    setSessionMeta((current) => {
      const existing = current[run.id];
      if (!existing || existing.status === "running" || existing.acknowledged) {
        return current;
      }
      return { ...current, [run.id]: { ...existing, acknowledged: true } };
    });
    void refreshRunGit(run.id);
  }

  async function openNewSession(target?: Workspace | null, isolate = false) {
    const workspace = target ?? activeWorkspace;
    const command = selectedHarnessId === "claude-task" ? taskPrompt.trim() : "";
    if (selectedHarnessId === "claude-task" && command.length === 0) {
      setLastError("claude-task needs a prompt");
      return;
    }
    setLastError(null);
    setChangedFiles([]);

    try {
      const response = await fetch(`${supervisorBase}/api/runs`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          harnessId: selectedHarnessId,
          command,
          cols: 120,
          rows: 32,
          ...(workspace ? { cwd: workspace.path } : {}),
          ...(isolate ? { isolate: true } : {})
        })
      });

      if (!response.ok) {
        setLastError(await errorMessageFromResponse(response));
        void refreshRuns();
        return;
      }

      const body = await response.json() as { run: SupervisorRun };
      ensureSession({ id: body.run.id, harnessId: selectedHarnessId, status: body.run.status });
      setSelectedRunId(body.run.id);
      setTaskPrompt("");
      void refreshRuns();
    } catch (error) {
      setLastError(`Supervisor unavailable at ${supervisorBase}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }

  // Reopen a finished agent session interactively in the harness family it
  // came from: claude --resume <id> or codex resume <id>, same cwd.
  async function resumeRun(run: SupervisorRun) {
    if (!run.agentSessionId) {
      return;
    }
    setLastError(null);
    const harnessId = run.harnessId.startsWith("codex") ? "codex-interactive" : "claude-code";

    try {
      const response = await fetch(`${supervisorBase}/api/runs`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          harnessId,
          command: "",
          cwd: run.cwd,
          cols: 120,
          rows: 32,
          resumeSessionId: run.agentSessionId
        })
      });

      if (!response.ok) {
        setLastError(await errorMessageFromResponse(response));
        void refreshRuns();
        return;
      }

      const body = await response.json() as { run: SupervisorRun };
      ensureSession({ id: body.run.id, harnessId, status: body.run.status });
      setSelectedRunId(body.run.id);
      void refreshRuns();
    } catch (error) {
      setLastError(`Supervisor unavailable at ${supervisorBase}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }

  async function refreshRunGit(runId: string) {
    try {
      const response = await fetch(`${supervisorBase}/api/runs/${runId}`);
      const body = await response.json() as { git: RunGitDetails };
      setChangedFiles(body.git.changedFiles);
    } catch {
      setChangedFiles([]);
    }
  }

  function scrollTerminal(deltaRows: number) {
    setViewport((current) => scrollViewport(current, deltaRows, viewBuffer.lines.length));
  }

  async function openDiff(runId: string, file: string | null) {
    try {
      const query = file ? `?file=${encodeURIComponent(file)}` : "";
      const response = await fetch(`${supervisorBase}/api/runs/${runId}/diff${query}`);
      if (!response.ok) {
        setLastError(await errorMessageFromResponse(response));
        return;
      }
      const body = await response.json() as { diff: string };
      setDiffView({ runId, file, text: body.diff });
      setViewport((current) => ({ ...current, scrollY: 0, followTail: false }));
    } catch (error) {
      setLastError(error instanceof Error ? error.message : String(error));
    }
  }

  async function mergeRun(runId: string) {
    const response = await fetch(`${supervisorBase}/api/runs/${runId}/merge`, { method: "POST" });
    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
    } else {
      setLastError(null);
      setDiffView(null);
    }
    void refreshRuns();
    void refreshRunGit(runId);
  }

  async function discardRun(runId: string) {
    const response = await fetch(`${supervisorBase}/api/runs/${runId}/discard`, { method: "POST" });
    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
    } else {
      setLastError(null);
      setDiffView(null);
    }
    void refreshRuns();
    void refreshRunGit(runId);
  }

  function handleTerminalWheel(event: WheelEvent<HTMLDivElement>) {
    event.preventDefault();
    const rows = Math.trunc(event.deltaY / font.lineHeightPx) || Math.sign(event.deltaY);
    scrollTerminal(rows);
  }

  function handleTerminalKeyDown(event: KeyboardEvent<HTMLDivElement>) {
    if (event.key === "PageUp") {
      event.preventDefault();
      scrollTerminal(-viewport.rect.height);
    } else if (event.key === "PageDown") {
      event.preventDefault();
      scrollTerminal(viewport.rect.height);
    } else if (event.key === "End") {
      event.preventDefault();
      setViewport((current) => scrollViewportToTail(current, viewBuffer.lines.length));
    } else if (event.key === "Home") {
      event.preventDefault();
      setViewport((current) => ({ ...current, scrollY: 0, followTail: false }));
    }
  }

  async function sendSupervisorInput() {
    if (!selectedRunId || stdinText.length === 0) {
      return;
    }

    await writeRunInput(selectedRunId, stdinText);
    setStdinText("");
  }

  async function writeRunInput(targetRunId: string, input: string) {
    await fetch(`${supervisorBase}/api/runs/${targetRunId}/input`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ input })
    });
  }

  async function resizeRun(targetRunId: string, cols: number, rows: number) {
    await fetch(`${supervisorBase}/api/runs/${targetRunId}/resize`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ cols, rows })
    });
  }

  async function terminateSupervisorRun() {
    if (!selectedRunId) {
      return;
    }

    await fetch(`${supervisorBase}/api/runs/${selectedRunId}/terminate`, { method: "POST" });
  }

  function closeSession(runId: string | null) {
    if (!runId) {
      return;
    }

    const session = sessionsRef.current.get(runId);
    if (session) {
      session.events?.close();
      session.terminal.dispose();
      session.host.remove();
      sessionsRef.current.delete(runId);
    }
    setSessionMeta((current) => {
      const { [runId]: _removed, ...rest } = current;
      return rest;
    });
    setActivity((current) => {
      const { [runId]: _removed, ...rest } = current;
      return rest;
    });
    if (selectedRunIdRef.current === runId) {
      setSelectedRunId(null);
      setChangedFiles([]);
    }
  }

  async function terminateRun(runId: string) {
    await fetch(`${supervisorBase}/api/runs/${runId}/terminate`, { method: "POST" });
  }

  async function removeRun(runId: string) {
    closeSession(runId);
    const response = await fetch(`${supervisorBase}/api/runs/${runId}`, { method: "DELETE" });
    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
    }
    void refreshRuns();
  }

  function openMenu(event: ReactMouseEvent, title: string, entries: MenuEntry[]) {
    event.preventDefault();
    event.stopPropagation();
    setMenu({ x: event.clientX, y: event.clientY, title, entries });
  }

  function handleRunClick(event: ReactMouseEvent, run: SupervisorRun) {
    if (event.ctrlKey || event.metaKey) {
      setSelectedRunIds((current) => {
        const next = new Set(current);
        if (next.has(run.id)) {
          next.delete(run.id);
        } else {
          next.add(run.id);
        }
        return next;
      });
      selectionAnchorRef.current = run.id;
      return;
    }

    if (event.shiftKey && selectionAnchorRef.current) {
      const ids = visibleRuns.map((entry) => entry.id);
      const anchorIndex = ids.indexOf(selectionAnchorRef.current);
      const clickedIndex = ids.indexOf(run.id);
      if (anchorIndex !== -1 && clickedIndex !== -1) {
        const [low, high] = anchorIndex < clickedIndex ? [anchorIndex, clickedIndex] : [clickedIndex, anchorIndex];
        setSelectedRunIds(new Set(ids.slice(low, high + 1)));
        return;
      }
    }

    selectionAnchorRef.current = run.id;
    setSelectedRunIds(new Set([run.id]));
    selectRun(run);
  }

  function handleRunContextMenu(event: ReactMouseEvent, run: SupervisorRun) {
    if (selectedRunIds.size > 1 && selectedRunIds.has(run.id)) {
      const ids = runs.filter((entry) => selectedRunIds.has(entry.id)).map((entry) => entry.id);
      openMenu(event, `${ids.length} runs selected`, bulkMenuEntries(ids));
      return;
    }

    selectionAnchorRef.current = run.id;
    setSelectedRunIds(new Set([run.id]));
    openMenu(event, `${run.id} · ${run.harnessId}`, runMenuEntries(run));
  }

  function bulkMenuEntries(ids: string[]): MenuEntry[] {
    const selectedRuns = runs.filter((run) => ids.includes(run.id));
    const isRunning = (run: SupervisorRun) => (sessionMeta[run.id]?.status ?? run.status) === "running";
    const runningIds = selectedRuns.filter(isRunning).map((run) => run.id);
    const stoppedIds = selectedRuns.filter((run) => !isRunning(run)).map((run) => run.id);
    const viewIds = ids.filter((id) => sessionsRef.current.has(id));
    return [
      { id: "copy-ids", label: "copy run ids", onSelect: () => void navigator.clipboard.writeText(ids.join("\n")) },
      { kind: "separator", id: "sep" },
      {
        id: "close",
        label: `close ${viewIds.length} view${viewIds.length === 1 ? "" : "s"}`,
        hint: "keeps processes",
        disabled: viewIds.length === 0,
        onSelect: () => viewIds.forEach((id) => closeSession(id))
      },
      {
        id: "term",
        label: `terminate ${runningIds.length} running`,
        hint: "SIGTERM",
        danger: true,
        disabled: runningIds.length === 0,
        onSelect: () => runningIds.forEach((id) => void terminateRun(id))
      },
      {
        id: "remove",
        label: `remove ${stoppedIds.length} run${stoppedIds.length === 1 ? "" : "s"}`,
        hint: "delete history",
        danger: true,
        disabled: stoppedIds.length === 0,
        onSelect: () => {
          stoppedIds.forEach((id) => void removeRun(id));
          setSelectedRunIds(new Set());
        }
      }
    ];
  }

  function workspaceStats(workspace: Workspace) {
    const wsRuns = runs.filter((run) => run.cwd === workspace.path || run.cwd.startsWith(`${workspace.path}/`));
    const live = wsRuns.filter((run) => run.status === "running").length;
    const attn = wsRuns.filter((run) => {
      const attention = attentionFor(run.id);
      return attention === "waiting" || attention === "done" || attention === "failed";
    }).length;
    return { total: wsRuns.length, live, attn };
  }

  async function removeWorkspace(workspaceId: string) {
    const response = await fetch(`${supervisorBase}/api/workspaces/${workspaceId}`, { method: "DELETE" });
    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
    }
    if (activeWorkspaceId === workspaceId) {
      setActiveWorkspaceId("all");
    }
    void refreshWorkspaces();
  }

  async function setWorkspacePolicy(workspaceId: string, policy: PolicyId) {
    const response = await fetch(`${supervisorBase}/api/workspaces/${workspaceId}`, {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ policy })
    });
    if (!response.ok) {
      setLastError(await errorMessageFromResponse(response));
    }
    void refreshWorkspaces();
  }

  function workspaceMenuEntries(workspace: Workspace): MenuEntry[] {
    const harness = harnesses.find((entry) => entry.id === selectedHarnessId);
    const harnessName = harness?.displayName ?? selectedHarnessId;
    return [
      { id: "open", label: "focus workspace", onSelect: () => setActiveWorkspaceId(workspace.id) },
      { id: "new", label: "new session here", hint: harnessName, onSelect: () => void openNewSession(workspace) },
      { id: "new-isolated", label: "new isolated session", hint: "git worktree", onSelect: () => void openNewSession(workspace, true) },
      { id: "copy-path", label: "copy path", onSelect: () => void navigator.clipboard.writeText(workspace.path) },
      { kind: "separator", id: "sep-policy" },
      ...(["permissive", "balanced", "strict"] as const).map((policy): MenuEntry => ({
        id: `policy-${policy}`,
        label: `policy: ${policy}`,
        hint: workspace.policy === policy ? "●" : undefined,
        disabled: workspace.policy === policy,
        onSelect: () => void setWorkspacePolicy(workspace.id, policy)
      })),
      { kind: "separator", id: "sep" },
      { id: "remove", label: "remove workspace", hint: "keeps runs", danger: true, onSelect: () => void removeWorkspace(workspace.id) }
    ];
  }

  async function renameRun(runId: string, title: string) {
    try {
      const response = await fetch(`${supervisorBase}/api/runs/${runId}`, {
        method: "PATCH",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title })
      });
      if (!response.ok) {
        setLastError(await errorMessageFromResponse(response));
        return;
      }
      await refreshRuns();
    } catch (error) {
      setLastError(error instanceof Error ? error.message : String(error));
    }
  }

  function runMenuEntries(run: SupervisorRun): MenuEntry[] {
    const meta = sessionMeta[run.id];
    const running = (meta?.status ?? run.status) === "running";
    return [
      { id: "attach", label: "attach", hint: "view terminal", onSelect: () => selectRun(run) },
      { id: "rename", label: "rename", hint: run.title ? `"${run.title}"` : "set a title", onSelect: () => setRenamingRunId(run.id) },
      { id: "copy-id", label: "copy run id", onSelect: () => void navigator.clipboard.writeText(run.id) },
      { id: "copy-cmd", label: "copy command", disabled: run.command.length === 0, onSelect: () => void navigator.clipboard.writeText(run.command) },
      {
        id: "resume",
        label: "resume session",
        hint: "continue conversation",
        disabled: running || !run.agentSessionId,
        onSelect: () => void resumeRun(run)
      },
      { kind: "separator", id: "sep" },
      { id: "close", label: "close view", hint: "keeps process", disabled: !sessionsRef.current.has(run.id), onSelect: () => closeSession(run.id) },
      { id: "term", label: "terminate", hint: "SIGTERM", danger: true, disabled: !running, onSelect: () => void terminateRun(run.id) },
      { id: "remove", label: "remove run", hint: "delete history", danger: true, disabled: running, onSelect: () => void removeRun(run.id) }
    ];
  }

  function terminalMenuEntries(): MenuEntry[] {
    const session = selectedRunId ? sessionsRef.current.get(selectedRunId) : undefined;
    const running = selectedRunId ? sessionMeta[selectedRunId]?.status === "running" : false;
    // Snapshot now: the selection can be gone by the time the menu item is clicked.
    const selectionText = session?.terminal.hasSelection() ? session.terminal.getSelection() : "";
    return [
      {
        id: "copy",
        label: "copy selection",
        hint: "ctrl+shift+c",
        disabled: selectionText.length === 0,
        onSelect: () => void navigator.clipboard.writeText(selectionText)
      },
      {
        id: "paste",
        label: "paste",
        hint: "ctrl+shift+v",
        disabled: !session || !running,
        onSelect: () => void navigator.clipboard.readText().then((text) => {
          if (text.length > 0) {
            session?.terminal.paste(text);
          }
        })
      },
      { id: "clear", label: "clear scrollback", disabled: !session, onSelect: () => session?.terminal.clear() },
      { kind: "separator", id: "sep" },
      { id: "close", label: "close view", hint: "keeps process", disabled: !session, onSelect: () => closeSession(selectedRunId) },
      { id: "term", label: "terminate", hint: "SIGTERM", danger: true, disabled: !running, onSelect: () => { if (selectedRunId) void terminateRun(selectedRunId); } }
    ];
  }

  function shellMenuEntries(): MenuEntry[] {
    const harness = harnesses.find((entry) => entry.id === selectedHarnessId);
    return [
      { id: "new", label: "new session", hint: harness?.displayName ?? selectedHarnessId, onSelect: () => void openNewSession() },
      { id: "refresh", label: "refresh runs", onSelect: () => void refreshRuns() }
    ];
  }

  function attentionFor(runId: string): Attention {
    const meta = sessionMeta[runId];
    if (!meta) {
      return null;
    }

    if (meta.status === "running") {
      return now - meta.lastOutputAt > waitingAfterMs ? "waiting" : "working";
    }

    if (meta.acknowledged) {
      return null;
    }

    return meta.status === "failed" || (meta.exitCode ?? 0) !== 0 ? "failed" : "done";
  }

  const attentionRuns = runs.filter((run) => {
    const attention = attentionFor(run.id);
    return attention === "waiting" || attention === "done" || attention === "failed";
  });
  const liveCount = runs.filter((run) => run.status === "running").length;
  const selectedAttention = selectedRunId ? attentionFor(selectedRunId) : null;
  const pendingApprovals = approvals.filter((approval) => approval.state === "pending");
  // Ended isolated runs still holding a worktree are waiting on merge/discard.
  const reviewRuns = runs.filter((run) => run.worktree && run.status !== "running" && run.status !== "created");
  const queueCount = pendingApprovals.length + reviewRuns.length;
  const attnTotal = attentionRuns.length + queueCount;
  const selectedActivity = selectedRunId ? activity[selectedRunId] ?? [] : [];

  useEffect(() => {
    const list = activityListRef.current;
    if (list) {
      list.scrollTop = list.scrollHeight;
    }
  }, [selectedActivity.length, selectedRunId]);

  return (
    <MicoRoot font={font} density="compact">
      <main className="demo-shell" onContextMenu={(event) => openMenu(event, "mico", shellMenuEntries())}>
        <header className="demo-topbar">
          <div className="demo-brand">
            <span className="demo-mark">
              <svg className="demo-mark-icon" viewBox="0 0 8 8" width="16" height="16" shapeRendering="crispEdges" aria-hidden="true">
                {/* cap */}
                <rect x="2" y="0" width="4" height="1" fill="currentColor" />
                <rect x="1" y="1" width="6" height="1" fill="currentColor" />
                <rect x="0" y="2" width="8" height="2" fill="currentColor" />
                {/* spots */}
                <rect className="mush-spot" x="2" y="2" width="1" height="1" />
                <rect className="mush-spot" x="5" y="2" width="1" height="1" />
                <rect className="mush-spot" x="3" y="1" width="1" height="1" />
                {/* stem */}
                <rect className="mush-stem" x="2" y="5" width="4" height="2" />
                <rect className="mush-stem" x="1" y="7" width="6" height="1" />
              </svg>
              MiCo
            </span>
            <span className="demo-path">mission/control ::</span>
            <select className="demo-workspace" value={activeWorkspaceId} onChange={(event) => setActiveWorkspaceId(event.target.value)} aria-label="workspace">
              <option value="all">all workspaces</option>
              {workspaces.map((workspace) => (
                <option key={workspace.id} value={workspace.id}>{workspace.name}</option>
              ))}
            </select>
            <button type="button" onClick={() => void openWsBrowser(activeWorkspace?.path)}>+ws</button>
            {attnTotal > 0 && (
              <span className="demo-attn">⚑ {attnTotal} need{attnTotal === 1 ? "s" : ""} attention</span>
            )}
          </div>
          <div className="demo-prompt">
            <select className="demo-harness" value={selectedHarnessId} onChange={(event) => setSelectedHarnessId(event.target.value)} aria-label="harness">
              {harnesses.map((harness) => (
                <option key={harness.id} value={harness.id} disabled={!harness.available}>
                  {harness.displayName}{harness.available ? "" : " (missing)"}
                </option>
              ))}
            </select>
            {selectedHarnessId === "claude-task" && (
              <input
                className="demo-task-prompt"
                value={taskPrompt}
                placeholder="task prompt…"
                onChange={(event) => setTaskPrompt(event.target.value)}
                onKeyDown={(event) => { if (event.key === "Enter") void openNewSession(); }}
                aria-label="task prompt"
              />
            )}
            <button type="button" onClick={() => void openNewSession()}>new session</button>
          </div>
          <div className="demo-controls" aria-label="demo controls">
            <input className="demo-stdin" value={stdinText} onChange={(event) => setStdinText(event.target.value)} onKeyDown={(event) => { if (event.key === "Enter") void sendSupervisorInput(); }} aria-label="stdin" placeholder="stdin" />
            <button type="button" onClick={sendSupervisorInput} disabled={!selectedRunId}>stdin</button>
            <button type="button" onClick={terminateSupervisorRun} disabled={!selectedRunId || selectedMeta?.status !== "running"}>term</button>
            <button type="button" onClick={() => closeSession(selectedRunId)} disabled={!selectedRunId}>close</button>
            <select value={fontId} onChange={(event) => setFontId(event.target.value)} aria-label="font">
              {defaultFontProfiles.map((profile) => (
                <option key={profile.id} value={profile.id}>{profile.id}</option>
              ))}
            </select>
          </div>
        </header>

        <SplitPane className="demo-grid" gapCells={1}>
          <Resizable className="demo-left-rail" axis="x" defaultWidthCells={42} minWidthCells={30} maxWidthCells={72}>
            <section className="demo-left-rail-stack">
              <Resizable axis="y" defaultHeightCells={20} minHeightCells={8} maxHeightCells={38}>
                <Panel
                  title="runs"
                  footer={
                    <StatusLine
                      items={[
                        { id: "count", label: "stored", value: String(visibleRuns.length) },
                        { id: "live", label: "live", value: String(visibleRuns.filter((run) => run.status === "running").length), tone: liveCount > 0 ? "success" : "muted" },
                        { id: "attn", label: "attn", value: String(attentionRuns.length), tone: attentionRuns.length > 0 ? "warning" : "muted" },
                        ...(selectedRunIds.size > 1 ? [{ id: "sel", label: "sel", value: String(selectedRunIds.size), tone: "accent" } as const] : [])
                      ]}
                    />
                  }
                >
                  <div className="run-list">
                    {visibleRuns.length === 0 ? (
                      <div className="run-empty">no runs in this workspace</div>
                    ) : visibleRuns.map((run) => {
                      const attention = attentionFor(run.id);
                      if (renamingRunId === run.id) {
                        return (
                          <div key={run.id} className="run-row run-row-active">
                            <span className={`run-dot run-dot-${attention ?? "idle"}`} />
                            <input
                              className="run-rename-input"
                              autoFocus
                              defaultValue={run.title ?? ""}
                              placeholder={run.id}
                              onKeyDown={(event) => {
                                if (event.key === "Enter") {
                                  void renameRun(run.id, event.currentTarget.value);
                                  setRenamingRunId(null);
                                } else if (event.key === "Escape") {
                                  setRenamingRunId(null);
                                }
                              }}
                              onBlur={() => setRenamingRunId(null)}
                            />
                          </div>
                        );
                      }
                      return (
                        <button
                          key={run.id}
                          type="button"
                          className={[
                            "run-row",
                            selectedRunId === run.id ? "run-row-active" : undefined,
                            selectedRunIds.has(run.id) ? "run-row-selected" : undefined,
                            attention === "waiting" || attention === "done" || attention === "failed" ? "run-row-attn" : undefined
                          ].filter(Boolean).join(" ")}
                          onClick={(event) => handleRunClick(event, run)}
                          onContextMenu={(event) => handleRunContextMenu(event, run)}
                        >
                          <span className={`run-dot run-dot-${attention ?? "idle"}`} />
                          <span className="run-main">
                            <span>{run.title ?? `${run.id} · ${run.harnessId}`}{run.worktree ? " ⎇" : ""}</span>
                            <span>{run.title ? `${run.id} · ${run.harnessId}` : run.command.length > 0 ? run.command : "interactive session"}</span>
                          </span>
                          <span className={attention === "waiting" ? "run-status-waiting" : undefined}>
                            {attention === "waiting" ? "needs input" : attention === "working" ? "working" : formatRunStatus(run)}
                          </span>
                        </button>
                      );
                    })}
                  </div>
                </Panel>
              </Resizable>

              <Resizable axis="y" defaultHeightCells={14} minHeightCells={6} maxHeightCells={28}>
                <Panel title="workspaces" footer={<StatusLine items={[{ id: "count", label: "registered", value: String(workspaces.length) }]} />}>
                  <div className="run-list">
                    <button
                      type="button"
                      className={["run-row", "ws-row", activeWorkspaceId === "all" ? "run-row-active" : undefined].filter(Boolean).join(" ")}
                      onClick={() => setActiveWorkspaceId("all")}
                    >
                      <span className="run-dot run-dot-idle" />
                      <span className="run-main">
                        <span>all workspaces</span>
                        <span>every registered project</span>
                      </span>
                      <span className="ws-counts">
                        <span className={runs.some((run) => run.status === "running") ? "ws-live" : "ws-none"}>●{runs.filter((run) => run.status === "running").length}</span>
                        {attentionRuns.length > 0 && <span className="ws-attn">⚑{attentionRuns.length}</span>}
                      </span>
                    </button>
                    {workspaces.map((workspace) => {
                      const stats = workspaceStats(workspace);
                      return (
                        <button
                          key={workspace.id}
                          type="button"
                          className={["run-row", "ws-row", activeWorkspaceId === workspace.id ? "run-row-active" : undefined].filter(Boolean).join(" ")}
                          onClick={() => setActiveWorkspaceId(workspace.id)}
                          onContextMenu={(event) => openMenu(event, `${workspace.name} :: ${workspace.path}`, workspaceMenuEntries(workspace))}
                        >
                          <span className={`run-dot ${stats.attn > 0 ? "run-dot-waiting" : stats.live > 0 ? "run-dot-working" : "run-dot-idle"}`} />
                          <span className="run-main">
                            <span>{workspace.name}</span>
                            <span>{workspace.path}</span>
                          </span>
                          <span className="ws-counts">
                            <span className={`ws-policy ws-policy-${workspace.policy}`}>{workspace.policy}</span>
                            <span className={stats.live > 0 ? "ws-live" : "ws-none"}>●{stats.live}</span>
                            {stats.attn > 0 && <span className="ws-attn">⚑{stats.attn}</span>}
                          </span>
                        </button>
                      );
                    })}
                  </div>
                </Panel>
              </Resizable>

              <Resizable className="demo-permissions" axis="y" minHeightCells={8} maxHeightCells={34}>
                <Panel
                  title="queue"
                  variant={queueCount > 0 ? "warning" : "default"}
                  footer={
                    <StatusLine
                      items={[
                        { id: "queue", label: "queue", value: String(queueCount), tone: queueCount > 0 ? "warning" : "muted" },
                        { id: "review", label: "review", value: String(reviewRuns.length), tone: reviewRuns.length > 0 ? "accent" : "muted" },
                        { id: "total", label: "recorded", value: String(approvals.length) }
                      ]}
                    />
                  }
                >
                  <div className="approval-list">
                    {reviewRuns.length > 0 && (
                      <>
                        <div className="rule-header">awaiting review</div>
                        {reviewRuns.map((run) => (
                          <div key={run.id} className="approval-row approval-row-pending">
                            <button type="button" className="approval-main approval-jump" onClick={() => selectRun(run)}>
                              <span>{run.title ?? `${run.id} · ${run.harnessId}`} ⎇</span>
                              <span>{run.command.length > 0 ? run.command : run.cwd}</span>
                            </button>
                            <span className="approval-actions">
                              <button type="button" onClick={() => void openDiff(run.id, null)}>diff</button>
                              <button type="button" className="approval-allow" onClick={() => void mergeRun(run.id)}>merge</button>
                              <button type="button" className="approval-deny" onClick={() => void discardRun(run.id)}>discard</button>
                            </span>
                          </div>
                        ))}
                      </>
                    )}
                    {approvals.length === 0 && reviewRuns.length === 0 ? (
                      <div className="run-empty">nothing waiting on you</div>
                    ) : approvals.slice(0, 40).map((approval) => (
                      <div
                        key={approval.id}
                        className={["approval-row", approval.state === "pending" ? "approval-row-pending" : undefined].filter(Boolean).join(" ")}
                      >
                        <button
                          type="button"
                          className="approval-main approval-jump"
                          onClick={() => {
                            const run = runs.find((candidate) => candidate.id === approval.runId);
                            if (run) {
                              selectRun(run);
                            }
                          }}
                        >
                          <span>{approval.toolName} · {approval.runId}</span>
                          <span>{summarizeApprovalInput(approval.input)}</span>
                        </button>
                        {approval.state === "pending" ? (
                          <span className="approval-actions">
                            <button type="button" className="approval-allow" onClick={() => void decideApproval(approval.id, "allowed")}>allow</button>
                            <button type="button" className="approval-always" onClick={() => void decideApproval(approval.id, "allowed", true)}>always</button>
                            <button type="button" className="approval-deny" onClick={() => void decideApproval(approval.id, "denied")}>deny</button>
                          </span>
                        ) : (
                          <span className={`approval-state-${approval.state}`}>
                            {approval.state}{approval.via ? ` ·${approval.via}` : ""}
                          </span>
                        )}
                      </div>
                    ))}
                    {approvalRules.length > 0 && (
                      <>
                        <div className="rule-header">always-allow rules</div>
                        {approvalRules.map((rule) => (
                          <div key={rule.id} className="rule-row">
                            <span className="approval-main">
                              <span>
                                {rule.toolName}
                                {rule.commandPrefix ? ` ${rule.commandPrefix}*` : ""}
                              </span>
                              <span>{rule.workspaceId ? workspaces.find((ws) => ws.id === rule.workspaceId)?.name ?? rule.workspaceId : "all workspaces"}</span>
                            </span>
                            <span className="approval-actions">
                              <button type="button" className="approval-deny" onClick={() => void removeApprovalRule(rule.id)}>x</button>
                            </span>
                          </div>
                        ))}
                      </>
                    )}
                  </div>
                </Panel>
              </Resizable>
            </section>
          </Resizable>

          <section className="demo-main-stack">
            <Resizable className="demo-terminal" axis="y" defaultHeightCells={36} minHeightCells={12} maxHeightCells={46}>
              <Panel
                title={
                  diffView
                    ? `diff :: ${diffView.runId}${diffView.file ? ` · ${diffView.file}` : ""}`
                    : selectedRun ? `terminal :: ${selectedRun.title ?? selectedRun.id} · ${selectedRun.harnessId}` : "channel :: idle"
                }
                focusState={selectedAttention === "waiting" ? "focused" : "active"}
                actions={
                  diffView ? (
                    <>
                      <button type="button" onClick={() => scrollTerminal(-3)} aria-label="scroll up">▲</button>
                      <button type="button" onClick={() => scrollTerminal(3)} aria-label="scroll down">▼</button>
                      <button type="button" onClick={() => setDiffView(null)}>close</button>
                    </>
                  ) : !selectedRunId ? (
                    <>
                      <button type="button" onClick={() => scrollTerminal(-3)} aria-label="scroll up">▲</button>
                      <button type="button" onClick={() => scrollTerminal(3)} aria-label="scroll down">▼</button>
                      <button type="button" onClick={() => setViewport((current) => scrollViewportToTail(current, viewBuffer.lines.length))}>tail</button>
                    </>
                  ) : undefined
                }
                footer={
                  <StatusLine
                    items={[
                      { id: "harness", label: "harness", value: selectedRun?.harnessId ?? selectedHarnessId, tone: "accent" },
                      { id: "run", label: "run", value: selectedRun ? formatRunStatus(selectedRun) : "none", tone: selectedMeta?.status === "running" ? "success" : "muted" },
                      ...(selectedAttention
                        ? [{
                            id: "attn",
                            label: "state",
                            value: selectedAttention === "waiting" ? "needs input" : selectedAttention,
                            tone: attentionTone(selectedAttention)
                          } as const]
                        : []),
                      ...(() => {
                        const runWorkspace = selectedRun
                          ? workspaces.filter((ws) => selectedRun.cwd === ws.path || selectedRun.cwd.startsWith(`${ws.path}/`)).sort((a, b) => b.path.length - a.path.length)[0]
                          : activeWorkspace;
                        return runWorkspace
                          ? [{
                              id: "policy",
                              label: "policy",
                              value: runWorkspace.policy,
                              tone: runWorkspace.policy === "permissive" ? "warning" : runWorkspace.policy === "strict" ? "accent" : "muted"
                            } as const]
                          : [];
                      })(),
                      ...(lastError
                        ? [{ id: "error", label: "error", value: lastError, tone: "danger" } as const]
                        : [])
                    ]}
                  />
                }
              >
                <div className="term-stack">
                  {visibleRuns.length > 0 && !diffView ? (
                    <div className="term-tabs">
                      {visibleRuns.map((run) => {
                        const attention = attentionFor(run.id);
                        return (
                          <button
                            key={run.id}
                            type="button"
                            className={[
                              "term-tab",
                              selectedRunId === run.id ? "term-tab-active" : undefined,
                              attention === "waiting" || attention === "done" || attention === "failed" ? "term-tab-attn" : undefined
                            ].filter(Boolean).join(" ")}
                            onClick={() => selectRun(run)}
                            onContextMenu={(event) => handleRunContextMenu(event, run)}
                            title={run.command.length > 0 ? run.command : run.id}
                          >
                            <span className={`run-dot run-dot-${attention ?? "idle"}`} />
                            <span className="term-tab-label">{run.title ?? run.id}</span>
                          </button>
                        );
                      })}
                    </div>
                  ) : null}
                  <div
                    className={selectedRunId && !diffView ? "xterm-plane xterm-plane-active" : "xterm-plane"}
                    ref={termContainerRef}
                    onContextMenu={(event) => openMenu(event, selectedRun ? `terminal :: ${selectedRun.id}` : "terminal", terminalMenuEntries())}
                  />
                  {!selectedRunId || diffView ? (
                    <div
                      className="terminal-scroll-plane"
                      onWheel={handleTerminalWheel}
                      onKeyDown={handleTerminalKeyDown}
                      tabIndex={0}
                    >
                      <Viewport viewport={viewport} buffer={viewBuffer} theme={micoDarkTheme} showLineNumbers />
                    </div>
                  ) : null}
                </div>
              </Panel>
            </Resizable>

            <SplitPane className="demo-bottom-grid" gapCells={1}>
              <Resizable className="demo-bottom-panel" axis="x" minWidthCells={34}>
                <Panel title="run details">
                  <TextBufferView buffer={detailsBuffer} theme={micoDarkTheme} />
                </Panel>
              </Resizable>
              <Resizable className="demo-bottom-panel" axis="x" minWidthCells={30}>
                <Panel
                  title="activity"
                  footer={
                    <StatusLine
                      items={[{ id: "events", label: "events", value: String(selectedActivity.length), tone: selectedActivity.length > 0 ? "accent" : "muted" }]}
                    />
                  }
                >
                  <div className="activity-list" ref={activityListRef}>
                    {selectedActivity.length === 0 ? (
                      <div className="run-empty">no structured events{selectedRun ? "" : " — select a run"}</div>
                    ) : selectedActivity.map((entry) => (
                      <div key={entry.id} className={`activity-row activity-${entry.kind}`}>
                        <span className="activity-label">{entry.label}</span>
                        {entry.detail && <span className="activity-detail">{entry.detail}</span>}
                      </div>
                    ))}
                  </div>
                </Panel>
              </Resizable>
              <Resizable className="demo-bottom-panel" axis="x" minWidthCells={28}>
                <Panel
                  title="changed files"
                  variant="success"
                  actions={
                    selectedRun ? (
                      <>
                        <button type="button" onClick={() => void openDiff(selectedRun.id, null)}>diff</button>
                        {selectedRun.worktree && selectedMeta?.status !== "running" && (
                          <>
                            <button type="button" onClick={() => void mergeRun(selectedRun.id)}>merge</button>
                            <button type="button" onClick={() => void discardRun(selectedRun.id)}>discard</button>
                          </>
                        )}
                      </>
                    ) : undefined
                  }
                >
                  <ul className="changed-files">
                    {changedFiles.length === 0 ? (
                      <li className="changed-files-empty">no git changes captured</li>
                    ) : changedFiles.map((file) => (
                      <li key={file}>
                        <button type="button" onClick={() => { if (selectedRunId) void openDiff(selectedRunId, file); }}>{file}</button>
                      </li>
                    ))}
                  </ul>
                </Panel>
              </Resizable>
            </SplitPane>
          </section>
        </SplitPane>
        {menu && <ContextMenu x={menu.x} y={menu.y} title={menu.title} entries={menu.entries} onClose={() => setMenu(null)} />}
        {wsBrowser && (
          <div className="ws-browser" role="dialog" aria-label="add workspace">
            <div className="ws-browser-title">add workspace</div>
            <div className="ws-browser-path">{wsBrowser.path}</div>
            <div className="ws-browser-list">
              {wsBrowser.parent && (
                <button type="button" onClick={() => void openWsBrowser(wsBrowser.parent!)}>../</button>
              )}
              {wsBrowser.dirs.length === 0 && <div className="ws-browser-empty">no subfolders</div>}
              {wsBrowser.dirs.map((dir) => (
                <button key={dir} type="button" onClick={() => void openWsBrowser(`${wsBrowser.path}/${dir}`)}>{dir}/</button>
              ))}
            </div>
            <div className="ws-browser-actions">
              <input
                value={newFolderName}
                placeholder="new folder name"
                onChange={(event) => setNewFolderName(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === "Enter") {
                    void makeFolder();
                  } else if (event.key === "Escape") {
                    setWsBrowser(null);
                  }
                }}
                aria-label="new folder name"
              />
              <button type="button" onClick={() => void makeFolder()} disabled={newFolderName.trim().length === 0}>mkdir</button>
            </div>
            <div className="ws-browser-actions">
              <button type="button" className="ws-browser-primary" onClick={() => void addWorkspaceAt(wsBrowser.path)}>add this folder</button>
              <button type="button" onClick={() => setWsBrowser(null)}>cancel</button>
            </div>
          </div>
        )}
      </main>
    </MicoRoot>
  );
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>
);
