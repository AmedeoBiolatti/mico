import { createTextSpan, type TextLine, type TextSpan, type TextStyle } from "./text";
import type { Theme } from "./theme";

const escapePattern = /\x1b\[([0-9;]*)m/g;

type MutableStyle = TextStyle;

export function parseAnsiLine(input: string, theme: Theme): TextLine {
  const spans: TextSpan[] = [];
  let style: MutableStyle = {};
  let cursor = 0;
  let match: RegExpExecArray | null;

  while ((match = escapePattern.exec(input)) !== null) {
    if (match.index > cursor) {
      spans.push(createTextSpan(input.slice(cursor, match.index), { ...style }));
    }

    style = applyAnsiCodes(style, parseCodes(match[1] ?? ""), theme);
    cursor = match.index + match[0].length;
  }

  if (cursor < input.length) {
    spans.push(createTextSpan(input.slice(cursor), { ...style }));
  }

  return { spans };
}

export function parseAnsiText(input: string, theme: Theme): TextLine[] {
  return input.split(/\r?\n/).map((line) => parseAnsiLine(line, theme));
}

function parseCodes(value: string): number[] {
  if (value.length === 0) {
    return [0];
  }

  return value.split(";").map((code) => Number.parseInt(code, 10)).filter(Number.isFinite);
}

function applyAnsiCodes(current: MutableStyle, codes: number[], theme: Theme): MutableStyle {
  let next: MutableStyle = { ...current };

  for (const code of codes) {
    if (code === 0) {
      next = {};
    } else if (code === 1) {
      next.weight = "bold";
    } else if (code === 2) {
      next.dim = true;
    } else if (code === 3) {
      next.italic = true;
    } else if (code === 4) {
      next.underline = true;
    } else if (code === 7) {
      next.inverse = true;
    } else if (code === 22) {
      next.weight = "normal";
      next.dim = false;
    } else if (code === 23) {
      next.italic = false;
    } else if (code === 24) {
      next.underline = false;
    } else if (code === 27) {
      next.inverse = false;
    } else if (code === 39) {
      delete next.foreground;
    } else if (code === 49) {
      delete next.background;
    } else {
      const foreground = foregroundByCode(code, theme);
      const background = backgroundByCode(code, theme);

      if (foreground) {
        next.foreground = foreground;
      } else if (background) {
        next.background = background;
      }
    }
  }

  return next;
}

function foregroundByCode(code: number, theme: Theme): string | undefined {
  const palette = theme.ansi;
  const colors: Record<number, string> = {
    30: palette.black,
    31: palette.red,
    32: palette.green,
    33: palette.yellow,
    34: palette.blue,
    35: palette.magenta,
    36: palette.cyan,
    37: palette.white,
    90: palette.brightBlack,
    91: palette.brightRed,
    92: palette.brightGreen,
    93: palette.brightYellow,
    94: palette.brightBlue,
    95: palette.brightMagenta,
    96: palette.brightCyan,
    97: palette.brightWhite
  };

  return colors[code];
}

function backgroundByCode(code: number, theme: Theme): string | undefined {
  const foreground = foregroundByCode(code - 10, theme);

  if (foreground) {
    return foreground;
  }

  return foregroundByCode(code - 60, theme);
}
