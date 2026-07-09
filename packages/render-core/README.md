# @mico/render-core

Generic monospace rendering primitives for MiCo.

This package intentionally has no runtime dependencies. It defines the shared model for terminal-like UI surfaces:

- Geometry and constraints.
- Font profiles and monospace metrics.
- Themes and ANSI palettes.
- Styled text lines and buffers.
- ANSI SGR parsing.
- Panels, borders, and viewports.
- A small render-tree model for future DOM, Canvas, terminal, or screenshot renderers.

## Current Scope

This is the foundation layer. It does not render to the DOM yet. The next package should be `@mico/render-dom`, which maps these primitives to React components and CSS.

## Example

```ts
import {
  bufferFromPlainText,
  createViewport,
  micoDarkTheme,
  parseAnsiLine,
  sliceBufferForViewport
} from "@mico/render-core";

const line = parseAnsiLine("\u001b[32mready\u001b[0m waiting for mission", micoDarkTheme);
const buffer = bufferFromPlainText("agent: codex\nstatus: running");
const viewport = createViewport({ x: 0, y: 0, width: 80, height: 24 });
const visible = sliceBufferForViewport(buffer, viewport);
```
