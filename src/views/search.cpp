#include <algorithm>
#include <ctime>
#include <string>

#include "core/search.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/views.h"

namespace mico {
namespace {

// Search across every chat: a query line, then the matches grouped by chat,
// newest chat first, each with a line of context. Results arrive while the
// search runs; Enter or a click opens the chat at the match.
class SearchView final : public Pane {
 public:
  explicit SearchView(ChatSearch& search) : search_(search), query_(search.query()) {}
  std::string title() const override { return "Search"; }
  // Every printable key is part of the query.
  bool captures_keys() const override { return true; }

  void render(Painter& p, bool focused) override {
    const Theme& th = app_->theme();
    std::string asked;
    if (app_->take_search_request(&asked) && !asked.empty()) {
      query_ = asked;
      run();
    }
    // A different folder or chat picked on the left: the same query, there.
    if (app_->filter_version() != searched_version_ && search_.started()) run();
    p.clear(Style{th.text, th.panel});
    rows_.clear();

    // The query line.
    int x = p.text(1, 0, " Search ", Style{th.bg, th.accent, attr::kBold}) + 2;
    x += p.text_clipped(x, 0, query_, Style{th.text, th.panel, attr::kBold}, std::max(0, p.width() - x - 2));
    if (focused) p.put(x, 0, U'▏', Style{th.accent, th.panel});
    const std::string scope = app_->view_filter().label;
    if (query_.empty())
      p.text_clipped(x + 2, 0, "type to search " + (scope == "all folders" ? std::string("every chat") : scope),
                     Style{th.dim, th.panel}, std::max(0, p.width() - x - 4));

    // What the search is doing.
    std::string status;
    if (!search_.started()) {
      status = "matches show as you type · \xE2\x86\x91/\xE2\x86\x93 choose · enter open · esc back";
    } else {
      status = std::to_string(search_.total_hits()) +
               (search_.total_hits() == 1 ? " match in " : " matches in ") +
               std::to_string(search_.chats_with_hits()) +
               (search_.chats_with_hits() == 1 ? " chat" : " chats") + " \xC2\xB7 " + scope;
      if (!search_.complete())
        status += " \xC2\xB7 searching " + std::to_string(search_.files_done()) + "/" +
                  std::to_string(search_.files_total());
      else
        status += " \xC2\xB7 " + std::to_string(search_.files_total()) + " searched";
    }
    p.text_clipped(1, 1, status, Style{th.dim, th.panel}, std::max(0, p.width() - 2));
    p.hline(1, 2, std::max(0, p.width() - 2), U'─', Style{th.border, th.panel});

    const auto& hits = search_.hits();
    sel_ = std::clamp(sel_, 0, std::max(0, int(hits.size()) - 1));
    // Rows: a heading for each chat, then its snippets.
    std::vector<std::pair<int, bool>> lines;  // (hit, heading?)
    int sel_line = 0;
    for (int i = 0; i < int(hits.size()); i++) {
      if (hits[size_t(i)].first_in_chat) lines.push_back({i, true});
      if (i == sel_) sel_line = int(lines.size());
      lines.push_back({i, false});
    }
    const int body_top = 3, body_h = std::max(0, p.height() - body_top);
    // Keep the selection in view, with its heading when that fits.
    if (sel_line - 1 < top_) top_ = std::max(0, sel_line - 1);
    if (sel_line >= top_ + body_h) top_ = sel_line - body_h + 1;
    top_ = std::clamp(top_, 0, std::max(0, int(lines.size()) - body_h));

    for (int r = 0; r < body_h && top_ + r < int(lines.size()); r++) {
      const auto [hi, heading] = lines[size_t(top_ + r)];
      const SearchHit& h = hits[size_t(hi)];
      const int y = body_top + r;
      rows_.push_back({y, hi});
      if (heading) {
        const std::string name = h.title.empty() ? agent_label(h.agent) + " chat" : text::oneline(h.title, 200);
        int cx = p.text_clipped(2, y, name, Style{th.text, th.panel, attr::kBold}, std::max(0, p.width() - 4));
        std::string meta = "  " + h.project + " \xC2\xB7 " + agent_label(h.agent) + " \xC2\xB7 " + ago(h.mtime);
        if (h.chat_hits > 1) meta += " \xC2\xB7 " + std::to_string(h.chat_hits) + " matches";
        p.text_clipped(2 + cx, y, meta, Style{th.dim, th.panel}, std::max(0, p.width() - cx - 4));
        continue;
      }
      const bool sel = hi == sel_;
      const Color bg = sel ? th.sel_bg : th.panel;
      if (sel) p.fill(Rect{1, y, p.width() - 2, 1}, Style{th.text, bg});
      if (sel) p.put(2, y, U'❯', Style{th.accent, bg, attr::kBold});
      const int sx = 5, sw = std::max(0, p.width() - sx - 2);
      p.text_clipped(sx, y, h.snippet, Style{sel ? th.text : th.dim, bg}, sw);
      const int mx = sx + text::str_width(std::string_view(h.snippet).substr(0, h.match_at));
      if (mx < sx + sw)
        p.text_clipped(mx, y, std::string_view(h.snippet).substr(h.match_at, h.match_len),
                       Style{th.bg, th.accent, attr::kBold}, sx + sw - mx);
    }
    if (search_.started() && search_.complete() && hits.empty() && body_h > 1)
      p.text_clipped(2, body_top + 1, "nothing found", Style{th.dim, th.panel}, std::max(0, p.width() - 4));
  }

