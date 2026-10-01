#include "views/json_view.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "term/text.h"

namespace mico::json_view {
namespace {

constexpr int kMaxDepth = 64;
constexpr size_t kFoldString = 48;  // folded, a string is cut past this many bytes
constexpr int kInlineWidth = 72;
constexpr size_t kBigLines = 80;     // longer than this, it opens partly closed     // plain values on one line up to this wide

struct Node {
  enum Kind : uint8_t { Scalar, Object, Array } kind = Scalar;
  int id = -1;           // a container's number, in document order
  std::string_view raw;  // a scalar's source, a key's source (quotes kept)
  std::vector<std::pair<std::string_view, Node>> members;  // objects: key, value; arrays: "", value
};

struct Parser {
  std::string_view s;
  size_t i = 0;
  int next_id = 0;
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
  }
  bool string(std::string_view& out) {
    if (i >= s.size() || s[i] != '"') return false;
    const size_t a = i++;
    while (i < s.size() && s[i] != '"') {
      if (uint8_t(s[i]) < 0x20) return false;
      i += s[i] == '\\' ? 2 : 1;
    }
    if (i >= s.size()) return false;
    out = s.substr(a, ++i - a);
    return true;
  }
  bool value(Node& n, int depth) {
    if (depth > kMaxDepth) return false;
    ws();
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '{' || c == '[') {
      n.kind = c == '{' ? Node::Object : Node::Array;
      n.id = next_id++;
      const char close = c == '{' ? '}' : ']';
      i++;
      ws();
      if (i < s.size() && s[i] == close) { i++; return true; }
      for (;;) {
        std::string_view key;
        if (n.kind == Node::Object) {
          ws();
          if (!string(key)) return false;
          ws();
          if (i >= s.size() || s[i] != ':') return false;
          i++;
        }
        n.members.emplace_back(key, Node{});
        if (!value(n.members.back().second, depth + 1)) return false;
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == close) { i++; return true; }
        return false;
      }
    }
    std::string_view sv;
    if (c == '"') {
      if (!string(sv)) return false;
      n.raw = sv;
      return true;
    }
    const size_t a = i;
    for (std::string_view w : {"true", "false", "null"})
      if (s.compare(i, w.size(), w) == 0) {
        i += w.size();
        n.raw = s.substr(a, w.size());
        return true;
      }
    if (c == '-' || (c >= '0' && c <= '9')) {
      i++;
      while (i < s.size() && (std::isdigit(uint8_t(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                              s[i] == '+' || s[i] == '-'))
        i++;
      n.raw = s.substr(a, i - a);
      return true;
    }
    return false;
  }
};

struct Printer {
  bool folded;
  std::string& out;
  void scalar(std::string_view raw) {
    if (folded && raw.size() > kFoldString && raw.front() == '"') {
      // Cut on a character boundary, and not inside an escape.
      size_t cut = kFoldString;
      while (cut > 1 && (uint8_t(raw[cut]) & 0xC0) == 0x80) cut--;
      size_t bs = cut;
      while (bs > 1 && raw[bs - 1] == '\\') bs--;
      if ((cut - bs) % 2) cut--;
      out.append(raw.substr(0, cut));
      out += "\xE2\x80\xA6\"";  // …"
      return;
    }
    out.append(raw);
  }
  // Plain values that fit on one line stay on one.
  bool flat(const Node& n, std::string& line) {
    if (n.members.size() > 16) return false;
    line = "[";
    for (size_t k = 0; k < n.members.size(); k++) {
      const Node& m = n.members[k].second;
      if (m.kind != Node::Scalar) return false;
      if (k) line += ", ";
      line.append(m.raw);
      if (int(line.size()) > kInlineWidth) return false;
    }
    line += "]";
    return true;
  }
  // Folded: one line, the top level's members with what they hold counted.
  void outline(const Node& n, int depth) {
    if (n.kind == Node::Scalar) return scalar(n.raw);
    const bool obj = n.kind == Node::Object;
    const size_t m = n.members.size();
    if (m == 0) {
      out += obj ? "{}" : "[]";
      return;
    }
    if (depth > 0) {
      out += obj ? "{\xE2\x80\xA6" : "[\xE2\x80\xA6";  // {… […
      out += std::to_string(m);
      out += obj ? (m == 1 ? " key}" : " keys}") : (m == 1 ? " item]" : " items]");
      return;
    }
    out += obj ? "{ " : "[ ";
    for (size_t k = 0; k < m; k++) {
      if (k) out += ", ";
      if (out.size() > 400) {  // more than the rows will show
        out += "\xE2\x80\xA6";
        break;
      }
      if (obj) {
        out.append(n.members[k].first);
        out += ": ";
      }
      outline(n.members[k].second, depth + 1);
    }
    out += obj ? " }" : " ]";
  }
  // Expanded: line by line, each container's opening line noting which it
  // is, so a reader can close it; a closed one is its summary.
  struct Line {
    std::string text;
    int node = -1;
    bool closed = false;
  };
  std::vector<Line>* lines = nullptr;
  std::function<bool(const Node&, int)> closed = [](const Node&, int) { return false; };
  std::string summary(const Node& n) {
    const bool obj = n.kind == Node::Object;
    const size_t m = n.members.size();
    std::string t = obj ? "{\xE2\x80\xA6" : "[\xE2\x80\xA6";
    t += std::to_string(m);
    t += obj ? (m == 1 ? " key}" : " keys}") : (m == 1 ? " item]" : " items]");
    return t;
  }
  void node(const Node& n, int depth, std::string prefix, bool comma) {
    const char* tail = comma ? "," : "";
    if (n.kind == Node::Scalar) {
      std::string& o = out;
      const size_t at = o.size();
      scalar(n.raw);
      lines->push_back({prefix + o.substr(at) + tail});
      o.resize(at);
      return;
    }
    const bool obj = n.kind == Node::Object;
    std::string line;
    if (n.members.empty() || (!obj && flat(n, line))) {
      lines->push_back({prefix + (n.members.empty() ? (obj ? "{}" : "[]") : line) + tail});
      return;
    }
    if (closed(n, depth)) {
      lines->push_back({prefix + summary(n) + tail, n.id, true});
      return;
    }
    lines->push_back({prefix + (obj ? "{" : "["), n.id, false});
    const std::string pad(size_t(2 * (depth + 1)), ' ');
    for (size_t k = 0; k < n.members.size(); k++) {
      std::string p = pad;
      if (obj) {
        p.append(n.members[k].first);
        p += ": ";
      }
      node(n.members[k].second, depth + 1, std::move(p), k + 1 < n.members.size());
    }
    lines->push_back({std::string(size_t(2 * depth), ' ') + (obj ? "}" : "]") + tail});
  }
  // Lines the whole of it takes, open.
  size_t count(const Node& n) {
    std::string line;
    if (n.kind == Node::Scalar || n.members.empty() || (n.kind == Node::Array && flat(n, line))) return 1;
    size_t c = 2;
    for (const auto& m : n.members) c += count(m.second);
    return c;
  }
};

}  // namespace

