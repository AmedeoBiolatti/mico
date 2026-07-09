import { spawn as spawnChild, type ChildProcessWithoutNullStreams } from "node:child_process";
import type { IPty } from "node-pty";
import { spawn as spawnPty } from "node-pty";

export type ProcessExit = {
  exitCode: number | null;
  signal: NodeJS.Signals | null;
};

export type SessionProcess = {
  pid: number | undefined;
  kind: "pty" | "pipe";
  write(input: string): void;
  resize(cols: number, rows: number): void;
  kill(signal?: NodeJS.Signals): void;
  onData(listener: (chunk: string) => void): void;
  onExit(listener: (exit: ProcessExit) => void): void;
  onError(listener: (error: Error) => void): void;
};

export type SpawnOptions = {
  command: string;
  args: string[];
  cwd: string;
  env: NodeJS.ProcessEnv;
  cols?: number;
  rows?: number;
};

export function spawnSessionProcess(options: SpawnOptions): SessionProcess {
  return new PtySessionProcess(options);
}

class PtySessionProcess implements SessionProcess {
  readonly kind = "pty";
  readonly #pty: IPty;
  readonly #exitListeners = new Set<(exit: ProcessExit) => void>();
  readonly #errorListeners = new Set<(error: Error) => void>();

  constructor(options: SpawnOptions) {
    this.#pty = spawnPty(options.command, options.args, {
      cwd: options.cwd,
      env: options.env,
      cols: options.cols ?? 100,
      rows: options.rows ?? 30,
      name: "xterm-256color"
    });
    this.#pty.onExit((event) => {
      for (const listener of this.#exitListeners) {
        listener({ exitCode: event.exitCode, signal: null });
      }
    });
  }

  get pid(): number | undefined {
    return this.#pty.pid;
  }

  write(input: string): void {
    this.#pty.write(input);
  }

  resize(cols: number, rows: number): void {
    this.#pty.resize(cols, rows);
  }

  kill(signal: NodeJS.Signals = "SIGTERM"): void {
    this.#pty.kill(signal);
  }

  onData(listener: (chunk: string) => void): void {
    this.#pty.onData(listener);
  }

  onExit(listener: (exit: ProcessExit) => void): void {
    this.#exitListeners.add(listener);
  }

  onError(listener: (error: Error) => void): void {
    this.#errorListeners.add(listener);
  }
}

export function spawnPipeSessionProcess(options: SpawnOptions): SessionProcess {
  return new PipeSessionProcess(options);
}

class PipeSessionProcess implements SessionProcess {
  readonly kind = "pipe";
  readonly #process: ChildProcessWithoutNullStreams;

  constructor(options: SpawnOptions) {
    this.#process = spawnChild(options.command, options.args, {
      cwd: options.cwd,
      env: options.env,
      stdio: "pipe"
    });
  }

  get pid(): number | undefined {
    return this.#process.pid;
  }

  write(input: string): void {
    this.#process.stdin.write(input);
  }

  resize(): void {
    // Pipes do not have terminal dimensions.
  }

  kill(signal: NodeJS.Signals = "SIGTERM"): void {
    this.#process.kill(signal);
  }

  onData(listener: (chunk: string) => void): void {
    this.#process.stdout.on("data", (chunk: Buffer) => listener(chunk.toString("utf8")));
    this.#process.stderr.on("data", (chunk: Buffer) => listener(chunk.toString("utf8")));
  }

  onExit(listener: (exit: ProcessExit) => void): void {
    this.#process.on("exit", (exitCode, signal) => listener({ exitCode, signal }));
  }

  onError(listener: (error: Error) => void): void {
    this.#process.on("error", listener);
  }
}
