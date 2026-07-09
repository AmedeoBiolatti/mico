import type { ReactNode } from "react";
import type { MicoDomProps } from "../types";

export type StatusTone = "neutral" | "accent" | "success" | "warning" | "danger" | "muted";

export type StatusItem = {
  id: string;
  label: ReactNode;
  value?: ReactNode;
  tone?: StatusTone;
};

export type StatusLineProps = MicoDomProps & {
  items: StatusItem[];
};

export function StatusLine({ id, className, style, items }: StatusLineProps) {
  return (
    <div id={id} className={["mico-status-line", className].filter(Boolean).join(" ")} style={style}>
      {items.map((item) => (
        <span key={item.id} className={["mico-status-item", `mico-tone-${item.tone ?? "neutral"}`].join(" ")}>
          <span className="mico-status-label">{item.label}</span>
          {item.value !== undefined && <span className="mico-status-value">{item.value}</span>}
        </span>
      ))}
    </div>
  );
}
