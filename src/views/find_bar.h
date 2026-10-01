#pragma once
#include <string>
#include <utility>

#include "base/text.h"
#include "ui/pane.h"
#include "ui/theme.h"
#include "views/chat_render.h"

namespace mico {

// Find in one chat (Ctrl+F): a one-row bar over the chat's footer. Typing
// lights matches on screen as it goes; Enter walks to the next older match,
// Shift+Enter (or ↓) to the next newer, Escape closes. The chat itself does
// the searching; see ChatRenderer::set_find_query.
class FindBar {
 public:
  bool active() const { return active_; }
  void open(ChatRenderer& chat) {
    active_ = true;
    chat.set_find_query(query_);
  }
  // Opened by a search across chats, already holding its query.
  void open(ChatRenderer& chat, std::string query) {
    query_ = std::move(query);
    open(chat);
  }
  const std::string& query() const { return query_; }
  // Ctrl+F in the open bar: the same query, across every chat.
  bool wants_all_chats() { return std::exchange(all_chats_, false); }
  void close(ChatRenderer& chat) {
    active_ = false;
    chat.clear_find();
  }

  // Keys while the bar is open. Everything is the bar's: a stray key must not
  // reach an agent while the user thinks they are typing a search.
  bool on_key(const KeyEvent& k, ChatRenderer& chat) {
    if (!active_) return false;
    switch (k.key) {
      case Key::Escape: close(chat); return true;
      case Key::Enter: chat.find_next(k.shift ? 1 : -1); return true;
      case Key::Up: chat.find_next(-1); return true;
      case Key::Down: chat.find_next(1); return true;
      case Key::Backspace:
        if (k.ctrl || k.alt) {
          while (!query_.empty() && query_.back() == ' ') query_.pop_back();
          while (!query_.empty() && query_.back() != ' ') pop_char();
        } else {
          pop_char();
        }
        chat.set_find_query(query_);
        return true;
      case Key::Char:
        if (k.ctrl && (k.ch == 'u')) { query_.clear(); chat.set_find_query(query_); return true; }
        if (k.ctrl && (k.ch == 'f')) { all_chats_ = true; return true; }
        if (k.ctrl || k.alt || k.ch < 0x20) return true;
        text::encode(k.ch, query_);
        chat.set_find_query(query_);
        return true;
      default: return true;
    }
  }
  void on_paste(std::string_view s, ChatRenderer& chat) {
    query_ += text::oneline(s, 200);
    chat.set_find_query(query_);
  }

  void render(Painter p, const Theme& th, const ChatRenderer& chat) const {
    const Color bg = th.strip_bg;
    p.clear(Style{th.text, bg});
    int x = p.text(1, 0, " Find ", Style{th.bg, th.accent, attr::kBold}) + 2;
    x += p.text_clipped(x, 0, query_, Style{th.text, bg, attr::kBold}, std::max(0, p.width() - x - 30));
    p.put(x, 0, U'▏', Style{th.accent, bg});  // ▏ cursor
    const int n = chat.find_count();
    std::string info;
    if (query_.empty()) info = "type to search this chat";
    else if (n < 0) info = "enter to search";
    else if (n == 0) info = "no matches";
    else info = (chat.find_index() > 0 ? std::to_string(chat.find_index()) + " of " : "") +
                std::to_string(n) + (n == 1 ? " match" : " matches");
    // The count always; the keys when there is room for them too.
    const std::string keys =
        "   \xE2\x86\xB5 older \xC2\xB7 \xE2\x87\xA7\xE2\x86\xB5 newer \xC2\xB7 ^F all chats \xC2\xB7 esc close";
    if (p.width() - text::str_width(info + keys) - 2 > x + 2) info += keys;
    const int iw = text::str_width(info);
    if (p.width() - iw - 2 > x + 2) p.text(p.width() - iw - 2, 0, info, Style{th.dim, bg});
  }

 private:
  void pop_char() {
    while (!query_.empty() && (uint8_t(query_.back()) & 0xC0) == 0x80) query_.pop_back();
    if (!query_.empty()) query_.pop_back();
  }
  bool active_ = false;
  bool all_chats_ = false;
  std::string query_;
};

}  // namespace mico
