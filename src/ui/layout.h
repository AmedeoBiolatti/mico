#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "ui/pane.h"

namespace mico {

enum class SplitDir { Horizontal, Vertical };

// A binary-ish split tree. M1 only builds one static layout, but panes are
// placed by the same code path that M2's dynamic splitting will use, so the
// pane geometry model does not have to change when PTY panes arrive.
class Node {
 public:
  struct Placed {
    Pane* pane;
    Rect rect;
    size_t index;  // stable id, assigned in placement order
  };

  // A draggable boundary between two adjacent children of a split. `hit` covers
  // both bordering columns/rows, so the two-cell seam between panes is the
  // grab target rather than a one-cell line nobody can hit with a mouse.
  struct Divider {
    Node* owner;
    size_t index;  // boundary between kids_[index] and kids_[index + 1]
    SplitDir dir;
    Rect area;  // the parent split's full area, needed to recompute fractions
    Rect hit;
  };

  static std::unique_ptr<Node> leaf(PanePtr pane);
  static std::unique_ptr<Node> split(SplitDir dir, std::vector<std::unique_ptr<Node>> kids,
                                     std::vector<float> fractions);

  bool is_leaf() const { return pane_ != nullptr; }

  // Flattens the tree into screen rectangles, in depth-first order. Dividers
  // are collected deepest-first so a nested boundary wins a hit test against
  // the outer one it sits inside.
  std::vector<Placed> place(Rect area, std::vector<Divider>* dividers = nullptr);
  void for_each_pane(const std::function<void(Pane&)>& fn) const;
  // Preserve a live pane's draft, scroll and view mode across layout changes.
  PanePtr take_session_pane();

  // Moves boundary `index` so it lands on `pos`, taking space from one
  // neighbour and giving it to the other. Other children keep their size.
  void drag_divider(size_t index, Rect area, Point pos);

  // Depth-first list of each split's fractions, in placement order, so a
  // rebuilt tree can be given back the sizes the user dragged.
  void collect_fractions(std::vector<std::vector<float>>& out) const;
  void apply_fractions(const std::vector<std::vector<float>>& in, size_t& idx);

 private:
  void place_into(Rect area, std::vector<Placed>& out, std::vector<Divider>* dividers);
  // Child extents in cells for a given area, matching what place_into lays out.
  std::vector<int> extents(Rect area) const;

  PanePtr pane_;
  SplitDir dir_ = SplitDir::Horizontal;
  std::vector<std::unique_ptr<Node>> kids_;
  std::vector<float> fractions_;
};

}  // namespace mico
