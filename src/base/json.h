#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// A scanning JSON reader for transcript lines. Not a parser: it builds no tree
// and allocates nothing. Callers walk the members they care about and skip the
// rest, which matters because a single line can carry a hundred kilobytes of
// tool output that never reaches the screen.
//
// The decisive property is early exit. A codex record writes "type" before
// "payload", so a line that is not a response_item is rejected after three
// members, without the huge payload ever being touched.
namespace mico::js {

enum class Type : uint8_t { Null, Bool, Number, String, Object, Array };

struct Value {
  std::string_view raw;  // exact source span; strings still include their quotes
  Type type = Type::Null;

  bool is_string() const { return type == Type::String; }
  bool is_object() const { return type == Type::Object; }
  bool is_array() const { return type == Type::Array; }
  bool is_true() const { return type == Type::Bool && !raw.empty() && raw[0] == 't'; }

  // Contents between the quotes, still escaped. Empty for non-strings.
  std::string_view body() const {
    return type == Type::String && raw.size() >= 2 ? raw.substr(1, raw.size() - 2)
                                                   : std::string_view{};
  }
  // True when the body can be used verbatim, with no unescaping pass.
  bool literal() const { return body().find('\\') == std::string_view::npos; }
};

namespace detail {

inline size_t skip_ws(std::string_view s, size_t i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
  return i;
}

// Returns the index just past the string starting at s[i] == '"'.
// Jumps quote to quote with memchr rather than walking bytes: transcript
// strings are routinely tens of kilobytes of tool output that is only being
// skipped over, and this is the hottest function in the whole parse path.
inline size_t skip_string(std::string_view s, size_t i) {
  const char* base = s.data();
  const size_t n = s.size();
  const size_t body = i + 1;
  size_t p = body;
#if defined(__SSE2__)
  // Tool output quoted in a transcript has an escaped quote every few dozen
  // bytes, and a memchr per quote costs more in calls than in bytes; base64
  // has none, and memchr crosses it fastest. So: memchr after a block with no
  // quote in it, and sixteen bytes at a time, every quote settled here, while
  // they keep coming.
  const __m128i quote = _mm_set1_epi8('"');
  bool skip = true;
  while (p + 16 <= n) {
    if (skip) {
      const char* q = static_cast<const char*>(memchr(base + p, '"', n - p));
      if (!q) return n;
      p = size_t(q - base);
      if (p + 16 > n) break;
    }
    unsigned mask = unsigned(_mm_movemask_epi8(
        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(base + p)), quote)));
    skip = mask == 0;
    while (mask) {
      const size_t qi = p + size_t(__builtin_ctz(mask));
      size_t b = qi;
      while (b > body && base[b - 1] == '\\') b--;
      if (((qi - b) & 1) == 0) return qi + 1;
      mask &= mask - 1;
    }
    p += 16;
  }
#endif
  for (;;) {
    if (p >= n) return n;

    const char* q = static_cast<const char*>(memchr(base + p, '"', n - p));
    if (!q) return n;

    const size_t qi = size_t(q - base);
    // A quote is a terminator only if preceded by an even number of backslashes.
    size_t b = qi;
    while (b > body && base[b - 1] == '\\') b--;
    if (((qi - b) & 1) == 0) return qi + 1;
    p = qi + 1;
  }
}

// Returns the index just past the value starting at s[i], and its type.
inline size_t skip_value(std::string_view s, size_t i, Type* type) {
  i = skip_ws(s, i);
  if (i >= s.size()) { *type = Type::Null; return i; }
  char c = s[i];

  if (c == '"') { *type = Type::String; return skip_string(s, i); }

  if (c == '{' || c == '[') {
    *type = c == '{' ? Type::Object : Type::Array;
    int depth = 0;
    while (i < s.size()) {
      char d = s[i];
      if (d == '"') { i = skip_string(s, i); continue; }
      if (d == '{' || d == '[') depth++;
      else if (d == '}' || d == ']') {
        depth--;
        if (depth == 0) return i + 1;
      }
      i++;
    }
    return i;
  }

  if (c == 't' || c == 'f') *type = Type::Bool;
  else if (c == 'n') *type = Type::Null;
  else *type = Type::Number;
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
  return i;
}

}  // namespace detail

