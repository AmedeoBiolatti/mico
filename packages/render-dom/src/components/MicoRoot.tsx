import type { FontProfile, Theme } from "@mico/render-core";
import { defaultFontProfiles, estimateMonospaceMetrics, micoDarkTheme, resolveFontProfile } from "@mico/render-core";
import type { CSSProperties } from "react";
import type { Density, MicoDomProps } from "../types";

export type MicoRootProps = MicoDomProps & {
  theme?: Theme;
  font?: FontProfile;
  density?: Density;
};

export function MicoRoot({
  id,
  className,
  style,
  children,
  theme = micoDarkTheme,
  font = defaultFontProfiles[0]!,
  density = "compact"
}: MicoRootProps) {
  const metrics = estimateMonospaceMetrics(font);
  const resolvedFont = resolveFontProfile(font, metrics);
  const rootStyle = {
    "--mico-bg": theme.background,
    "--mico-fg": theme.foreground,
    "--mico-muted": theme.muted,
    "--mico-border": theme.border,
    "--mico-panel": theme.panel,
    "--mico-panel-active": theme.panelActive,
    "--mico-accent": theme.accent,
    "--mico-danger": theme.danger,
    "--mico-warning": theme.warning,
    "--mico-success": theme.success,
    "--mico-selection": theme.selection,
    "--mico-focus": theme.focus,
    "--mico-font-family": resolvedFont.cssFamily,
    "--mico-font-size": `${font.sizePx}px`,
    "--mico-line-height": `${font.lineHeightPx}px`,
    "--mico-cell-width": `${metrics.cellWidthPx}px`,
    "--mico-cell-height": `${metrics.cellHeightPx}px`,
    "--mico-grid-x": `${metrics.cellWidthPx}px`,
    "--mico-grid-y": `${metrics.cellHeightPx}px`,
    ...style
  } as CSSProperties;

  return (
    <div id={id} className={["mico-root", `mico-density-${density}`, className].filter(Boolean).join(" ")} style={rootStyle}>
      {children}
    </div>
  );
}
