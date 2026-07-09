import { clamp, type Rect } from "./geometry";
import type { TextBuffer, TextLine } from "./text";

export type ViewportState = {
  rect: Rect;
  scrollX: number;
  scrollY: number;
  followTail: boolean;
};

export type VisibleRange = {
  startRow: number;
  endRow: number;
  totalRows: number;
};

export type ViewportSlice = VisibleRange & {
  lines: TextLine[];
};

export function createViewport(rect: Rect): ViewportState {
  return {
    rect,
    scrollX: 0,
    scrollY: 0,
    followTail: true
  };
}

export function visibleRange(viewport: ViewportState, totalRows: number): VisibleRange {
  const height = Math.max(0, Math.floor(viewport.rect.height));
  const maxStart = Math.max(0, totalRows - height);
  const startRow = clamp(Math.floor(viewport.scrollY), 0, maxStart);
  const endRow = clamp(startRow + height, startRow, totalRows);

  return {
    startRow,
    endRow,
    totalRows
  };
}

export function sliceBufferForViewport(buffer: TextBuffer, viewport: ViewportState): ViewportSlice {
  const range = visibleRange(viewport, buffer.lines.length);

  return {
    ...range,
    lines: buffer.lines.slice(range.startRow, range.endRow)
  };
}

export function scrollViewport(viewport: ViewportState, deltaRows: number, totalRows: number): ViewportState {
  const maxScrollY = Math.max(0, totalRows - viewport.rect.height);

  return {
    ...viewport,
    scrollY: clamp(viewport.scrollY + deltaRows, 0, maxScrollY),
    followTail: false
  };
}

export function scrollViewportToTail(viewport: ViewportState, totalRows: number): ViewportState {
  return {
    ...viewport,
    scrollY: Math.max(0, totalRows - viewport.rect.height),
    followTail: true
  };
}
