import { singleBorder, type BorderStyle } from "./border";
import { insetRect, normalizeInsets, type Insets, type Rect } from "./geometry";
import type { PanelNode, RenderNode } from "./render-tree";

export type PanelVariant = "default" | "focused" | "muted" | "danger" | "warning" | "success";

export type PanelOptions = {
  id?: string;
  rect: Rect;
  title?: string;
  variant?: PanelVariant;
  border?: boolean;
  borderStyle?: BorderStyle;
  padding?: Partial<Insets> | number;
  children?: RenderNode[];
};

export type PanelLayout = {
  outer: Rect;
  content: Rect;
  border: boolean;
  padding: Insets;
  borderStyle: BorderStyle;
};

export function layoutPanel(options: PanelOptions): PanelLayout {
  const hasBorder = options.border ?? true;
  const borderInsets = hasBorder ? normalizeInsets(1) : normalizeInsets(0);
  const padding = normalizeInsets(options.padding ?? 0);
  const content = insetRect(insetRect(options.rect, borderInsets), padding);

  return {
    outer: options.rect,
    content,
    border: hasBorder,
    padding,
    borderStyle: options.borderStyle ?? singleBorder
  };
}

export function createPanel(options: PanelOptions): PanelNode {
  const node: PanelNode = {
    kind: "panel",
    rect: options.rect,
    focused: options.variant === "focused",
    children: options.children ?? []
  };

  if (options.id) {
    node.id = options.id;
  }

  if (options.title) {
    node.title = options.title;
  }

  return node;
}