// Invokes fn(key, Value) for each member of the object in `s`. `key` excludes
// quotes and is assumed unescaped, which holds for every field these agents
// write. Returning false from fn stops the scan — that is the early exit.
template <class F>
bool scan_object(std::string_view s, F&& fn) {
  size_t i = detail::skip_ws(s, 0);
  if (i >= s.size() || s[i] != '{') return false;
  i++;

  for (;;) {
    i = detail::skip_ws(s, i);
    if (i >= s.size() || s[i] == '}') return true;
    if (s[i] == ',') { i++; continue; }
    if (s[i] != '"') return true;  // malformed; stop rather than spin

    size_t key_end = detail::skip_string(s, i);
    std::string_view key = s.substr(i + 1, key_end - i - 2);
    i = detail::skip_ws(s, key_end);
    if (i >= s.size() || s[i] != ':') return true;
    i++;

    size_t vstart = detail::skip_ws(s, i);
    Type t = Type::Null;
    size_t vend = detail::skip_value(s, vstart, &t);
    if (!fn(key, Value{s.substr(vstart, vend - vstart), t})) return true;
    i = vend;
  }
}

// The body of the JSON string at the start of `s`, still escaped; empty when
// `s` does not start with one. For the `rest` handed out by scan_keys.
inline std::string_view string_body(std::string_view s) {
  if (s.empty() || s[0] != '"') return {};
  const size_t end = detail::skip_string(s, 0);
  return end >= 2 ? s.substr(1, end - 2) : std::string_view{};
}

// Like scan_object, but calls fn(key, rest), where `rest` runs from the start
// of the member's value to the end of `s`, and measures the value only when fn
// returns true to move on. A caller that stops at a key never pays to skip
// what follows it: for a record whose payload is tens of kilobytes, that skip
// is most of the cost of reading it. scan_object() on `rest` then reads the
// value itself, stopping at its own closing brace.
template <class F>
bool scan_keys(std::string_view s, F&& fn) {
  size_t i = detail::skip_ws(s, 0);
  if (i >= s.size() || s[i] != '{') return false;
  i++;

  for (;;) {
    i = detail::skip_ws(s, i);
    if (i >= s.size() || s[i] == '}') return true;
    if (s[i] == ',') { i++; continue; }
    if (s[i] != '"') return true;  // malformed; stop rather than spin

    size_t key_end = detail::skip_string(s, i);
    std::string_view key = s.substr(i + 1, key_end - i - 2);
    i = detail::skip_ws(s, key_end);
    if (i >= s.size() || s[i] != ':') return true;
    i++;

    size_t vstart = detail::skip_ws(s, i);
    if (!fn(key, s.substr(vstart))) return true;
    Type t = Type::Null;
    i = detail::skip_value(s, vstart, &t);
  }
}

// Invokes fn(Value) for each element of the array in `s`.
template <class F>
bool scan_array(std::string_view s, F&& fn) {
  size_t i = detail::skip_ws(s, 0);
  if (i >= s.size() || s[i] != '[') return false;
  i++;

  for (;;) {
    i = detail::skip_ws(s, i);
    if (i >= s.size() || s[i] == ']') return true;
    if (s[i] == ',') { i++; continue; }
    Type t = Type::Null;
    size_t vend = detail::skip_value(s, i, &t);
    if (!fn(Value{s.substr(i, vend - i), t})) return true;
    i = vend;
  }
}

// Appends the unescaped body of a JSON string to `out`.
void unescape_append(std::string_view body, std::string& out);

// The JSON string at the start of `s`, unescaped, but no more than about its
// first `max` bytes: for a field of which only the head is ever read, so that
// a megabyte of tool output is neither walked to its end nor copied. Empty
// when `s` does not start with a string.
inline std::string string_prefix(std::string_view s, size_t max) {
  std::string out;
  if (s.empty() || s[0] != '"') return out;
  const std::string_view window = s.substr(0, std::min(s.size(), max + 2));
  const size_t end = detail::skip_string(window, 0);
  unescape_append(end >= 2 && window[end - 1] == '"' ? window.substr(1, end - 2) : window.substr(1), out);
  return out;
}
// `s` as a JSON string, quotes included.
std::string quote(std::string_view s);

}  // namespace mico::js
