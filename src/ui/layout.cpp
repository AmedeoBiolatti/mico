#include "ui/layout.h"

#include <algorithm>
#include <numeric>

namespace mico {

PanePtr Node::take_session_pane() {
  if (pane_ && pane_->session()) return std::move(pane_);
  for (auto& child : kids_)
    if (auto pane = child->take_session_pane()) return pane;
  return {};
}

std::unique_ptr<Node> Node::leaf(PanePtr pane) {
  auto n = std::make_unique<Node>();
  n->pane_ = std::move(pane);
  return n;
}

std::unique_ptr<Node> Node::split(SplitDir dir, std::vector<std::unique_ptr<Node>> kids,
                                  std::vector<float> fractions) {
  auto n = std::make_unique<Node>();
  n->dir_ = dir;
  n->kids_ = std::move(kids);
  n->fractions_ = std::move(fractions);
  n->fractions_.resize(n->kids_.size(), 1.0f);
  return n;
}

std::vector<int> Node::extents(Rect area) const {
  const bool horiz = dir_ == SplitDir::Horizontal;
  const int total = horiz ? area.w : area.h;
  const float sum = std::accumulate(fractions_.begin(), fractions_.end(), 0.0f);

  std::vector<int> out;
  out.reserve(kids_.size());
  int used = 0;
  for (size_t i = 0; i < kids_.size(); i++) {
    // The last child absorbs the rounding remainder so splits always tile
    // exactly, with no one-column gap between panes.
    int extent = (i + 1 == kids_.size())
                     ? total - used
                     : std::max(1, int(float(total) * (fractions_[i] / sum)));
    out.push_back(extent);
    used += extent;
  }
  return out;
}

std::vector<Node::Placed> Node::place(Rect area, std::vector<Divider>* dividers) {
  std::vector<Placed> out;
  if (dividers) dividers->clear();
  place_into(area, out, dividers);
  for (size_t i = 0; i < out.size(); i++) out[i].index = i;
  return out;
}

void Node::place_into(Rect area, std::vector<Placed>& out, std::vector<Divider>* dividers) {
  if (is_leaf()) {
    out.push_back(Placed{pane_.get(), area, 0});
    return;
  }
  if (kids_.empty() || area.empty()) return;

  const bool horiz = dir_ == SplitDir::Horizontal;
  const std::vector<int> ext = extents(area);

  int used = 0;
  for (size_t i = 0; i < kids_.size(); i++) {
    Rect r = horiz ? Rect{area.x + used, area.y, ext[i], area.h}
                   : Rect{area.x, area.y + used, area.w, ext[i]};
    kids_[i]->place_into(r, out, dividers);
    used += ext[i];

    // Record the seam after every child but the last. Children are recursed
    // into first, so nested dividers land earlier in the list and win hit tests.
    if (dividers && i + 1 < kids_.size()) {
      Rect hit = horiz ? Rect{area.x + used - 1, area.y, 2, area.h}
                       : Rect{area.x, area.y + used - 1, area.w, 2};
      dividers->push_back(Divider{this, i, dir_, area, hit});
    }
  }
}

void Node::drag_divider(size_t index, Rect area, Point pos) {
  if (index + 1 >= kids_.size()) return;
  const bool horiz = dir_ == SplitDir::Horizontal;
  const std::vector<int> ext = extents(area);

  // Space before the pair is fixed; the pair splits what is left between them.
  int before = 0;
  for (size_t i = 0; i < index; i++) before += ext[i];
  const int pair = ext[index] + ext[index + 1];
  const int origin = (horiz ? area.x : area.y) + before;

  // Enough room for a border on each side plus a column of content.
  constexpr int kMin = 4;
  if (pair < 2 * kMin) return;

  int want = (horiz ? pos.x : pos.y) - origin + 1;
  want = std::clamp(want, kMin, pair - kMin);

  const float pair_frac = fractions_[index] + fractions_[index + 1];
  fractions_[index] = pair_frac * float(want) / float(pair);
  fractions_[index + 1] = pair_frac - fractions_[index];
}

void Node::collect_fractions(std::vector<std::vector<float>>& out) const {
  if (is_leaf()) return;
  out.push_back(fractions_);
  for (const auto& k : kids_) k->collect_fractions(out);
}

void Node::apply_fractions(const std::vector<std::vector<float>>& in, size_t& idx) {
  if (is_leaf()) return;
  // Only adopt a saved row that still matches this split's shape; a layout
  // change (a future third pane, say) must not scramble the sizes.
  if (idx < in.size() && in[idx].size() == fractions_.size()) fractions_ = in[idx];
  idx++;
  for (const auto& k : kids_) k->apply_fractions(in, idx);
}

void Node::for_each_pane(const std::function<void(Pane&)>& fn) const {
  if (is_leaf()) { fn(*pane_); return; }
  for (const auto& k : kids_) k->for_each_pane(fn);
}

}  // namespace mico
