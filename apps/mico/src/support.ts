import { micoDarkTheme } from "@mico/render-core";
import type { Attention, SupervisorRun } from "./types";

export const supervisorBase = "http://127.0.0.1:4317";

export const xtermTheme = {
  background: micoDarkTheme.background,
  foreground: micoDarkTheme.foreground,
  cursor: micoDarkTheme.foreground,
  selectionBackground: micoDarkTheme.selection,
  black: micoDarkTheme.ansi.black,
  red: micoDarkTheme.ansi.red,
  green: micoDarkTheme.ansi.green,
  yellow: micoDarkTheme.ansi.yellow,
  blue: micoDarkTheme.ansi.blue,
  magenta: micoDarkTheme.ansi.magenta,
  cyan: micoDarkTheme.ansi.cyan,
  white: micoDarkTheme.ansi.white,
  brightBlack: micoDarkTheme.ansi.brightBlack,
  brightRed: micoDarkTheme.ansi.brightRed,
  brightGreen: micoDarkTheme.ansi.brightGreen,
  brightYellow: micoDarkTheme.ansi.brightYellow,
  brightBlue: micoDarkTheme.ansi.brightBlue,
  brightMagenta: micoDarkTheme.ansi.brightMagenta,
  brightCyan: micoDarkTheme.ansi.brightCyan,
  brightWhite: micoDarkTheme.ansi.brightWhite
};

export function ansi(code: string, text: string): string {
  const esc = String.fromCharCode(27);
  return `${esc}[${code}m${text}${esc}[0m`;
}

export function attentionTone(attention: Attention): "success" | "warning" | "danger" | "accent" | "muted" {
  if (attention === "working") {
    return "success";
  }
  if (attention === "waiting") {
    return "warning";
  }
  if (attention === "failed") {
    return "danger";
  }
  if (attention === "done") {
    return "accent";
  }
  return "muted";
}

export function eventNumber(eventId: string): number | null {
  const match = eventId.match(/^evt_(\d+)$/);
  return match ? Number(match[1]) : null;
}

export async function errorMessageFromResponse(response: Response): Promise<string> {
  try {
    const body = await response.json() as { error?: unknown };
    return typeof body.error === "string" ? body.error : `${response.status} ${response.statusText}`;
  } catch {
    return `${response.status} ${response.statusText}`;
  }
}

export function formatRunStatus(run: SupervisorRun): string {
  if (run.status === "exited") {
    return `exited:${run.exitCode ?? "signal"}`;
  }

  return run.status;
}

export function summarizeApprovalInput(input: unknown): string {
  if (input && typeof input === "object" && "command" in input && typeof (input as { command: unknown }).command === "string") {
    return (input as { command: string }).command.slice(0, 80);
  }

  const text = JSON.stringify(input ?? {});
  return text.length > 80 ? `${text.slice(0, 77)}…` : text;
}
