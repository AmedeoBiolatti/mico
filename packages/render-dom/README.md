# @mico/render-dom

React DOM components for MiCo's monospace rendering system.

This package maps `@mico/render-core` primitives into practical UI pieces:

- `MicoRoot` for theme, density, and font CSS variables.
- `Panel` for framed terminal-like regions.
- `SplitPane` for horizontal and vertical layouts.
- `Viewport` for visible slices of larger text buffers.
- `TextBufferView`, `TextLineView`, and `TextSpanView` for styled monospace text.
- `StatusLine` for dense operational state.

Import the stylesheet once in the app entrypoint:

```ts
import "@mico/render-dom/src/styles.css";
```

## Example

```tsx
import { bufferFromPlainText, createViewport } from "@mico/render-core";
import { MicoRoot, Panel, StatusLine, Viewport } from "@mico/render-dom";

const buffer = bufferFromPlainText("agent: codex\nstatus: running");
const viewport = createViewport({ x: 0, y: 0, width: 80, height: 10 });

export function Demo() {
  return (
    <MicoRoot>
      <Panel title="mission/control" footer={<StatusLine items={[{ id: "state", label: "state", value: "active", tone: "success" }]} />}>
        <Viewport viewport={viewport} buffer={buffer} showLineNumbers />
      </Panel>
    </MicoRoot>
  );
}
```
