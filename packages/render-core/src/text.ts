import type { Theme } from "./theme";

export type TextWeight = "normal" | "bold";

export type TextStyle = {
  foreground?: string;
  background?: string;
  weight?: TextWeight;
  italic?: boolean;
  underline?: boolean;
  inverse?: boolean;
  dim?: boolean;
};

export type TextSpan = {
  text: string;
  style?: TextStyle;
};

export type TextLine = {
  spans: TextSpan[];
};

export type TextBuffer = {
  lines: TextLine[];
};

export type TextRenderOptions = {
  width: number;
  wrap?: boolean;
  truncate?: boolean;
  ellipsis?: string;
};

export const emptyTextBuffer: TextBuffer = {
  lines: []
};

export function lineFromText(text: string, style?: TextStyle): TextLine {
  return {
    spans: [createTextSpan(text, style)]
  };
}

export function bufferFromPlainText(text: string, style?: TextStyle): TextBuffer {
  return {
    lines: text.split(/\r?\n/).map((line) => lineFromText(line, style))
  };
}

export function textLineLength(line: TextLine): number {
  return line.spans.reduce((length, span) => length + span.text.length, 0);
}

export function flattenTextLine(line: TextLine): string {
  return line.spans.map((span) => span.text).join("");
}

export function truncateTextLine(line: TextLine, width: number, ellipsis = "…"): TextLine {
  if (width <= 0) {
    return { spans: [] };
  }

  if (textLineLength(line) <= width) {
    return line;
  }

  const marker = width >= ellipsis.length ? ellipsis : "";
  const target = Math.max(0, width - marker.length);
  const spans: TextSpan[] = [];
  let remaining = target;

  for (const span of line.spans) {
    if (remaining <= 0) {
      break;
    }

    const text = span.text.slice(0, remaining);
    spans.push(createTextSpan(text, span.style));
    remaining -= text.length;
  }

  if (marker.length > 0) {
    spans.push(createTextSpan(marker, line.spans.at(-1)?.style));
  }

  return { spans };
}

export function createTextSpan(text: string, style?: TextStyle): TextSpan {
  return style ? { text, style } : { text };
}

export function resolveTextStyle(style: TextStyle | undefined, theme: Theme): Required<TextStyle> {
  const inverse = style?.inverse ?? false;
  const foreground = style?.foreground ?? theme.foreground;
  const background = style?.background ?? "transparent";

  return {
    foreground: inverse ? background : foreground,
    background: inverse ? foreground : background,
    weight: style?.weight ?? "normal",
    italic: style?.italic ?? false,
    underline: style?.underline ?? false,
    inverse,
    dim: style?.dim ?? false
  };
}
