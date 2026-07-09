import { useEffect, useRef } from "react";
import type { ReactNode } from "react";

export type MenuEntry =
  | { kind?: "item"; id: string; label: ReactNode; hint?: ReactNode; disabled?: boolean; danger?: boolean; onSelect: () => void }
  | { kind: "separator"; id: string };

export type ContextMenuProps = {
  x: number;
  y: number;
  title?: ReactNode;
  entries: MenuEntry[];
  onClose: () => void;
};

export function ContextMenu({ x, y, title, entries, onClose }: ContextMenuProps) {
  const menuRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    function handlePointer(event: MouseEvent) {
      if (menuRef.current && !menuRef.current.contains(event.target as Node)) {
        onClose();
      }
    }
    function handleKey(event: KeyboardEvent) {
      if (event.key === "Escape") {
        onClose();
      }
    }
    window.addEventListener("mousedown", handlePointer);
    window.addEventListener("keydown", handleKey);
    window.addEventListener("blur", onClose);

    return () => {
      window.removeEventListener("mousedown", handlePointer);
      window.removeEventListener("keydown", handleKey);
      window.removeEventListener("blur", onClose);
    };
  }, [onClose]);

  // Keep the menu inside the viewport.
  useEffect(() => {
    const element = menuRef.current;
    if (!element) {
      return;
    }

    const rect = element.getBoundingClientRect();
    if (rect.right > window.innerWidth) {
      element.style.left = `${Math.max(0, window.innerWidth - rect.width - 8)}px`;
    }
    if (rect.bottom > window.innerHeight) {
      element.style.top = `${Math.max(0, window.innerHeight - rect.height - 8)}px`;
    }
  }, [x, y]);

  return (
    <div ref={menuRef} className="mico-menu" style={{ left: x, top: y }} role="menu">
      {title && <div className="mico-menu-title">{title}</div>}
      {entries.map((entry) =>
        entry.kind === "separator" ? (
          <div key={entry.id} className="mico-menu-separator" />
        ) : (
          <button
            key={entry.id}
            type="button"
            role="menuitem"
            className={["mico-menu-item", entry.danger ? "mico-menu-danger" : undefined].filter(Boolean).join(" ")}
            disabled={entry.disabled}
            onClick={() => {
              entry.onSelect();
              onClose();
            }}
          >
            <span className="mico-menu-label">{entry.label}</span>
            {entry.hint !== undefined && <span className="mico-menu-hint">{entry.hint}</span>}
          </button>
        )
      )}
    </div>
  );
}
