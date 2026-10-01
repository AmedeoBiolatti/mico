#pragma once

#include "model/event.h"

namespace mico {

// Unwrap the agent's standalone paste envelopes in displayed user turns.
// Keep the payload verbatim, and leave code examples or incomplete envelopes
// alone. This only changes normalized events, never the transcript on disk.
inline Str unwrap_pasted_content(Arena& arena, Str text) {
  const std::string_view source = arena.view(text);
  constexpr std::string_view prefix = "<pasted_content id=\"";
  if (source.find(prefix) == std::string_view::npos) return text;
  std::string clean;
  size_t pos = 0;
  char fence = 0;
  size_t fence_width = 0;
  bool changed = false;
  while (pos < source.size()) {
    const size_t nl = source.find('\n', pos);
    const size_t end = nl == std::string_view::npos ? source.size() : nl;
    auto line = source.substr(pos, end - pos);
    if (line.ends_with('\r')) line.remove_suffix(1);
    auto trimmed = line;
    while (trimmed.starts_with(' ')) trimmed.remove_prefix(1);
    if (trimmed.starts_with("```") || trimmed.starts_with("~~~")) {
      size_t count = 0;
      while (count < trimmed.size() && trimmed[count] == trimmed[0]) ++count;
      if (!fence) { fence = trimmed[0]; fence_width = count; }
      else if (trimmed[0] == fence && count >= fence_width &&
               trimmed.substr(count).find_first_not_of(" \t") == std::string_view::npos) fence = 0;
    }
    if (!fence && line.starts_with(prefix) && line.ends_with("\">")) {
      const auto id = line.substr(prefix.size(), line.size() - prefix.size() - 2);
      // Paste IDs are opaque strings (Claude also uses IDs such as "bdd7").
      // Validate the attribute boundary, not an assumed numeric format.
      if (!id.empty() && id.find_first_of("\"<> \t\r\n") == std::string_view::npos && nl != std::string_view::npos) {
        const std::string close = "</pasted_content id=\"" + std::string(id) + "\">";
        const size_t body = nl + 1;
        size_t next = body;
        while (next < source.size()) {
          const size_t closing_nl = source.find('\n', next);
          const size_t closing_end = closing_nl == std::string_view::npos ? source.size() : closing_nl;
          auto candidate = source.substr(next, closing_end - next);
          if (candidate.ends_with('\r')) candidate.remove_suffix(1);
          if (candidate == close || candidate == "</pasted_content>") {
            size_t body_end = next;
            if (body_end > body && source[body_end - 1] == '\n') --body_end;
            if (body_end > body && source[body_end - 1] == '\r') --body_end;
            clean.append(source.substr(body, body_end - body));
            if (closing_nl != std::string_view::npos) clean += '\n';
            pos = closing_nl == std::string_view::npos ? source.size() : closing_nl + 1;
            changed = true;
            break;
          }
          next = closing_nl == std::string_view::npos ? source.size() : closing_nl + 1;
        }
        // Successful unwrapping has moved beyond this opening line.
        if (pos > end) continue;
      }
    }
    const size_t after = nl == std::string_view::npos ? source.size() : nl + 1;
    clean.append(source.substr(pos, after - pos));
    pos = after;
  }
  return changed ? arena.add(clean) : text;
}

}  // namespace mico