bool pretty(std::string_view text, bool folded, std::string& out, Folding* folding) {
  Parser p{text};
  p.ws();
  if (p.i >= text.size() || (text[p.i] != '{' && text[p.i] != '[')) return false;
  Node root;
  if (!p.value(root, 0)) return false;
  p.ws();
  if (p.i != text.size()) return false;
  out.clear();
  Printer pr{folded, out};
  if (folded) {
    pr.outline(root, 0);
    out += '\n';
    return true;
  }
  std::vector<Printer::Line> lines;
  pr.lines = &lines;
  if (folding) {
    // A big one opens with what is below its top two levels closed; the
    // reader's clicks flip containers from there.
    const bool big = pr.count(root) > kBigLines;
    pr.closed = [big, folding](const Node& n, int depth) {
      const bool dflt = big && depth >= 2;
      return folding->flipped && folding->flipped->count(n.id) ? !dflt : dflt;
    };
    folding->line_nodes.clear();
  }
  pr.node(root, 0, std::string(), false);
  for (const auto& l : lines) {
    if (folding) {
      // The mark just before the line's text, under its indentation.
      const size_t lead = std::min(l.text.find_first_not_of(' '), l.text.size());
      out.append(l.text, 0, lead);
      out += l.node < 0 ? "  " : l.closed ? "\xE2\x96\xB8 " : "\xE2\x96\xBE ";  // ▸ ▾
      out.append(l.text, lead);
      folding->line_nodes.push_back(l.node < 0 || l.node >= 0xFFFF ? 0 : uint16_t(l.node + 1));
    } else {
      out += l.text;
    }
    out += '\n';
  }
  return true;
}

}  // namespace mico::json_view
