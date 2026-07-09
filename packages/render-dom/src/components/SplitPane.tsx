import type { Axis } from "@mico/render-core";
import type { CSSProperties } from "react";
import type { MicoDomProps } from "../types";

export type SplitPaneProps = MicoDomProps & {
  axis?: Axis;
  gapCells?: number;
};

export function SplitPane({ id, className, style, axis = "horizontal", gapCells = 1, children }: SplitPaneProps) {
  return (
    <div
      id={id}
      className={["mico-split-pane", `mico-split-${axis}`, className].filter(Boolean).join(" ")}
      style={{
        "--mico-split-gap": `calc(var(--mico-grid-x) * ${gapCells})`,
        ...style
      } as CSSProperties}
    >
      {children}
    </div>
  );
}
