#include "ui/picker.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "base/text.h"

namespace mico {

namespace {

char32_t lower(char32_t c) { return c < 0x80 ? char32_t(std::tolower(int(c))) : c; }
bool alnum(char32_t c) { return c >= 0x80 || std::isalnum(int(c)); }

// A match that begins a word reads as intended ("fork" in "Fork into a new
// chat", "tc" for "tool calls"), one inside a word as luck.
bool word_start(std::string_view s, size_t at) {
  if (at == 0) return true;
  const char a = s[at - 1], b = s[at];
  if (!alnum(char32_t(uint8_t(a)))) return true;
  return std::islower(uint8_t(a)) && std::isupper(uint8_t(b));
}

std::vector<char32_t> codepoints(std::string_view s) {
  std::vector<char32_t> out;
  for (size_t i = 0; i < s.size();) out.push_back(lower(text::decode(s, i)));
  return out;
}

}  // namespace

int fuzzy_match(std::string_view text, std::string_view query, std::vector<uint32_t>* marks) {
  if (marks) marks->clear();
  const std::vector<char32_t> q = codepoints(query);
  if (q.empty()) return 0;

  // The query as a run first: at a word start it is what a person typing a
  // name means, and the kind of match worth ranking first.
  const std::string needle = text::fold(query);
  if (const size_t at = text::find_folded(text, needle); at != std::string::npos) {
    if (marks) {
      size_t i = at;
      for (size_t k = 0; k < q.size() && i < text.size(); k++) {
        marks->push_back(uint32_t(i));
        text::decode(text, i);
      }
    }
    // Inside a word it is only a little better than the scattered kind:
    // "tc" means "tool calls" sooner than "fetch".
    int score = word_start(text, at) ? 2000 + (at == 0 ? 400 : 200) : 1000 + 15 * int(q.size());
    score -= int(std::min<size_t>(at, 200));
    return score - int(std::min<size_t>(text.size(), 200)) / 8;
  }

  // Otherwise every character in order, each one either continuing the run
  // before it or starting a word: "tc" is "tool calls", "nc" "New Chat". A
  // character found in the middle of an unrelated word is not a match —
  // "co" scattered through "doctor" is noise, not intent.
  int score = 1000, run = 0;
  size_t prev_end = 0;
  bool first = true;
  for (char32_t want : q) {
    size_t at = std::string::npos;
    if (!first && prev_end < text.size()) {
      size_t j = prev_end;
      if (lower(text::decode(text, j)) == want) at = prev_end;
    }
    const bool continues = at != std::string::npos;
    if (!continues) {
      for (size_t j = first ? 0 : prev_end; j < text.size();) {
        const size_t here = j;
        if (lower(text::decode(text, j)) == want && word_start(text, here)) {
          at = here;
          break;
        }
      }
    }
    if (at == std::string::npos) {
      if (marks) marks->clear();
      return -1;
    }
    if (marks) marks->push_back(uint32_t(at));
    if (continues) {
      score += 15 + 5 * run++;
    } else {
      // Every word it starts is worth something: initials are intent.
      score += 20 - (first ? int(std::min<size_t>(at, 100)) : int(std::min<size_t>(at - prev_end, 30)));
      run = 0;
    }
    first = false;
    size_t next = at;
    text::decode(text, next);
    prev_end = next;
  }
  return score - int(std::min<size_t>(text.size(), 200)) / 8;
}

// ------------------------------------------------------------------- items

void Picker::set_items(std::vector<PickItem> items) {
  items_ = std::move(items);
  refilter(false);  // the cursor stays on the same item index while it exists
  if (cur_ >= 0 && !selectable(size_t(cur_))) cur_ = -1;
  if (cur_ < 0 && !opt_.start_unselected) move(1);
}

void Picker::set_query(std::string q) {
  if (q == query_) return;
  query_ = std::move(q);
  refilter(true);
}

void Picker::refilter(bool query_changed) {
  const int keep = cursor();
  view_.clear();
  marks_.assign(items_.size(), {});
  if (query_.empty()) {
    std::string group;
    for (size_t i = 0; i < items_.size(); i++) {
      const PickItem& it = items_[i];
      if (!it.separator && !it.group.empty() && it.group != group) view_.push_back({int(i), true});
      if (!it.separator) group = it.group;
      view_.push_back({int(i), false});
    }
  } else {
    // Ranked, so the best match is where the cursor lands. Headings and
    // separators describe an order that no longer holds.
    std::vector<std::pair<int, int>> ranked;
    // The whole query first; failing that, with spaces in it, each word on
    // its own, anywhere, so "mico codex" finds the Codex chats in mico.
    std::vector<std::string> words;
    for (size_t a = 0; a < query_.size();) {
      const size_t b = std::min(query_.find(' ', a), query_.size());
      if (b > a) words.push_back(query_.substr(a, b - a));
      a = b + 1;
    }
    auto one = [&](const PickItem& it, const std::string& q, std::vector<uint32_t>* marks) {
      int s = fuzzy_match(it.label, q, marks);
      // The detail counts only as a whole word run: a subsequence scattered
      // over a sentence of help text matches almost any query.
      if (s < 0 && opt_.match_detail && !it.detail.empty() &&
          text::find_folded(it.detail, text::fold(q)) != std::string::npos)
        s = 0;
      return s;
    };
    std::vector<uint32_t> word_marks;
    for (size_t i = 0; i < items_.size(); i++) {
      const PickItem& it = items_[i];
      if (it.separator || it.pinned) continue;
      int s = one(it, query_, &marks_[i]);
      if (s < 0 && words.size() > 1) {
        s = 0;
        marks_[i].clear();
        for (const auto& w : words) {
          const int ws = one(it, w, &word_marks);
          if (ws < 0) { s = -1; break; }
          s += ws / int(words.size());
          marks_[i].insert(marks_[i].end(), word_marks.begin(), word_marks.end());
        }
        if (s < 0) marks_[i].clear();
        std::sort(marks_[i].begin(), marks_[i].end());
        marks_[i].erase(std::unique(marks_[i].begin(), marks_[i].end()), marks_[i].end());
      }
      if (s >= 0) ranked.push_back({s, int(i)});
    }
    // A caller that ranked its items itself (files, by name before path)
    // keeps its order; the query still filters and marks.
    if (opt_.ranked)
      std::stable_sort(ranked.begin(), ranked.end(),
                       [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& r : ranked) view_.push_back({r.second, false});
    for (size_t i = 0; i < items_.size(); i++)
      if (items_[i].pinned) view_.push_back({int(i), false});
  }
  if (query_changed) {
    top_ = 0;
    follow_ = true;
    cur_ = -1;
    if (!query_.empty() || !opt_.start_unselected) move(1);
  } else {
    cur_ = keep >= 0 ? entry_of(keep) : -1;
  }
}

bool Picker::selectable(size_t e) const {
  if (e >= view_.size() || view_[e].heading) return false;
  const PickItem& it = items_[size_t(view_[e].item)];
  return it.enabled && !it.separator;
}

int Picker::entry_of(int item) const {
  for (size_t e = 0; e < view_.size(); e++)
    if (!view_[e].heading && view_[e].item == item) return int(e);
  return -1;
}

int Picker::cursor() const { return cur_ >= 0 && cur_ < int(view_.size()) ? view_[size_t(cur_)].item : -1; }

void Picker::set_cursor(int item) {
  const int e = entry_of(item);
  if (e >= 0) {
    cur_ = e;
    follow_ = true;
  }
}

std::vector<int> Picker::checked() const {
  std::vector<int> out;
  for (size_t i = 0; i < items_.size(); i++)
    if (items_[i].checked && !items_[i].separator) out.push_back(int(i));
  return out;
}

int Picker::visible_count() const {
  int n = 0;
  for (const Entry& e : view_)
    if (!e.heading && !items_[size_t(e.item)].separator) n++;
  return n;
}

// ------------------------------------------------------------------- input

Picker::Result Picker::move(int delta) {
  if (view_.empty() || delta == 0) return Result::Handled;
  const int n = int(view_.size());
  const int dir = delta > 0 ? 1 : -1;
  follow_ = true;
  if (cur_ < 0) {
    for (int e = dir > 0 ? 0 : n - 1; e >= 0 && e < n; e += dir)
      if (selectable(size_t(e))) { cur_ = e; return Result::Moved; }
    return Result::Handled;
  }
  // One step wraps; a page stops at the end.
  int e = cur_, steps = std::abs(delta), last = cur_;
  const bool wrap = steps == 1 && opt_.wrap;
  for (int guard = 0; steps > 0 && guard < 2 * n; guard++) {
    e += dir;
    if (e < 0 || e >= n) {
      if (!wrap) break;
      e = (e + n) % n;
    }
    if (selectable(size_t(e))) {
      last = e;
      steps--;
    }
  }
  if (last == cur_) return Result::Handled;
  cur_ = last;
  return Result::Moved;
}

Picker::Result Picker::step_to(int e) {
  if (e < 0 || !selectable(size_t(e))) return Result::Handled;
  follow_ = true;
  if (e == cur_) return Result::Handled;
  cur_ = e;
  return Result::Moved;
}

Picker::Result Picker::choose(size_t e) {
  if (!selectable(e)) return Result::Handled;
  cur_ = int(e);
  index_ = view_[e].item;
  return Result::Chosen;
}

Picker::Result Picker::toggle(size_t e) {
  if (!selectable(e)) return Result::Handled;
  cur_ = int(e);
  index_ = view_[e].item;
  items_[size_t(index_)].checked = !items_[size_t(index_)].checked;
  return Result::Toggled;
}

Picker::Result Picker::on_key(const KeyEvent& k) {
  const int n = int(view_.size());
  auto first = [&](int from, int dir) {
    for (int e = from; e >= 0 && e < n; e += dir)
      if (selectable(size_t(e))) return e;
    return -1;
  };
  auto pop_char = [&] {
    while (!query_.empty() && (uint8_t(query_.back()) & 0xC0) == 0x80) query_.pop_back();
    if (!query_.empty()) query_.pop_back();
  };
  auto pop_word = [&] {
    while (!query_.empty() && query_.back() == ' ') query_.pop_back();
    while (!query_.empty() && query_.back() != ' ') pop_char();
  };
  auto edited = [&](std::string before) {
    if (before == query_) return Result::Handled;
    refilter(true);
    return Result::QueryChanged;
  };

  switch (k.key) {
    case Key::Up:
    case Key::Down: {
      const int dir = k.key == Key::Down ? 1 : -1;
      // Shift extends a multi-selection over the rows it passes.
      if (opt_.multi && k.shift && cur_ >= 0 && selectable(size_t(cur_))) {
        items_[size_t(view_[size_t(cur_)].item)].checked = true;
        const Result r = move(dir);
        if (cur_ >= 0) items_[size_t(view_[size_t(cur_)].item)].checked = true;
        index_ = cursor();
        return r == Result::Moved ? Result::Toggled : r;
      }
      return move(dir);
    }
    case Key::PageUp: return move(-std::max(1, page_ - 1));
    case Key::PageDown: return move(std::max(1, page_ - 1));
    case Key::Home: return step_to(first(0, 1));
    case Key::End: return step_to(first(n - 1, -1));
    case Key::Tab:
      if (opt_.tab_completes && !k.shift) {
        if (cur_ < 0 || !selectable(size_t(cur_))) return Result::Handled;
        index_ = view_[size_t(cur_)].item;
        return Result::Completed;
      }
      // fzf's convention: while typing, Space is a character, so Tab marks.
      if (opt_.multi && opt_.filter && cur_ >= 0) {
        const Result r = toggle(size_t(cur_));
        move(1);
        return r;
      }
      return move(1);
    case Key::BackTab: return move(-1);
    case Key::Enter:
      if (opt_.multi) return Result::Confirmed;
      return cur_ >= 0 ? choose(size_t(cur_)) : Result::Handled;
    case Key::Escape:
      if (!query_.empty()) return edited(std::exchange(query_, {}));
      return Result::Cancelled;
    case Key::Backspace: {
      if (opt_.back_on_empty && query_.empty()) return Result::Back;
      if (!opt_.filter) return Result::Ignored;
      std::string before = query_;
      if (k.ctrl || k.alt) pop_word();
      else pop_char();
      return edited(std::move(before));
    }
    case Key::Char: {
      if (k.ctrl) {
        if (k.ch == 'p') return move(-1);
        if (k.ch == 'n') return move(1);
        if (opt_.filter && k.ch == 'u') return edited(std::exchange(query_, {}));
        if (opt_.filter && k.ch == 'w') {
          std::string before = query_;
          pop_word();
          return edited(std::move(before));
        }
        if (opt_.multi && k.ch == 'a') {
          // Everything shown; again to clear it.
          bool all = true;
          for (size_t e = 0; e < view_.size(); e++)
            if (selectable(e) && !items_[size_t(view_[e].item)].checked) all = false;
          for (size_t e = 0; e < view_.size(); e++)
            if (selectable(e)) items_[size_t(view_[e].item)].checked = !all;
          index_ = cursor();
          return Result::Toggled;
        }
        return Result::Ignored;
      }
      if (k.alt) return Result::Ignored;
      if (opt_.multi && k.ch == ' ' && (!opt_.filter || query_.empty()))
        return cur_ >= 0 ? toggle(size_t(cur_)) : Result::Handled;
      if (opt_.numbers && query_.empty() && k.ch >= '1' && k.ch <= '9') {
        int want = int(k.ch - '0');
        for (size_t e = 0; e < view_.size(); e++) {
          if (!selectable(e) || --want > 0) continue;
          return opt_.multi ? toggle(e) : choose(e);
        }
        return opt_.filter ? Result::Handled : Result::Ignored;
      }
      if (opt_.filter && k.ch >= 0x20) {
        text::encode(k.ch, query_);
        refilter(true);
        return Result::QueryChanged;
      }
      return Result::Ignored;
    }
    default: return Result::Ignored;
  }
}

Picker::Result Picker::on_paste(std::string_view s) {
  if (!opt_.filter) return Result::Ignored;
  const std::string add = text::oneline(s, 200);
  if (add.empty()) return Result::Handled;
  query_ += add;
  refilter(true);
  return Result::QueryChanged;
}

Picker::Result Picker::on_mouse(const MouseEvent& m, Point local) {
  if (m.kind == MouseKind::WheelUp || m.kind == MouseKind::WheelDown) {
    // The wheel looks without moving the cursor; the next key brings it back.
    follow_ = false;
    top_ = std::clamp(top_ + (m.kind == MouseKind::WheelDown ? 3 : -3), 0,
                      std::max(0, int(view_.size()) - 1));
    return Result::Handled;
  }
  const int e = local.y >= 0 && local.y < int(hits_.size()) ? hits_[size_t(local.y)] : -1;
  if (e < 0) return local.y >= 0 && local.y < int(hits_.size()) ? Result::Handled : Result::Ignored;
  if (m.kind == MouseKind::Move || m.kind == MouseKind::Drag) {
    if (!selectable(size_t(e)) || e == cur_) return Result::Handled;
    cur_ = e;
    return Result::Moved;
  }
  if (m.kind == MouseKind::Press &&
      (m.button == MouseButton::Left || m.button == MouseButton::Right)) {
    if (!selectable(size_t(e))) return Result::Handled;
    follow_ = true;
    return opt_.multi ? toggle(size_t(e)) : choose(size_t(e));
  }
  return Result::Handled;
}

// ------------------------------------------------------------------ layout

Picker::Geometry Picker::geometry(int width) const {
  Geometry g;
  int x;
  if (opt_.frame == Frame::Popup) {
    // The cursor is the row's highlight; the check column is always there,
    // so a menu with one ticked entry does not indent it alone.
    g.check_x = 2;
    x = 4;
    g.right = width - 2;
  } else {
    g.mark_x = 1;
    x = 3;
    if (opt_.multi) {
      g.check_x = x;
      x += 2;
    }
    g.right = width - 1;
  }
  if (opt_.numbers) {
    g.num_x = x;
    x += 3;
  }
  g.label_x = x;
  return g;
}

int Picker::head_rows() const {
  int n = 0;
  if (opt_.frame == Frame::Panel) n += !opt_.title.empty() + !opt_.subtitle.empty();
  if (opt_.filter && (opt_.show_query || !query_.empty())) n++;
  return n;
}

int Picker::tail_rows() const {
  return !opt_.note.empty() + (opt_.frame == Frame::Panel && opt_.footer);
}

int Picker::entry_rows(size_t e, const Geometry& g) const {
  const Entry& en = view_[e];
  const PickItem& it = items_[size_t(en.item)];
  if (en.heading || it.separator) return 1;
  static thread_local std::vector<text::Span> spans;
  int rows = 1;
  const int lead = it.lead.empty() ? 0 : text::str_width(it.lead) + 1;
  const int room = g.right - g.label_x - lead;
  const int hint = it.hint.empty() ? 0 : std::min(text::str_width(it.hint), std::max(0, room / 3)) + 2;
  const int cols = std::max(4, room - hint);
  if (opt_.label_rows > 1 && text::str_width(it.label) > cols) {
    text::wrap_spans(it.label, cols, spans, size_t(opt_.label_rows));
    rows = std::max(1, int(spans.size()));
  }
  if (opt_.detail_rows > 0 && !it.detail.empty()) {
    text::wrap_spans(it.detail, std::max(4, g.right - g.label_x - lead), spans, size_t(opt_.detail_rows));
    rows += int(spans.size());
  }
  return rows;
}

int Picker::rows(int width, int max_rows) const {
  const Geometry g = geometry(width);
  int n = head_rows() + tail_rows() + (opt_.frame == Frame::Popup ? 2 : 0);
  int items = 0;
  for (size_t e = 0; e < view_.size(); e++) items += entry_rows(e, g);
  n += std::max(1, items);
  return std::min(n, max_rows);
}

int Picker::natural_width() const {
  const Geometry g = geometry(1000);
  const int pad = 1000 - g.right + g.label_x;  // everything a row spends outside its text
  int w = 16;
  for (const PickItem& it : items_) {
    if (it.separator) continue;
    int row = pad + text::str_width(it.label);
    if (!it.lead.empty()) row += text::str_width(it.lead) + 1;
    if (!it.detail.empty() && opt_.detail_rows == 0) row += 2 + text::str_width(it.detail);
    if (!it.hint.empty()) row += 2 + text::str_width(it.hint);
    w = std::max(w, row);
    if (!it.group.empty()) w = std::max(w, text::str_width(it.group) + 4);
  }
  w = std::max(w, text::str_width(opt_.title) + 6);
  if (opt_.filter) w = std::max(w, text::str_width(opt_.query_placeholder) + 12);
  return w;
}

std::string Picker::footer_hint() const {
  if (!opt_.footer_text.empty()) return opt_.footer_text;
  std::string s = "\xE2\x86\x91\xE2\x86\x93 move";  // ↑↓
  if (opt_.numbers) s += " \xC2\xB7 1-9";
  if (opt_.multi) s += opt_.filter ? " \xC2\xB7 tab mark \xC2\xB7 enter done" : " \xC2\xB7 space mark \xC2\xB7 enter done";
  else s += " \xC2\xB7 enter pick";
  s += " \xC2\xB7 esc close";
  return s;
}

void Picker::draw_label(Painter& p, int x, int y, std::string_view s, size_t base,
                        const std::vector<uint32_t>& marks, Style st, Style hi, int max_w) const {
  if (marks.empty()) {
    p.text_clipped(x, y, s, st, max_w);
    return;
  }
  // Glyph by glyph: matched characters take the highlight.
  const int limit = x + max_w;
  const bool fits = text::str_width(s) <= max_w;
  size_t m = 0;
  for (size_t i = 0; i < s.size();) {
    const size_t at = base + i;
    int cw;
    const char32_t cp = text::next_glyph(s, i, &cw);
    if (cw <= 0) continue;
    if (!fits && x + cw > limit - 1) {
      p.put(x, y, U'…', st);
      return;
    }
    while (m < marks.size() && marks[m] < at) m++;
    p.put(x, y, cp, m < marks.size() && marks[m] == at ? hi : st, uint8_t(cw));
    x += cw;
  }
}

void Picker::render(Painter p, const Theme& th) {
  const bool popup = opt_.frame == Frame::Popup;
  const Color bg = opt_.bg != kDefaultColor ? opt_.bg : popup ? th.menu_bg : th.strip_bg;
  const int w = p.width(), h = p.height();
  hits_.assign(size_t(std::max(0, h)), -1);
  query_row_ = -1;
  p.clear(Style{th.text, bg});
  if (w < 4 || h < 1) return;
  const Color tc = opt_.title_color != kDefaultColor ? opt_.title_color : th.accent;

  int y = 0, bottom = h;
  if (popup) {
    p.box(Rect{0, 0, w, h}, Style{th.border_focus, bg});
    if (!opt_.title.empty())
      p.text_clipped(2, 0, " " + opt_.title + " ", Style{th.border_focus, bg, attr::kBold}, w - 4);
    y = 1;
    bottom = h - 1;
  } else {
    if (!opt_.title.empty() && y < bottom) {
      int x = 1;
      if (!opt_.title_lead.empty()) x += p.text(1, y, opt_.title_lead + " ", Style{tc, bg, attr::kBold});
      p.text_clipped(x, y, opt_.title, Style{tc, bg, attr::kBold}, std::max(0, w - x - 1));
      y++;
    }
    if (!opt_.subtitle.empty() && y < bottom) {
      p.text_clipped(3, y, opt_.subtitle, Style{th.text, bg, attr::kBold}, std::max(0, w - 4));
      y++;
    }
  }
  if (opt_.filter && (opt_.show_query || !query_.empty()) && y < bottom) {
    query_row_ = y;
    const Style field{th.text, th.sel_bg};
    const int x0 = 1, fw = w - 2;
    p.fill(Rect{x0, y, fw, 1}, field);
    p.put(x0 + 1, y, U'›', Style{th.accent, field.bg, attr::kBold});  // ›
    const std::string count = query_.empty() ? std::string()
                                             : std::to_string(visible_count()) + "/" +
                                                   std::to_string(items_.size() - std::count_if(items_.begin(), items_.end(), [](const PickItem& i) { return i.separator; }));
    const int cw = count.empty() ? 0 : text::str_width(count) + 2;
    if (query_.empty()) {
      p.put(x0 + 3, y, U'▏', Style{th.accent, field.bg});
      p.text_clipped(x0 + 4, y, opt_.query_placeholder, Style{th.dim, field.bg}, std::max(0, fw - 5));
    } else {
      const int used = p.text_clipped(x0 + 3, y, query_, Style{th.text, field.bg, attr::kBold},
                                      std::max(0, fw - 5 - cw));
      p.put(x0 + 3 + used, y, U'▏', Style{th.accent, field.bg});
      if (cw) p.text(x0 + fw - cw + 1, y, count, Style{th.dim, field.bg});
    }
    y++;
  }
  // The note and the footer keep their rows whatever the items need.
  int note_y = -1, footer_y = -1;
  if (!popup && opt_.footer && bottom - y >= 2) footer_y = --bottom;
  if (!opt_.note.empty() && bottom - y >= 2) note_y = --bottom;

  const Geometry g = geometry(w);
  const int avail = std::max(0, bottom - y);
  const int n = int(view_.size());
  std::vector<int> height(static_cast<size_t>(n));
  for (int e = 0; e < n; e++) height[size_t(e)] = entry_rows(size_t(e), g);
  auto span = [&](int a, int b) {  // rows of entries [a, b]
    int s = 0;
    for (int e = a; e <= b; e++) s += height[size_t(e)];
    return s;
  };
  top_ = std::clamp(top_, 0, std::max(0, n - 1));
  if (follow_ && cur_ >= 0 && cur_ < n) {
    if (cur_ < top_) top_ = cur_;
    // Bring a group's heading along with its first item.
    if (top_ == cur_ && top_ > 0 && view_[size_t(top_ - 1)].heading) top_--;
    while (top_ < cur_ && span(top_, cur_) > avail) top_++;
  }
  // A list scrolled past its end, then shortened by a filter, would show a
  // blank; pull it back to fill the rows.
  while (top_ > 0 && n > 0 && span(top_ - 1, n - 1) <= avail) top_--;

  // Details after their labels line up in one column, as far right as the
  // widest label on screen but no further than half the row.
  int detail_col = 0;
  if (opt_.detail_rows == 0) {
    for (int e = top_; e < n; e++) {
      const PickItem& it = items_[size_t(view_[size_t(e)].item)];
      if (view_[size_t(e)].heading || it.separator || it.detail.empty()) continue;
      const int lead = it.lead.empty() ? 0 : text::str_width(it.lead) + 1;
      detail_col = std::max(detail_col, lead + text::str_width(it.label));
    }
    detail_col = g.label_x + std::min(detail_col, (g.right - g.label_x) / 2) + 2;
  }

  static thread_local std::vector<text::Span> spans;
  static const std::vector<uint32_t> no_marks;
  int shown = 0, ordinal = 0;
  for (int e = 0; e < top_; e++) ordinal += selectable(size_t(e));
  int e = top_;
  for (; e < n && y < bottom; e++) {
    const Entry& en = view_[size_t(e)];
    const PickItem& it = items_[size_t(en.item)];
    if (en.heading) {
      p.text_clipped(popup ? 2 : 1, y, it.group, Style{th.dim, bg, attr::kBold}, std::max(0, w - 4));
      y++;
      continue;
    }
    if (it.separator) {
      p.hline(1, y, w - 2, U'─', Style{th.border, bg});
      y++;
      continue;
    }
    shown++;
    const bool sel = selectable(size_t(e));
    if (sel) ordinal++;
    const bool hot = e == cur_ && sel;
    Style st{it.enabled ? th.text : th.dim, bg};
    if (hot && popup) st.bg = th.sel_bg;
    if (hot && !popup) {
      st.fg = th.accent;
      st.a = attr::kBold;
    }
    const Style hi{th.accent, st.bg, uint16_t(st.a | attr::kBold | attr::kUnderline)};
    const Style dim{th.dim, st.bg};
    const int rows = std::min(height[size_t(e)], bottom - y);
    if (hot && popup) p.fill(Rect{1, y, w - 2, rows}, st);
    for (int r = 0; r < rows; r++) hits_[size_t(y + r)] = e;

    if (hot && g.mark_x >= 0) p.put(g.mark_x, y, U'❯', Style{th.accent, st.bg, attr::kBold});  // ❯
    if (g.check_x >= 0) {
      if (opt_.multi)
        p.put(g.check_x, y, it.checked ? U'■' : U'□',  // ■ □
              Style{it.checked ? th.accent : th.dim, st.bg, it.checked ? attr::kBold : attr::kNone});
      else if (it.checked)
        p.put(g.check_x, y, U'✓', st);  // ✓
    }
    if (g.num_x >= 0 && sel && ordinal <= 9) p.text(g.num_x, y, std::to_string(ordinal) + ".", dim);

    int x = g.label_x;
    if (!it.lead.empty())
      x += p.text(x, y, it.lead, Style{it.lead_color != kDefaultColor ? it.lead_color : st.fg, st.bg}) + 1;
    int right = g.right;
    if (!it.hint.empty()) {
      // A hint gets a third of the row at most: it is the aside, the label
      // and its detail are the entry.
      const int full = text::str_width(it.hint);
      const int hw = std::min(full, std::max(0, (right - x) / 3));
      if (hw >= std::min(full, 3)) {
        p.text_clipped(right - hw, y, it.hint, dim, hw);
        right -= hw + 2;
      }
    }
    const int cols = std::max(1, right - x);
    const std::vector<uint32_t>& marks = query_.empty() ? no_marks : marks_[size_t(en.item)];
    int label_rows = 1, used = 0;
    if (opt_.label_rows > 1 && text::str_width(it.label) > cols) {
      text::wrap_spans(it.label, cols, spans, size_t(opt_.label_rows) + 1);
      label_rows = std::min<int>({int(spans.size()), opt_.label_rows, rows});
      for (int r = 0; r < label_rows; r++) {
        const text::Span sp = spans[size_t(r)];
        std::string_view piece = std::string_view(it.label).substr(sp.off, sp.len);
        const bool more = r == label_rows - 1 && int(spans.size()) > label_rows;
        const int room = more ? std::max(1, std::min(cols, text::str_width(piece) + 1)) : cols;
        if (more) {
          draw_label(p, x, y + r, piece, sp.off, marks, st, hi, room - 1);
          p.put(x + std::min(text::str_width(piece), room - 1), y + r, U'…', st);
        } else {
          draw_label(p, x, y + r, piece, sp.off, marks, st, hi, room);
        }
        used = text::str_width(piece);
      }
    } else {
      draw_label(p, x, y, it.label, 0, marks, st, hi, cols);
      used = std::min(cols, text::str_width(it.label));
    }
    if (!it.detail.empty()) {
      if (opt_.detail_rows == 0) {
        const int dx = std::max(x + used + 2, detail_col);
        if (right - dx >= 6) p.text_clipped(dx, y + label_rows - 1, it.detail, dim, right - dx);
      } else {
        text::wrap_spans(it.detail, std::max(4, g.right - x), spans, size_t(opt_.detail_rows));
        for (int r = 0; r < int(spans.size()) && label_rows + r < rows; r++)
          p.text_clipped(x, y + label_rows + r,
                         std::string_view(it.detail).substr(spans[size_t(r)].off, spans[size_t(r)].len),
                         Style{th.dim, bg}, g.right - x);
      }
    }
    y += rows;
  }
  page_ = std::max(1, shown);

  if (n == 0 || visible_count() == 0) {
    const int ey = std::min(bottom - 1, std::max(0, bottom - avail));
    if (ey >= 0 && ey < h) p.text_clipped(g.label_x, ey, opt_.empty_text, Style{th.dim, bg}, std::max(0, g.right - g.label_x));
  }
  // What the rows cannot hold is marked at the edge it is past.
  if (popup) {
    if (top_ > 0) p.put(w - 3, 0, U'▲', Style{th.border_focus, bg});
    if (e < n) p.put(w - 3, h - 1, U'▼', Style{th.border_focus, bg});
  } else if (avail > 0) {
    if (top_ > 0) p.put(w - 1, bottom - avail, U'▲', Style{th.dim, bg});
    if (e < n) p.put(w - 1, bottom - 1, U'▼', Style{th.dim, bg});
  }

  if (note_y >= 0)
    p.text_clipped(g.label_x, note_y, "\xE2\x9C\x8E " + text::oneline(opt_.note, 400), Style{th.text, bg},
                   std::max(0, g.right - g.label_x));
  if (opt_.footer) {
    const std::string hint = footer_hint();
    if (popup) {
      const std::string s = " " + hint + " ";
      if (text::str_width(s) <= w - 6) p.text(2, h - 1, s, Style{th.dim, bg});
    } else if (footer_y >= 0) {
      p.text_clipped(3, footer_y, hint, Style{th.dim, bg}, std::max(0, w - 4));
    }
  }
}

}  // namespace mico
