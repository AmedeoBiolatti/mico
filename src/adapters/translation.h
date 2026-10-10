#pragma once

#include "model/event.h"
#include <vector>

namespace mico {

// Unknown conversation content must stay visible. Keep a bounded preview;
// the row's source line also lets the reader copy the complete original record.
inline void translation_warning(Arena& arena, std::vector<Event>& out, std::string_view agent,
                                std::string_view kind, std::string_view raw) {
  Event e;
  e.kind = EventKind::Notice;
  e.ok = false;
  e.text = arena.add("Warning: mico cannot display " + std::string(agent) + " " + std::string(kind) +
                     ". Expand to inspect the original content.");
  std::string original = "```json\n" + std::string(raw.substr(0, Arena::kMaxText)) + "\n```";
  if (raw.size() > Arena::kMaxText) original += "\nPreview truncated; copy the original transcript record for the full content.";
  e.detail = arena.add(original);
  out.push_back(e);
}

// Text is read by the adapter, images elsewhere by Conversation::add_images.
// Everything else in a visible content array needs an explicit diagnostic.
inline void warn_content(Arena& arena, std::vector<Event>& out, std::string_view agent,
                         const js::Value& content) {
  if (content.is_string() || content.type == js::Type::Null) return;
  if (!content.is_array()) {
    translation_warning(arena, out, agent, "message content", content.raw);
    return;
  }
  js::scan_array(content.raw, [&](const js::Value& item) {
    std::string_view type;
    bool text = false, image = false;
    if (item.is_object())
      js::scan_object(item.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "type") type = v.body();
        else if (k == "text" && v.is_string()) text = true;
        else if (k == "data" && v.is_string()) image = true;
        else if (k == "image_url" && v.is_string() && v.body().starts_with("data:image/")) image = true;
        else if (k == "source" && v.is_object())
          js::scan_object(v.raw, [&](std::string_view sk, const js::Value& sv) {
            if (sk == "data" && sv.is_string()) image = true;
            return true;
          });
        return true;
      });
    if (text && (type.empty() || type == "text" || type == "input_text" || type == "output_text" || type == "summary_text"))
      return true;
    if (image && (type == "image" || type == "input_image" || type == "output_image")) return true;
    translation_warning(arena, out, agent, type.empty() ? "content block" : "content block " + std::string(type), item.raw);
    return true;
  });
}

}  // namespace mico
