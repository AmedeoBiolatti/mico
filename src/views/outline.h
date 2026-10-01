#pragma once
#include <string>
#include <vector>

#include "ui/app.h"
#include "views/chat_render.h"

namespace mico {

// Ctrl+G in a chat: its outline as a picker — your messages, the files the
// agent edited, failed tool calls, questions — newest first, the cursor on
// where the view is now. Choosing one sends "goto:<offset>" to `owner`, which
// hands it to handle_outline_action().
inline void open_outline(App* app, Pane* owner, ChatRenderer& chat, int budget_ms = 700) {
  std::vector<ChatRenderer::OutlineEntry> entries;
  const bool complete = chat.outline(entries, budget_ms);
  const Theme& th = app->theme();
  const uint64_t here = chat.view_offset();
  std::vector<PickItem> items;
  int cursor = -1;
  for (size_t k = entries.size(); k-- > 0;) {
    const auto& e = entries[k];
    PickItem it;
    it.label = e.label;
    it.detail = e.detail;
    it.id = "goto:" + std::to_string(e.offset);
    switch (e.kind) {
      case 'u': it.lead = "\xE2\x80\xBA"; it.lead_color = th.user; break;       // ›
      case 'e': it.lead = "\xE2\x9C\x8E"; it.lead_color = th.tool; break;       // ✎
      case 'x': it.lead = "\xE2\x9C\x97"; it.lead_color = th.err; break;        // ✗
      case 'q': it.lead = "?"; it.lead_color = th.attention; break;
      default: it.lead = "\xE2\x97\x86"; it.lead_color = th.dim; break;         // ◆
    }
    if (cursor < 0 && e.offset <= here) cursor = int(items.size());
    items.push_back(std::move(it));
  }
  if (!complete) {
    PickItem more;
    more.label = "Read older history\xE2\x80\xA6";
    more.detail = "the start of this chat is not in the outline yet";
    more.id = "outline_more";
    items.push_back(std::move(more));
  }
  if (items.empty()) {
    app->set_status("nothing to outline yet");
    return;
  }
  app->open_picker(owner, "Outline", std::move(items), std::max(0, cursor),
                   "enter go \xC2\xB7 type to filter \xC2\xB7 esc close");
}

// True when `action` came from the outline and was carried out.
inline bool handle_outline_action(App* app, Pane* owner, ChatRenderer& chat, const std::string& action) {
  if (action.starts_with("goto:")) {
    chat.go_to(std::strtoull(action.c_str() + 5, nullptr, 10));
    return true;
  }
  if (action == "outline_more") {
    open_outline(app, owner, chat, 5000);
    return true;
  }
  return false;
}

}  // namespace mico
