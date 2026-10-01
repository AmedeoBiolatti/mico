#pragma once
#include <algorithm>
#include <string>

#include "term/term.h"

namespace mico {

// Selection + scroll offset for a vertical list. Shared by every list-shaped
// pane so keyboard and mouse behave identically across them.
struct ListState {
  int sel = 0;
  int top = 0;

  void clamp(int count, int height) {
    if (count <= 0) { sel = top = 0; return; }
    sel = std::clamp(sel, 0, count - 1);
    if (height > 0) {
      top = std::clamp(top, std::max(0, sel - height + 1), sel);
      top = std::clamp(top, 0, std::max(0, count - height));
    }
  }

  // Returns true if the key was consumed.
  bool on_key(const KeyEvent& k, int count, int height) {
    int prev = sel;
    switch (k.key) {
      case Key::Up: sel--; break;
      case Key::Down: sel++; break;
      case Key::PageUp: sel -= std::max(1, height - 1); break;
      case Key::PageDown: sel += std::max(1, height - 1); break;
      case Key::Home: sel = 0; break;
      case Key::End: sel = count - 1; break;
      default:
        if (k.is('j')) sel++;
        else if (k.is('k')) sel--;
        else return false;
    }
    clamp(count, height);
    return sel != prev || true;
  }

  // Returns the clicked row index, or -1.
  int on_mouse(const MouseEvent& m, Point local, int count, int height) {
    if (m.kind == MouseKind::WheelUp) { top = std::max(0, top - 3); clamp(count, height); return -1; }
    if (m.kind == MouseKind::WheelDown) {
      top = std::min(std::max(0, count - height), top + 3);
      clamp(count, height);
      return -1;
    }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left) return -1;
    int idx = top + local.y;
    if (idx < 0 || idx >= count) return -1;
    sel = idx;
    clamp(count, height);
    return idx;
  }
};

// A list of items several lines tall, above a button pinned to the pane's
// bottom edge. Tall items are easy to hit with a finger on a phone terminal.
// The selection runs 0..count, where count is the button: it can be selected
// like a row but never scrolls away.
struct TallList {
  static constexpr int kItemH = 2;

  ListState list;
  int count = 0;   // items, not counting the button
  int height = 0;  // pane lines

  // The button gets a full item's height when that still leaves room for a
  // couple of items, and one line otherwise.
  int footer_h() const { return height >= 3 * kItemH ? kItemH : 1; }
  int footer_y() const { return std::max(0, height - footer_h()); }
  int list_h() const { return footer_y(); }
  int visible() const { return std::max(1, list_h() / kItemH); }

  // Size to the pane and bring the selection into view — only when it or the
  // pane moved, so a wheel scroll is not snapped back on the next frame.
  void fit(int n, int h) {
    count = n;
    height = h;
    list.sel = std::clamp(list.sel, 0, count);
    if (list.sel != followed_ || height != followed_h_) {
      if (list.sel < count)
        list.top = std::clamp(list.top, std::max(0, list.sel - visible() + 1), list.sel);
      followed_ = list.sel;
      followed_h_ = height;
    }
    list.top = std::clamp(list.top, 0, std::max(0, count - visible()));
  }

  bool on_key(const KeyEvent& k) {
    if (!list.on_key(k, count + 1, visible())) return false;
    fit(count, height);
    return true;
  }

  // The item under pane line y: count for the button, -1 for nothing.
  int index_at(int y) const {
    if (y >= footer_y() && y < height) return count;
    if (y < 0 || y >= list_h()) return -1;
    const int i = list.top + y / kItemH;
    return i < count ? i : -1;
  }

  // Returns the clicked index (count for the button), or -1. The wheel scrolls
  // the items without moving the selection.
  int on_mouse(const MouseEvent& m, Point local) {
    if (m.kind == MouseKind::WheelUp || m.kind == MouseKind::WheelDown) {
      list.top += m.kind == MouseKind::WheelUp ? -1 : 1;
      list.top = std::clamp(list.top, 0, std::max(0, count - visible()));
      return -1;
    }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left) return -1;
    const int i = index_at(local.y);
    if (i >= 0) {
      list.sel = i;
      fit(count, height);
    }
    return i;
  }

 private:
  int followed_ = -1;
  int followed_h_ = -1;
};

struct Theme;

// The pinned button under a TallList: label centred, highlighted when selected.
void draw_list_button(Painter& p, const Theme& th, const TallList& tl, std::string_view label,
                      bool sel, bool focused);

// The row above a sidebar list while it filters the other tabs: "All
// folders", "All chats". Lit when it is what the filter is set to. Takes
// kFilterHeaderH lines; the list goes under it.
inline constexpr int kFilterHeaderH = 2;
void draw_filter_header(Painter& p, const Theme& th, std::string_view label, bool on, bool focused);

// "3m", "5h", "2d" — compact enough for a narrow sidebar.
std::string rel_time(int64_t mtime);

}  // namespace mico
