import { accessSync, constants, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { delimiter, join } from "node:path";
import { cwd as processCwd, env as processEnv, execPath } from "node:process";
import { fileURLToPath } from "node:url";
import { spawnSessionProcess, type SessionProcess } from "./process.js";
import type { HarnessDescriptor, HarnessId, PolicyId, StartRunRequest } from "./types.js";

export type StructuredFormat = "claude-stream-json";

export type LaunchResult = {
  process: SessionProcess;
  command: string;
  cwd: string;
  structured?: StructuredFormat;
};

export interface HarnessAdapter {
  descriptor: HarnessDescriptor;
  launch(request: StartRunRequest): LaunchResult;
}

type CommandHarnessOptions = {
  id: HarnessId;
  displayName: string;
  binary: string;
  args?: string[];
  promptMode?: "shell-command" | "stdin" | "arg";
  policyArgs?: Partial<Record<PolicyId, string[]>>;
  extraArgsForRun?: (context: { runId: string }) => string[];
  structured?: StructuredFormat;
};

export class CommandHarnessAdapter implements HarnessAdapter {
  readonly descriptor: HarnessDescriptor;
  readonly #binary: string;
  readonly #args: string[];
  readonly #promptMode: NonNullable<CommandHarnessOptions["promptMode"]>;
  readonly #policyArgs: Partial<Record<PolicyId, string[]>>;
  readonly #extraArgsForRun: CommandHarnessOptions["extraArgsForRun"];
  readonly #structured: StructuredFormat | undefined;

  constructor(options: CommandHarnessOptions) {
    const binaryPath = resolveCommand(options.binary);
    this.#binary = binaryPath ?? options.binary;
    this.#args = options.args ?? [];
    this.#promptMode = options.promptMode ?? "arg";
    this.#policyArgs = options.policyArgs ?? {};
    this.#extraArgsForRun = options.extraArgsForRun;
    this.#structured = options.structured;
    this.descriptor = {
      id: options.id,
      displayName: options.displayName,
      command: this.#binary,
      available: binaryPath !== null,
      supportsInput: true,
      supportsResize: true,
      supportsPty: true
    };
  }

  launch(request: StartRunRequest): LaunchResult {
    if (!this.descriptor.available) {
      throw new Error(`Harness is not available: ${this.descriptor.id}`);
    }

    const cwd = request.cwd ?? processCwd();
    const policyArgs = request.policy ? this.#policyArgs[request.policy] ?? [] : [];
    const extraArgs = request.runId && this.#extraArgsForRun ? this.#extraArgsForRun({ runId: request.runId }) : [];
    const args = [...policyArgs, ...extraArgs, ...this.#launchArgs(request.command)];
    const process = spawnSessionProcess({
      command: this.#binary,
      args,
      cwd,
      env: {
        ...processEnv,
        ...request.env
      },
      ...(request.cols !== undefined ? { cols: request.cols } : {}),
      ...(request.rows !== undefined ? { rows: request.rows } : {})
    });

    if (this.#promptMode === "stdin" && request.command.length > 0) {
      process.write(`${request.command}\r`);
    }

    return {
      process,
      command: request.command,
      cwd,
      ...(this.#structured !== undefined ? { structured: this.#structured } : {})
    };
  }

  #launchArgs(command: string): string[] {
    if (this.#promptMode === "shell-command") {
      // Empty command opens an interactive shell session.
      return command.length > 0 ? ["-lc", command] : ["-il"];
    }

    if (this.#promptMode === "arg" && command.length > 0) {
      return [...this.#args, command];
    }

    return [...this.#args];
  }
}

export class ShellHarnessAdapter extends CommandHarnessAdapter {
  constructor() {
    super({
      id: "shell",
      displayName: "Shell",
      binary: processEnv.SHELL ?? "/bin/bash",
      promptMode: "shell-command"
    });
  }
}

export function defaultHarnesses(): HarnessAdapter[] {
  return [
    new ShellHarnessAdapter(),
    new CommandHarnessAdapter({
      id: "codex",
      displayName: "Codex Exec",
      binary: "codex",
      args: ["exec", "--skip-git-repo-check"],
      promptMode: "arg",
      policyArgs: {
        permissive: ["--sandbox", "workspace-write"],
        strict: ["--sandbox", "read-only"]
      }
    }),
    new CommandHarnessAdapter({
      id: "codex-interactive",
      displayName: "Codex Interactive",
      binary: "codex",
      args: ["--no-alt-screen", "-c", "check_for_update_on_startup=false", "-c", "update_on_startup=false"],
      promptMode: "stdin",
      policyArgs: {
        permissive: ["--full-auto"],
        strict: ["--sandbox", "read-only"]
      }
    }),
    new CommandHarnessAdapter({
      id: "claude-task",
      displayName: "Claude Task (brokered)",
      binary: "claude",
      args: ["-p", "--output-format", "stream-json", "--verbose"],
      promptMode: "arg",
      structured: "claude-stream-json",
      extraArgsForRun: ({ runId }) => {
        const configPath = join(tmpdir(), `mico-mcp-${runId}.json`);
        writeFileSync(configPath, JSON.stringify({
          mcpServers: {
            mico: {
              command: execPath,
              args: [approvalMcpScriptPath()],
              env: {
                MICO_RUN_ID: runId,
                MICO_BASE: processEnv.MICO_BASE ?? "http://127.0.0.1:4317"
              }
            }
          }
        }));
        return ["--mcp-config", configPath, "--permission-prompt-tool", "mcp__mico__approve"];
      }
    }),
    new CommandHarnessAdapter({
      id: "claude-code",
      displayName: "Claude Code",
      binary: "claude",
      promptMode: "arg",
      policyArgs: {
        permissive: ["--permission-mode", "acceptEdits"],
        strict: ["--permission-mode", "plan"]
      }
    })
  ];
}

function approvalMcpScriptPath(): string {
  if (processEnv.MICO_APPROVAL_MCP) {
    return processEnv.MICO_APPROVAL_MCP;
  }

  // source layout: packages/supervisor/src/harnesses.ts → ../scripts
  // dist layout:   dist/types/packages/supervisor/src/harnesses.js → repo root ../../../../../packages/supervisor/scripts
  const fromSource = fileURLToPath(new URL("../scripts/approval-mcp.mjs", import.meta.url));
  const fromDist = fileURLToPath(new URL("../../../../../packages/supervisor/scripts/approval-mcp.mjs", import.meta.url));
  try {
    accessSync(fromSource, constants.R_OK);
    return fromSource;
  } catch {
    return fromDist;
  }
}

function resolveCommand(command: string): string | null {
  if (command.includes("/")) {
    return isExecutable(command) ? command : null;
  }

  for (const directory of (processEnv.PATH ?? "").split(delimiter)) {
    const candidate = `${directory}/${command}`;
    if (isExecutable(candidate)) {
      return candidate;
    }
  }

  return null;
}

function isExecutable(path: string): boolean {
  try {
    accessSync(path, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}
