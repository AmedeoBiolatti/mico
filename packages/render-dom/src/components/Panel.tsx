import type { PanelVariant } from "@mico/render-core";
import type { ReactNode } from "react";
import type { FocusState, MicoDomProps } from "../types";

export type PanelProps = MicoDomProps & {
  title?: ReactNode;
  actions?: ReactNode;
  footer?: ReactNode;
  variant?: PanelVariant;
  focusState?: FocusState;
  borderless?: boolean;
};

export function Panel({
  id,
  className,
  style,
  title,
  actions,
  footer,
  variant = "default",
  focusState = "idle",
  borderless = false,
  children
}: PanelProps) {
  return (
    <section
      id={id}
      className={[
        "mico-panel",
        `mico-panel-${variant}`,
        `mico-focus-${focusState}`,
        borderless ? "mico-panel-borderless" : undefined,
        className
      ]
        .filter(Boolean)
        .join(" ")}
      style={style}
    >
      {(title || actions) && (
        <header className="mico-panel-header">
          <div className="mico-panel-title">{title}</div>
          {actions && <div className="mico-panel-actions">{actions}</div>}
        </header>
      )}
      <div className="mico-panel-body">{children}</div>
      {footer && (
        <footer className="mico-panel-footer">
          <span className="mico-panel-footer-content">{footer}</span>
        </footer>
      )}
    </section>
  );
}
