#include "adapters/tool_calls.h"

#include <cstdint>
#include <vector>

namespace mico::tools {

std::string one_line(std::string_view s, size_t max) {
  std::string out;
  bool space = false;
  for (size_t i = 0; i < s.size() && out.size() < max; i++) {
    const char c = s[i];
    if (c == '\n' || c == '\t' || c == '\r' || c == ' ') {
      if (!out.empty()) space = true;
      continue;
    }
    if (space) out += ' ', space = false;
    out += c;
  }
  // Never end inside a character.
  while (!out.empty() && (uint8_t(out.back()) & 0xC0) == 0x80) out.pop_back();
  if (!out.empty() && (uint8_t(out.back()) & 0x80)) out.pop_back();
  return out;
}

std::string text_of(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}

std::string result_text(const js::Value& content) {
  if (content.is_string()) return text_of(content);
  std::string out;
  if (!content.is_array()) return out;
  js::scan_array(content.raw, [&](const js::Value& b) {
    if (!b.is_object()) return true;
    js::scan_object(b.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "text" && v.is_string()) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        js::unescape_append(v.body(), out);
      }
      return true;
    });
    return true;
  });
  return out;
}

std::string subject(const js::Value& input) {
  std::string cmd, other;
  if (input.is_object()) {
    js::scan_object(input.raw, [&](std::string_view k, const js::Value& v) {
      if ((k == "command" || k == "cmd") && v.is_string()) cmd = text_of(v);
      else if ((k == "command" || k == "cmd") && v.is_array()) {
        // ["bash", "-lc", "make"]: the script is what ran.
        std::vector<std::string> parts;
        js::scan_array(v.raw, [&](const js::Value& p) { parts.push_back(text_of(p)); return true; });
        if (parts.size() >= 3 && (parts[1] == "-lc" || parts[1] == "-c")) cmd = parts.back();
        else for (const auto& p : parts) cmd += (cmd.empty() ? "" : " ") + p;
      } else if (other.empty() && (k == "file_path" || k == "path" || k == "notebook_path" || k == "pattern" ||
                                   k == "url" || k == "query" || k == "description")) {
        other = text_of(v);
      }
      return true;
    });
  }
  std::string& s = cmd.empty() ? other : cmd;
  if (s.size() > 4096) s.resize(4096);
  return s;
}

}  // namespace mico::tools
