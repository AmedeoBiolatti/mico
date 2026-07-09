export type ParsedAgentEvent = {
  kind: "init" | "text" | "tool_use" | "tool_result" | "result" | "error" | "system";
  label: string;
  detail: string | null;
  text: string;
  /** Present on init events: the agent's own session id. */
  sessionId?: string;
};

const esc = String.fromCharCode(27);

function color(code: string, text: string): string {
  return `${esc}[${code}m${text}${esc}[0m`;
}

function truncate(text: string, max: number): string {
  const flat = text.replace(/\s+/g, " ").trim();
  return flat.length > max ? `${flat.slice(0, max - 1)}…` : flat;
}

function summarizeToolInput(input: unknown): string {
  if (!input || typeof input !== "object") {
    return "";
  }

  const record = input as Record<string, unknown>;
  for (const key of ["command", "file_path", "path", "pattern", "url", "description", "prompt"]) {
    if (typeof record[key] === "string" && (record[key] as string).length > 0) {
      return truncate(record[key] as string, 90);
    }
  }

  return truncate(JSON.stringify(record), 90);
}

function toolResultText(content: unknown): string {
  if (typeof content === "string") {
    return content;
  }

  if (Array.isArray(content)) {
    return content
      .map((block) => (block && typeof block === "object" && typeof (block as { text?: unknown }).text === "string" ? (block as { text: string }).text : ""))
      .join("\n");
  }

  return "";
}

// Parse one line of `claude -p --output-format stream-json` output.
// Returns [] for ignorable events and null when the line is not stream-json.
export function parseClaudeStreamLine(line: string): ParsedAgentEvent[] | null {
  let payload: Record<string, unknown>;
  try {
    payload = JSON.parse(line) as Record<string, unknown>;
  } catch {
    return null;
  }

  if (!payload || typeof payload !== "object" || typeof payload.type !== "string") {
    return null;
  }

  if (payload.type === "system") {
    if (payload.subtype === "init") {
      const model = typeof payload.model === "string" ? payload.model : "unknown model";
      const sessionId = typeof payload.session_id === "string" ? payload.session_id : undefined;
      return [{
        kind: "init",
        label: `session started · ${model}`,
        detail: null,
        text: `${color("36", "[agent]")} session started · ${model}\n`,
        ...(sessionId !== undefined ? { sessionId } : {})
      }];
    }
    return [];
  }

  if (payload.type === "assistant") {
    const message = payload.message as { content?: unknown } | undefined;
    const blocks = Array.isArray(message?.content) ? message.content : [];
    const events: ParsedAgentEvent[] = [];
    for (const block of blocks) {
      if (!block || typeof block !== "object") {
        continue;
      }
      const typed = block as { type?: string; text?: string; name?: string; input?: unknown };
      if (typed.type === "text" && typeof typed.text === "string" && typed.text.trim().length > 0) {
        events.push({
          kind: "text",
          label: truncate(typed.text, 90),
          detail: null,
          text: `${typed.text.trimEnd()}\n`
        });
      } else if (typed.type === "tool_use" && typeof typed.name === "string") {
        const summary = summarizeToolInput(typed.input);
        events.push({
          kind: "tool_use",
          label: typed.name,
          detail: summary || null,
          text: `${color("33", "[tool]")} ${typed.name}${summary ? ` · ${summary}` : ""}\n`
        });
      }
    }
    return events;
  }

  if (payload.type === "user") {
    const message = payload.message as { content?: unknown } | undefined;
    const blocks = Array.isArray(message?.content) ? message.content : [];
    const events: ParsedAgentEvent[] = [];
    for (const block of blocks) {
      if (!block || typeof block !== "object" || (block as { type?: string }).type !== "tool_result") {
        continue;
      }
      const typed = block as { content?: unknown; is_error?: boolean };
      const resultText = truncate(toolResultText(typed.content), 110);
      if (typed.is_error) {
        events.push({
          kind: "error",
          label: "tool error",
          detail: resultText || null,
          text: `${color("31", "[error]")} ${resultText}\n`
        });
      } else if (resultText.length > 0) {
        events.push({
          kind: "tool_result",
          label: "result",
          detail: resultText,
          text: `${color("90", `  → ${resultText}`)}\n`
        });
      }
    }
    return events;
  }

  if (payload.type === "result") {
    const subtype = typeof payload.subtype === "string" ? payload.subtype : "done";
    const turns = typeof payload.num_turns === "number" ? `${payload.num_turns} turns` : null;
    const cost = typeof payload.total_cost_usd === "number" ? `$${payload.total_cost_usd.toFixed(4)}` : null;
    const label = [subtype, turns, cost].filter(Boolean).join(" · ");
    const resultText = typeof payload.result === "string" ? payload.result : "";
    const ok = subtype === "success";
    return [{
      kind: ok ? "result" : "error",
      label,
      detail: resultText.length > 0 ? truncate(resultText, 110) : null,
      text: `${color(ok ? "32" : "31", "[done]")} ${label}\n`
    }];
  }

  return [];
}
