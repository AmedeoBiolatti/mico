import type { CSSProperties, ReactNode } from "react";

export type MicoDomProps = {
  id?: string;
  className?: string;
  style?: CSSProperties;
  children?: ReactNode;
};

export type Density = "compact" | "standard" | "spacious";

export type FocusState = "idle" | "focused" | "active";
