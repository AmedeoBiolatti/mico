#include <algorithm>

#include "core/store.h"
#include "base/text.h"
#include "ui/app.h"
#include "views/outline.h"
#include "views/chat_render.h"
#include "views/find_bar.h"
#include "views/views.h"

namespace mico {
namespace {

// Browses transcripts the agents already wrote. The live equivalent is
// SessionPane, which shares the same ChatRenderer.
class ChatView final : public Pane {
 public:
  std::string title() const override {
    const SessionRef* s = app_->current_session();
    if (!s) return "Chat";
    if (const auto* name = app_->store().custom_name(s->agent, s->id)) return *name;
    return s->title.empty() ? "Saved " + agent_label(s->agent) + " chat" : text::oneline(s->title, 100);
  }

  void on_state_changed() override { chat_.to_bottom(); }

  void render(Painter& p, bool focused) override {
    const Theme& th = app_->theme();
    const SessionRef* s = app_->current_session();
    if (!s) {
      p.clear(Style{th.text, th.panel});
      resume_row_ = chip_row_ = -1;
      const int y = std::max(0, p.height() / 3);
      p.text_clipped(3, y, "Your workspace, ready when you are.",
                     Style{th.text, th.panel, attr::kBold}, p.width() - 6);
      p.text_clipped(3, y + 2, "Open a conversation or start a new chat.",
                     Style{th.dim, th.panel}, p.width() - 6);
      p.text_clipped(3, y + 4, "F4  Claude     F5  Codex     F7  Other agent",
                     Style{th.accent, th.panel}, p.width() - 6);
      return;
    }
    const bool changed = chat_.path() != s->path;
    chat_.set_base_dir(s->cwd);
    chat_.open(s->path, Store::adapter_for(*s));
    if (changed) {
      chat_.to_bottom();
      chat_.set_scroll(app_->start_scroll);
    }
    chat_.poll_growth();
    // A search across chats opened this one at a match.
    {
      uint64_t at = 0;
      std::string q;
      if (app_->take_reveal(s->path, &at, &q)) {
        if (!q.empty()) find_.open(chat_, q);
        chat_.reveal(at, std::move(q));
      }
    }
    // A strip of what the session was running, read out of the transcript.
    // Reserve on "there is an adapter", not on "state is known": state arrives
    // during the render below, and testing it first costs a frame of lag.
    const bool chips = chat_.has_adapter() && p.height() > 2;
    // A rule above the strip, room permitting, so it reads as a footer rather
    // than one more line of chat.
    const bool chip_sep = chips && p.height() > 3;
    resume_row_ = p.height() >= 5 ? p.height() - 1 : -1;
    chip_row_ = chips ? p.height() - 1 - (resume_row_ >= 0 ? 1 : 0) : -1;
    const int footer_rows = (chips ? 1 : 0) + (chip_sep ? 1 : 0) + (resume_row_ >= 0 ? 1 : 0);
    Painter body = p.sub(Rect{0, 0, p.width(), std::max(1, p.height() - footer_rows)});
    chat_.render(body, th, app_->filters());
    if (chip_sep) {
      Painter sep = p.sub(Rect{0, chip_row_ - 1, p.width(), 1});
      sep.clear(Style{th.dim, th.strip_bg});
      sep.hline(0, 0, sep.width(), U'─', Style{th.border, th.strip_bg});
    }
    if (chips) {
      Painter strip = p.sub(Rect{0, chip_row_, p.width(), 1});
      chat_.render_chips(strip, th, chips_);
    }
    if (resume_row_ >= 0 && find_.active()) {
      find_.render(p.sub(Rect{0, resume_row_, p.width(), 1}), th, chat_);
    } else if (resume_row_ >= 0) {
      Painter action = p.sub(Rect{0, resume_row_, p.width(), 1});
      action.clear(Style{th.dim, th.strip_bg});
      action.text(2, 0, " Open chat  ↵ ", Style{th.bg, th.accent, attr::kBold});
      if (p.width() > 44)
        action.text(19, 0, "Resumes automatically", Style{th.dim, th.strip_bg});
    }
  }

  bool on_key(const KeyEvent& k) override {
    if (find_.on_key(k, chat_)) {
      if (find_.wants_all_chats()) app_->open_search(find_.query());
      return true;
    }
    if (k.key == Key::Char && k.ctrl && !k.alt && (k.ch == 'f' || k.ch == 'F') &&
        app_->current_session()) {
      find_.open(chat_);
      return true;
    }
    if (k.key == Key::Enter) return app_->open_selected_chat();
    if (k.is_ctrl('g')) {
      open_outline(app_, this, chat_);
      return true;
    }
    return chat_.on_key(k);
  }
  bool on_paste(std::string_view t) override {
    if (!find_.active()) return false;
    find_.on_paste(t, chat_);
    return true;
  }
  std::string take_url() override { return chat_.take_url(); }

  bool on_mouse(const MouseEvent& m, Point local) override {
    if (resume_row_ >= 0 && local.y == resume_row_ && m.kind == MouseKind::Press &&
        m.button == MouseButton::Left) return app_->open_selected_chat();
    if (chip_row_ >= 0 && local.y == chip_row_ && m.kind == MouseKind::Press) {
      for (const auto& c : chips_)
        if (local.x >= c.rect.x && local.x < c.rect.x + c.rect.w) {
          const Point chip{m.pos.x - local.x + c.rect.x, m.pos.y};
          open_chip_picker(app_, this, chip, chat_.state(), c.key, false, "");
          return true;
        }
      return true;
    }
    const bool used = chat_.on_mouse(m, local);
    // Grabbing the scrollbar means the pointer will leave this pane.
    if (chat_.grabbing()) app_->capture_mouse(this);
    return used;
  }
  std::vector<MenuItem> context_menu(Point local) override {
    if (chip_row_ >= 0 && local.y == chip_row_)
      for (const auto& c : chips_)
        if (local.x >= c.rect.x && local.x < c.rect.x + c.rect.w)
          return chip_menu(chat_.state(), c.key, false, "");
    std::vector<MenuItem> items;
    if (app_->current_session()) items.push_back(MenuItem{"Open chat", "open_chat"});
    for (auto& it : chat_.context_menu(local)) items.push_back(std::move(it));
    if (!items.empty()) items.push_back(MenuItem::sep());
    items.push_back(MenuItem{"Outline\xE2\x80\xA6", "outline", true, false, false, "", "Ctrl+G"});
    return items;
  }
  void on_action(const std::string& a) override {
    if (a == "open_chat") { app_->open_selected_chat(); return; }
    if (a == "outline") { open_outline(app_, this, chat_); return; }
    if (handle_outline_action(app_, this, chat_, a)) return;
    chat_action(*app_, chat_, a);
  }

 private:
  ChatRenderer chat_;
  FindBar find_;
  std::vector<ChatRenderer::Chip> chips_;
  int chip_row_ = -1;
  int resume_row_ = -1;
};

}  // namespace

void chat_action(App& app, ChatRenderer& chat, const std::string& a) {
  if (a.rfind("chipcopy:", 0) == 0) {
    if (const std::string* v = chat.state().find(a.substr(9))) {
      app.copy_to_clipboard(*v);
      app.set_status("copied: " + *v);
    }
    return;
  }
  std::string copy;
  chat.on_action(a, app.filters(), &copy);
  if (!copy.empty()) {
    app.copy_to_clipboard(copy);
    app.set_status("copied to clipboard");
  } else if (std::string n = chat.take_notice(); !n.empty()) {
    app.set_status(std::move(n));
  }
}

PanePtr make_chat_view() { return std::make_unique<ChatView>(); }

}  // namespace mico
