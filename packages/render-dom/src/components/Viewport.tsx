import { micoDarkTheme, sliceBufferForViewport, type TextBuffer, type Theme, type ViewportState } from "@mico/render-core";
import type { MicoDomProps } from "../types";
import { TextBufferView } from "./Text";

export type ViewportProps = MicoDomProps & {
  viewport: ViewportState;
  buffer: TextBuffer;
  theme?: Theme;
  showLineNumbers?: boolean;
};

export function Viewport({
  id,
  className,
  style,
  viewport,
  buffer,
  theme = micoDarkTheme,
  showLineNumbers = false
}: ViewportProps) {
  const slice = sliceBufferForViewport(buffer, viewport);

  return (
    <div
      id={id}
      className={["mico-viewport", viewport.followTail ? "mico-viewport-tail" : undefined, className].filter(Boolean).join(" ")}
      style={style}
      data-start-row={slice.startRow}
      data-end-row={slice.endRow}
      data-total-rows={slice.totalRows}
    >
      <TextBufferView
        buffer={{ lines: slice.lines }}
        theme={theme}
        startLineNumber={slice.startRow + 1}
        showLineNumbers={showLineNumbers}
      />
    </div>
  );
}
