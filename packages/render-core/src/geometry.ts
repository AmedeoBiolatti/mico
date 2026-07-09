export type Axis = "horizontal" | "vertical";

export type Size = {
  width: number;
  height: number;
};

export type Point = {
  x: number;
  y: number;
};

export type Rect = Point & Size;

export type Insets = {
  top: number;
  right: number;
  bottom: number;
  left: number;
};

export type Constraint = {
  minWidth?: number;
  maxWidth?: number;
  minHeight?: number;
  maxHeight?: number;
};

export const zeroInsets: Insets = {
  top: 0,
  right: 0,
  bottom: 0,
  left: 0
};

export function insetRect(rect: Rect, insets: Insets): Rect {
  const width = Math.max(0, rect.width - insets.left - insets.right);
  const height = Math.max(0, rect.height - insets.top - insets.bottom);

  return {
    x: rect.x + insets.left,
    y: rect.y + insets.top,
    width,
    height
  };
}

export function clamp(value: number, min: number, max: number): number {
  if (max < min) {
    return min;
  }

  return Math.min(max, Math.max(min, value));
}

export function clampRectToSize(rect: Rect, size: Size): Rect {
  const x = clamp(rect.x, 0, size.width);
  const y = clamp(rect.y, 0, size.height);
  const width = clamp(rect.width, 0, size.width - x);
  const height = clamp(rect.height, 0, size.height - y);

  return { x, y, width, height };
}

export function normalizeInsets(value?: Partial<Insets> | number): Insets {
  if (typeof value === "number") {
    return {
      top: value,
      right: value,
      bottom: value,
      left: value
    };
  }

  return {
    top: value?.top ?? 0,
    right: value?.right ?? 0,
    bottom: value?.bottom ?? 0,
    left: value?.left ?? 0
  };
}
