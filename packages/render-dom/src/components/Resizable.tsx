import type { CSSProperties, PointerEvent as ReactPointerEvent } from "react";
import { useRef, useState } from "react";
import type { MicoDomProps } from "../types";

export type ResizeAxis = "x" | "y" | "both";
export type ResizeHandle = "right" | "bottom" | "corner";

export type ResizeSize = {
  width?: number;
  height?: number;
};

export type ResizableProps = MicoDomProps & {
  axis?: ResizeAxis;
  defaultWidthCells?: number;
  defaultHeightCells?: number;
  minWidthCells?: number;
  minHeightCells?: number;
  maxWidthCells?: number;
  maxHeightCells?: number;
  onResize?: (size: ResizeSize) => void;
};

type DragState = {
  handle: ResizeHandle;
  startX: number;
  startY: number;
  startWidth: number;
  startHeight: number;
  cellWidth: number;
  cellHeight: number;
};

const defaultCellWidth = 8;
const defaultCellHeight = 18;

export function Resizable({
  id,
  className,
  style,
  children,
  axis = "both",
  defaultWidthCells,
  defaultHeightCells,
  minWidthCells = 12,
  minHeightCells = 4,
  maxWidthCells,
  maxHeightCells,
  onResize
}: ResizableProps) {
  const elementRef = useRef<HTMLDivElement | null>(null);
  const dragRef = useRef<DragState | null>(null);
  const [size, setSize] = useState<ResizeSize>(() => {
    const initialSize: ResizeSize = {};

    if (defaultWidthCells !== undefined) {
      initialSize.width = defaultWidthCells;
    }

    if (defaultHeightCells !== undefined) {
      initialSize.height = defaultHeightCells;
    }

    return initialSize;
  });
  const [dragging, setDragging] = useState<ResizeHandle | null>(null);

  const canResizeX = axis === "x" || axis === "both";
  const canResizeY = axis === "y" || axis === "both";
  const snappedWidth = size.width !== undefined ? `calc(var(--mico-cell-width) * ${size.width})` : undefined;
  const snappedHeight = size.height !== undefined ? `calc(var(--mico-cell-height) * ${size.height})` : undefined;
  const resolvedStyle = {
    ...style,
    ...(snappedWidth !== undefined ? { width: snappedWidth, flexBasis: snappedWidth, flexGrow: 0 } : {}),
    ...(snappedHeight !== undefined ? { height: snappedHeight } : {})
  } as CSSProperties;

  function startResize(handle: ResizeHandle, event: ReactPointerEvent<HTMLButtonElement>) {
    const element = elementRef.current;

    if (!element) {
      return;
    }

    event.currentTarget.setPointerCapture(event.pointerId);
    const styles = getComputedStyle(element);
    dragRef.current = {
      handle,
      startX: event.clientX,
      startY: event.clientY,
      startWidth: element.offsetWidth,
      startHeight: element.offsetHeight,
      cellWidth: parseCssPixels(styles.getPropertyValue("--mico-cell-width"), defaultCellWidth),
      cellHeight: parseCssPixels(styles.getPropertyValue("--mico-cell-height"), defaultCellHeight)
    };
    setDragging(handle);
  }

  function updateResize(event: ReactPointerEvent<HTMLButtonElement>) {
    const drag = dragRef.current;

    if (!drag) {
      return;
    }

    const next: ResizeSize = { ...size };

    if ((drag.handle === "right" || drag.handle === "corner") && canResizeX) {
      const widthPx = drag.startWidth + event.clientX - drag.startX;
      next.width = clampCells(Math.round(widthPx / drag.cellWidth), minWidthCells, maxWidthCells);
    }

    if ((drag.handle === "bottom" || drag.handle === "corner") && canResizeY) {
      const heightPx = drag.startHeight + event.clientY - drag.startY;
      next.height = clampCells(Math.round(heightPx / drag.cellHeight), minHeightCells, maxHeightCells);
    }

    setSize(next);
    onResize?.(next);
  }

  function stopResize(event: ReactPointerEvent<HTMLButtonElement>) {
    if (dragRef.current) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }

    dragRef.current = null;
    setDragging(null);
  }

  return (
    <div
      id={id}
      ref={elementRef}
      className={["mico-resizable", dragging ? "mico-resizing" : undefined, className].filter(Boolean).join(" ")}
      style={resolvedStyle}
      data-resize-axis={axis}
    >
      <div className="mico-resizable-content">{children}</div>
      {canResizeX && (
        <ResizeButton handle="right" dragging={dragging === "right"} onStart={startResize} onMove={updateResize} onStop={stopResize} />
      )}
      {canResizeY && (
        <ResizeButton handle="bottom" dragging={dragging === "bottom"} onStart={startResize} onMove={updateResize} onStop={stopResize} />
      )}
      {canResizeX && canResizeY && (
        <ResizeButton handle="corner" dragging={dragging === "corner"} onStart={startResize} onMove={updateResize} onStop={stopResize} />
      )}
    </div>
  );
}

type ResizeButtonProps = {
  handle: ResizeHandle;
  dragging: boolean;
  onStart: (handle: ResizeHandle, event: ReactPointerEvent<HTMLButtonElement>) => void;
  onMove: (event: ReactPointerEvent<HTMLButtonElement>) => void;
  onStop: (event: ReactPointerEvent<HTMLButtonElement>) => void;
};

function ResizeButton({ handle, dragging, onStart, onMove, onStop }: ResizeButtonProps) {
  return (
    <button
      type="button"
      className={["mico-resize-handle", `mico-resize-${handle}`, dragging ? "mico-resize-dragging" : undefined].filter(Boolean).join(" ")}
      aria-label={`resize ${handle}`}
      onPointerDown={(event) => onStart(handle, event)}
      onPointerMove={onMove}
      onPointerUp={onStop}
      onPointerCancel={onStop}
    />
  );
}

function parseCssPixels(value: string, fallback: number): number {
  const parsed = Number.parseFloat(value);

  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

function clampCells(value: number, min: number, max?: number): number {
  const lowerBound = Math.max(min, value);

  return max === undefined ? lowerBound : Math.min(lowerBound, max);
}
