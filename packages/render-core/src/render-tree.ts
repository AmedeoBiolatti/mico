import type { Rect } from "./geometry";
import type { TextLine } from "./text";

export type RenderNodeKind = "box" | "text" | "panel" | "viewport";

export type RenderNodeBase = {
  id?: string;
  kind: RenderNodeKind;
  rect: Rect;
  foreground?: string;
  background?: string;
};

export type BoxNode = RenderNodeBase & {
  kind: "box";
  children: RenderNode[];
};

export type TextNode = RenderNodeBase & {
  kind: "text";
  lines: TextLine[];
};

export type PanelNode = RenderNodeBase & {
  kind: "panel";
  title?: string;
  focused: boolean;
  children: RenderNode[];
};

export type ViewportNode = RenderNodeBase & {
  kind: "viewport";
  scrollY: number;
  totalRows: number;
  children: RenderNode[];
};

export type RenderNode = BoxNode | TextNode | PanelNode | ViewportNode;

export function createBox(rect: Rect, children: RenderNode[] = []): BoxNode {
  return {
    kind: "box",
    rect,
    children
  };
}
