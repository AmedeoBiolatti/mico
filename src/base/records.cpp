#include "base/records.h"

#include "base/json.h"

namespace mico {
namespace {

// Used only on suspicious lines, not on the normal transcript parse path.
// Unlike the field scanners, this checks the whole candidate before recovery
// can promote a nested fragment to a record. Depth and recovery attempts are
// bounded so malformed input cannot monopolize a reader.
class ObjectReader {
 public:
  explicit ObjectReader(std::string_view s) : s_(s) {}

  size_t end(size_t from) {
    limited_ = false;
    at_ = js::detail::skip_ws(s_, from);
    if (at_ >= s_.size() || s_[at_] != '{' || !value(0)) return std::string_view::npos;
    return at_;
  }
  bool limited() const { return limited_; }

 private:
  bool take(char c) {
    at_ = js::detail::skip_ws(s_, at_);
    if (at_ == s_.size() || s_[at_] != c) return false;
    ++at_;
    return true;
  }

  bool string() {
    if (!take('"')) return false;
    while (at_ < s_.size()) {
      const unsigned char c = s_[at_++];
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') continue;
      if (at_ == s_.size()) return false;
      const char escape = s_[at_++];
      if (escape == 'u') {
        for (int n = 0; n < 4; ++n) {
          if (at_ == s_.size()) return false;
          const char h = s_[at_++];
          if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
        }
      } else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos) return false;
    }
    return false;
  }

  bool value(unsigned depth) {
    at_ = js::detail::skip_ws(s_, at_);
    if (at_ == s_.size()) return false;
    if (depth >= 128) { limited_ = true; return false; }
    const char c = s_[at_];
    if (c == '"') return string();
    if (c == '{' || c == '[') {
      ++at_;
      const char close = c == '{' ? '}' : ']';
      if (take(close)) return true;
      for (;;) {
        if (c == '{' && (!string() || !take(':'))) return false;
        if (!value(depth + 1)) return false;
        if (take(close)) return true;
        if (!take(',')) return false;
      }
    }
    for (std::string_view literal : {"true", "false", "null"}) {
      if (s_.substr(at_).starts_with(literal)) { at_ += literal.size(); return true; }
    }
    // JSON number syntax, without converting or allocating.
    if (c == '-') ++at_;
    const auto digit = [&] { return at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9'; };
    if (!digit()) return false;
    if (s_[at_++] != '0') while (digit()) ++at_;
    if (at_ < s_.size() && s_[at_] == '.') {
      ++at_;
      if (!digit()) return false;
      while (digit()) ++at_;
    }
    if (at_ < s_.size() && (s_[at_] == 'e' || s_[at_] == 'E')) {
      ++at_;
      if (at_ < s_.size() && (s_[at_] == '+' || s_[at_] == '-')) ++at_;
      if (!digit()) return false;
      while (digit()) ++at_;
    }
    return true;
  }

  std::string_view s_;
  size_t at_ = 0;
  bool limited_ = false;
};

}  // namespace

RecordRecovery recover_records(std::string_view line) {
  RecordRecovery out;
  const size_t first = js::detail::skip_ws(line, 0);
  if (first >= line.size() || line[first] != '{') return out;
  const size_t key = js::detail::skip_ws(line, first + 1);
  if (key >= line.size() || line[key] != '"') return out;
  const size_t end_key = js::detail::skip_string(line.substr(0, std::min(line.size(), key + 128)), key);
  const size_t colon = js::detail::skip_ws(line, end_key);
  if (colon >= line.size() || line[colon] != ':') return out;
  const auto header = line.substr(first, colon - first + 1);
  // Repeated opening headers catch a cut-off string (including base64).
  // Adjacent braces also catch complete records with a lost newline, even
  // when their first fields differ. A valid nested object is left alone.
  if (line.find(header, colon + 1) == std::string_view::npos &&
      line.find("}{", colon + 1) == std::string_view::npos) return out;
  ObjectReader reader(line);
  const size_t end = reader.end(first);
  if (reader.limited()) return out;  // exceeding the recovery budget proves no damage
  if (end != std::string_view::npos && js::detail::skip_ws(line, end) == line.size()) return out;

  out.damaged = true;
  size_t at = first;
  for (unsigned attempts = 0; attempts < 64 && at < line.size(); ++attempts) {
    const size_t to = reader.end(at);
    if (to != std::string_view::npos) {
      out.records.push_back({at, to - at});
      at = js::detail::skip_ws(line, to);
      if (at < line.size() && line[at] == '{') continue;
    }
    const size_t next = line.find(header, at + (to == std::string_view::npos ? 1 : 0));
    if (next == std::string_view::npos) break;
    at = next;
  }
  return out;
}

}  // namespace mico
