#pragma once
#include <string>
#include <string_view>

#include "base/json.h"

// Reading tool calls out of transcripts: what several agents' records have in
// common.
namespace mico::tools {

// `s` on one line, whitespace runs collapsed, at most `max` bytes, never cut
// inside a character.
std::string one_line(std::string_view s, size_t max = 300);
// A JSON string's text, unescaped; empty for anything else.
std::string text_of(const js::Value& v);
// What a call works on, from its arguments: the command line, else a file,
// a pattern or a URL.
std::string subject(const js::Value& input);
// What a tool result printed: a string, or the text blocks of an array of
// content blocks ({"type": "text", "text": …}), one after another.
std::string result_text(const js::Value& content);
// The last component of a path.
inline std::string_view basename(std::string_view p) {
  const size_t slash = p.rfind('/');
  return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

}  // namespace mico::tools
