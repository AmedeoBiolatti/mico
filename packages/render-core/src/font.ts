export type FontProfile = {
  id: string;
  family: string;
  sizePx: number;
  lineHeightPx: number;
  weight?: number | string;
  fallbackFamilies: string[];
};

export type FontMetrics = {
  cellWidthPx: number;
  cellHeightPx: number;
  baselinePx?: number;
};

export type ResolvedFont = FontProfile & {
  cssFamily: string;
  cssFont: string;
  metrics?: FontMetrics;
};

export const defaultFontProfiles: FontProfile[] = [
  {
    id: "mico-compact",
    family: "Berkeley Mono",
    sizePx: 13,
    lineHeightPx: 18,
    fallbackFamilies: ["JetBrains Mono", "SFMono-Regular", "Consolas", "monospace"]
  },
  {
    id: "mico-standard",
    family: "JetBrains Mono",
    sizePx: 14,
    lineHeightPx: 20,
    fallbackFamilies: ["Berkeley Mono", "SFMono-Regular", "Consolas", "monospace"]
  }
];

export function resolveFontProfile(profile: FontProfile, metrics?: FontMetrics): ResolvedFont {
  const cssFamily = [profile.family, ...profile.fallbackFamilies].join(", ");
  const weight = profile.weight ?? 400;
  const cssFont = `${weight} ${profile.sizePx}px/${profile.lineHeightPx}px ${cssFamily}`;

  const resolved: ResolvedFont = {
    ...profile,
    cssFamily,
    cssFont
  };

  if (metrics) {
    resolved.metrics = metrics;
  }

  return resolved;
}

export function estimateMonospaceMetrics(profile: FontProfile): FontMetrics {
  return {
    cellWidthPx: Math.ceil(profile.sizePx * 0.62),
    cellHeightPx: profile.lineHeightPx
  };
}