  bool on_key(const KeyEvent& k) override {
    const int n = int(search_.hits().size());
    switch (k.key) {
      case Key::Escape:
        if (!query_.empty()) {
          query_.clear();
          search_.start({}, app_->store(), {});
        } else {
          app_->open_tab(0);
        }
        return true;
      case Key::Enter: open_selected(); return true;
      case Key::Up: sel_ = std::max(0, sel_ - 1); return true;
      case Key::Down: sel_ = std::min(std::max(0, n - 1), sel_ + 1); return true;
      case Key::PageUp: sel_ = std::max(0, sel_ - 10); return true;
      case Key::PageDown: sel_ = std::min(std::max(0, n - 1), sel_ + 10); return true;
      case Key::Backspace:
        if (k.ctrl || k.alt) {
          while (!query_.empty() && query_.back() == ' ') query_.pop_back();
          while (!query_.empty() && query_.back() != ' ') pop_char();
        } else {
          pop_char();
        }
        run();
        return true;
      case Key::Char:
        if (k.ctrl && k.ch == 'u') { query_.clear(); run(); return true; }
        if (k.ctrl || k.alt || k.ch < 0x20) return false;
        text::encode(k.ch, query_);
        run();
        return true;
      default: return false;
    }
  }
  bool on_paste(std::string_view t) override {
    query_ += text::oneline(t, 200);
    run();
    return true;
  }
  bool on_mouse(const MouseEvent& m, Point local) override {
    if (m.kind == MouseKind::WheelUp) { sel_ = std::max(0, sel_ - 3); return true; }
    if (m.kind == MouseKind::WheelDown) {
      sel_ = std::min(std::max(0, int(search_.hits().size()) - 1), sel_ + 3);
      return true;
    }
    if (m.kind != MouseKind::Press || m.button != MouseButton::Left) return false;
    for (const auto& [y, hi] : rows_)
      if (y == local.y) {
        sel_ = hi;
        open_selected();
        return true;
      }
    return false;
  }

 private:
  // Searches from two characters: one matches nearly every line of every chat.
  void run() {
    sel_ = top_ = 0;
    const Filters& f = app_->filters();
    // Only the chats the sidebar has selected: a folder, one chat, or all.
    searched_version_ = app_->filter_version();
    search_.start(app_->filtered_projects(), app_->store(), query_.size() >= 2 ? query_ : std::string(),
                  SearchScope{f.show_thinking(), f.show_tools(), f.show_meta()});
  }
  void open_selected() {
    const auto& hits = search_.hits();
    if (sel_ < 0 || sel_ >= int(hits.size())) return;
    app_->open_search_hit(hits[size_t(sel_)], search_.query());
  }
  void pop_char() {
    while (!query_.empty() && (uint8_t(query_.back()) & 0xC0) == 0x80) query_.pop_back();
    if (!query_.empty()) query_.pop_back();
  }

  ChatSearch& search_;
  std::string query_;
  int sel_ = 0, top_ = 0;
  uint64_t searched_version_ = 0;  // the filter the running search was for
  std::vector<std::pair<int, int>> rows_;  // (screen row, hit) of the last frame
};

}  // namespace

PanePtr make_search_view(ChatSearch& search) { return std::make_unique<SearchView>(search); }

}  // namespace mico
