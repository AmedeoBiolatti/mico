import { micoDarkTheme, resolveTextStyle, type TextBuffer, type TextLine, type TextSpan, type TextStyle, type Theme } from "@mico/render-core";
import type { CSSProperties } from "react";
import type { MicoDomProps } from "../types";

export type TextSpanViewProps = {
  span: TextSpan;
  theme?: Theme;
};

export type TextLineViewProps = {
  line: TextLine;
  theme?: Theme;
  lineNumber?: number | undefined;
};

export type TextBufferViewProps = MicoDomProps & {
  buffer: TextBuffer;
  theme?: Theme;
  startLineNumber?: number;
  showLineNumbers?: boolean;
};

export function TextSpanView({ span, theme = micoDarkTheme }: TextSpanViewProps) {
  const style = styleForText(span.style, theme);
  return <span style={style}>{span.text}</span>;
}

export function TextLineView({ line, theme = micoDarkTheme, lineNumber }: TextLineViewProps) {
  return (
    <div className="mico-text-line">
      {lineNumber !== undefined && <span className="mico-text-gutter">{lineNumber}</span>}
      <span className="mico-text-content">
        {line.spans.map((span, index) => (
          <TextSpanView key={index} span={span} theme={theme} />
        ))}
      </span>
    </div>
  );
}

export function TextBufferView({
  id,
  className,
  style,
  buffer,
  theme = micoDarkTheme,
  startLineNumber = 1,
  showLineNumbers = false
}: TextBufferViewProps) {
  return (
    <div id={id} className={["mico-text-buffer", className].filter(Boolean).join(" ")} style={style}>
      {buffer.lines.map((line, index) => (
        <TextLineView
          key={index}
          line={line}
          theme={theme}
          lineNumber={showLineNumbers ? startLineNumber + index : undefined}
        />
      ))}
    </div>
  );
}

function styleForText(style: TextStyle | undefined, theme: Theme): CSSProperties {
  const resolved = resolveTextStyle(style, theme);

  return {
    color: resolved.foreground,
    backgroundColor: resolved.background,
    fontWeight: resolved.weight === "bold" ? 700 : 400,
    fontStyle: resolved.italic ? "italic" : "normal",
    textDecoration: resolved.underline ? "underline" : "none",
    opacity: resolved.dim ? 0.72 : 1
  };
}
