#include "views/diagram.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "term/text.h"

namespace mico::diagram {
namespace {

using chart::Figure;
using chart::Piece;

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == ';' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::vector<std::string_view> lines_of(std::string_view src) {
  std::vector<std::string_view> out;
  size_t pos = 0;
  while (pos <= src.size()) {
    size_t nl = src.find('\n', pos);
    if (nl == std::string_view::npos) nl = src.size();
    std::string_view l = trim(src.substr(pos, nl - pos));
    if (!l.empty() && !l.starts_with("%%")) out.push_back(l);
    pos = nl + 1;
  }
  return out;
}

// A label as shown: quotes off, <br> and \n as spaces, markdown backticks off.
std::string clean(std::string_view t) {
  t = trim(t);
  if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
  if (t.size() >= 2 && t.front() == '`' && t.back() == '`') t = t.substr(1, t.size() - 2);
  std::string out;
  for (size_t i = 0; i < t.size(); i++) {
    if (t.compare(i, 3, "<br") == 0) {
      const size_t e = t.find('>', i);
      if (e != std::string_view::npos) {
        out += ' ';
        i = e;
        continue;
      }
    }
    if (t.compare(i, 2, "\\n") == 0) { out += ' '; i++; continue; }
    if (t.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; continue; }
    if (t.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; continue; }
    if (t.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; continue; }
    if (t.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; continue; }
    out += t[i];
  }
  return std::string(trim(out));
}

// Words to lines of at most `max` columns (a longer word keeps its own line).
std::vector<std::string> wrap(const std::string& s, int max) {
  std::vector<std::string> out;
  std::string line;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(' ', i);
    if (j == std::string::npos) j = s.size();
    const std::string word = s.substr(i, j - i);
    i = j + 1;
    if (word.empty()) continue;
    if (!line.empty() && text::str_width(line) + 1 + text::str_width(word) > max) {
      out.push_back(line);
      line.clear();
    }
    if (!line.empty()) line += ' ';
    line += word;
  }
  if (!line.empty() || out.empty()) out.push_back(line);
  return out;
}

int widest(const std::vector<std::string>& lines) {
  int w = 0;
  for (const auto& l : lines) w = std::max(w, text::str_width(l));
  return w;
}

// --- a grid of cells, lines drawn as masks so junctions join ------------

enum : uint8_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8 };
enum : uint8_t { kInkText = chart::kInkText, kInkLine = chart::kInkAxis, kInkBox = chart::kInkSeries,
                 kInkTitle = chart::kInkTitle };

struct Grid {
  int w = 0, h = 0;
  std::vector<char32_t> cp;
  std::vector<uint8_t> ink, mask, style;  // style: 1 dotted, 2 solid, 3 thick (0: none yet)
  void resize(int w_, int h_) {
    w = w_, h = h_;
    cp.assign(size_t(w) * size_t(h), U' ');
    ink.assign(cp.size(), kInkText);
    mask.assign(cp.size(), 0);
    style.assign(cp.size(), 0);
  }
  bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  size_t at(int x, int y) const { return size_t(y) * size_t(w) + size_t(x); }
  bool blank(int x, int y) const { return in(x, y) && cp[at(x, y)] == U' ' && mask[at(x, y)] == 0; }
  // A plain stretch of horizontal line, which a label may sit in.
  bool bare_hline(int x, int y) const {
    return in(x, y) && ink[at(x, y)] == kInkLine && mask[at(x, y)] == (kLeft | kRight);
  }
  void put(int x, int y, char32_t c, uint8_t k) {
    if (!in(x, y)) return;
    cp[at(x, y)] = c;
    ink[at(x, y)] = k;
    mask[at(x, y)] = 0;
  }
  void text(int x, int y, std::string_view s, uint8_t k) {
    for (size_t i = 0; i < s.size();) {
      const char32_t c = text::decode(s, i);
      const int cw = std::max(1, text::cp_width(c));
      put(x, y, c, k);
      if (cw == 2) put(x + 1, y, 0, k);  // the right half of a wide character
      x += cw;
    }
  }
  void link(int x, int y, uint8_t m, uint8_t st) {
    if (!in(x, y) || (cp[at(x, y)] != U' ' && mask[at(x, y)] == 0)) return;  // never through text or a box
    mask[at(x, y)] |= m;
    // Where lines share cells, thick beats solid beats dotted.
    static constexpr uint8_t kRank[3] = {2, 1, 3};
    if (kRank[st] > style[at(x, y)]) style[at(x, y)] = kRank[st];
  }
  void hline(int x0, int x1, int y, uint8_t st) {
    if (x0 > x1) std::swap(x0, x1);
    for (int x = x0; x <= x1; x++)
      link(x, y, uint8_t((x > x0 ? kLeft : 0) | (x < x1 ? kRight : 0)), st);
  }
  void vline(int x, int y0, int y1, uint8_t st) {
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; y++)
      link(x, y, uint8_t((y > y0 ? kUp : 0) | (y < y1 ? kDown : 0)), st);
  }
  // Lines become box-drawing characters, joined where they meet.
  void finish() {
    static const char32_t kSolid[16] = {U' ', U'│', U'│', U'│', U'─', U'┘', U'┐', U'┤',
                                        U'─', U'└', U'┌', U'├', U'─', U'┴', U'┬', U'┼'};
    for (size_t i = 0; i < cp.size(); i++) {
      const uint8_t m = mask[i];
      if (!m) continue;
      char32_t c = kSolid[m];
      if (m == (kUp | kDown) || m == kUp || m == kDown) c = style[i] == 1 ? U'┆' : style[i] == 3 ? U'┃' : U'│';
      if (m == (kLeft | kRight) || m == kLeft || m == kRight) c = style[i] == 1 ? U'┄' : style[i] == 3 ? U'━' : U'─';
      cp[i] = c;
      ink[i] = kInkLine;
    }
  }
  void box(int x, int y, int bw, int bh, char shape, const std::vector<std::string>& lines) {
    // [ ] square, ( ) rounded, { } a decision (double).
    static const char32_t kSq[6] = {U'┌', U'┐', U'└', U'┘', U'─', U'│'};
    static const char32_t kRound[6] = {U'╭', U'╮', U'╰', U'╯', U'─', U'│'};
    static const char32_t kDouble[6] = {U'╔', U'╗', U'╚', U'╝', U'═', U'║'};
    const char32_t* c = shape == '(' ? kRound : shape == '{' ? kDouble : kSq;
    put(x, y, c[0], kInkBox);
    put(x + bw - 1, y, c[1], kInkBox);
    put(x, y + bh - 1, c[2], kInkBox);
    put(x + bw - 1, y + bh - 1, c[3], kInkBox);
    for (int i = 1; i < bw - 1; i++) put(x + i, y, c[4], kInkBox), put(x + i, y + bh - 1, c[4], kInkBox);
    for (int j = 1; j < bh - 1; j++) put(x, y + j, c[5], kInkBox), put(x + bw - 1, y + j, c[5], kInkBox);
    for (int j = 1; j < bh - 1; j++)
      for (int i = 1; i < bw - 1; i++) put(x + i, y + j, U' ', kInkText);  // inside is no place for a line
    const int top = y + (bh - int(lines.size())) / 2;
    for (size_t k = 0; k < lines.size(); k++)
      text(x + (bw - text::str_width(lines[k])) / 2, top + int(k), lines[k], kInkText);
  }
  void to_figure(Figure& f) const {
    f.rows.clear();
    f.width = 0;
    int last_row = h - 1;
    const auto empty_row = [&](int y) {
      for (int x = 0; x < w; x++)
        if (cp[at(x, y)] != U' ') return false;
      return true;
    };
    while (last_row >= 0 && empty_row(last_row)) last_row--;
    // Columns blank in every row, on the left, are not part of the drawing.
    int first = w;
    for (int y = 0; y <= last_row; y++)
      for (int x = 0; x < first; x++)
        if (cp[at(x, y)] != U' ') {
          first = x;
          break;
        }
    std::string cell;
    for (int y = 0; y <= last_row; y++) {
      std::vector<Piece> row;
      int last = w - 1;
      while (last >= 0 && cp[at(last, y)] == U' ') last--;
      for (int x = first; x <= last;) {
        const uint8_t k = ink[at(x, y)];
        Piece p;
        p.ink = k;
        while (x <= last && ink[at(x, y)] == k) {
          const char32_t c = cp[at(x, y)];
          if (c) {
            cell.clear();
            text::encode(c, cell);
            p.text += cell;
          }
          x++;
        }
        row.push_back(std::move(p));
      }
      f.width = std::max(f.width, last + 1 - first);
      f.rows.push_back(std::move(row));
    }
  }
};

// --- flowcharts and state diagrams -------------------------------------
//
// Layered (Sugiyama-style): edges that close a cycle are turned around,
// nodes go in layers by longest path, edges longer than a layer pass through
// stand-ins, layers are ordered by barycentres, and nodes are placed near
// their neighbours. Edges leave a node from one port and bend in a lane of
// their own source between layers. Worked out in (u, v): u along a layer,
// v across layers, so one layout serves both top-down and left-right.

constexpr int kWrap = 24;  // a node's label wraps at this many columns

struct Node {
  std::string id, label;
  std::vector<std::string> lines;
  char shape = '[';
  bool dummy = false;
  bool rev_port = false;  // has turned-around edges, which use a second port
  int cluster = -1;       // the (outermost) subgraph it is in
  int layer = 0, order = 0;
  int u = 0, ulen = 1, v = 0, vlen = 1;
  int fport() const { return dummy ? u : u + (ulen - 1) / 2; }
  int rport() const { return dummy ? u : u + ulen - 2; }
  int port(bool rev) const { return rev ? rport() : fport(); }
};
struct Edge {
  int from = 0, to = 0;
  std::string label;
  uint8_t style = 0;  // 0 solid, 1 dotted, 2 thick
  bool head_to = true, head_from = false;
  bool rev = false;  // turned around to keep the layers acyclic: drawn upwards
};

struct Graph {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::map<std::string, int> index;
  std::vector<std::string> clusters;  // subgraph titles; nested ones count as their outermost
  int open_cluster = -1;              // while inside a subgraph block
  bool lr = false;
  int node(const std::string& id) {
    auto it = index.find(id);
    int n;
    if (it != index.end()) {
      n = it->second;
    } else {
      Node nd;
      nd.id = id;
      nd.label = id;
      nodes.push_back(nd);
      n = int(nodes.size()) - 1;
      index[id] = n;
    }
    // Named inside a subgraph: it belongs there (unless already elsewhere).
    if (open_cluster >= 0 && nodes[size_t(n)].cluster < 0) nodes[size_t(n)].cluster = open_cluster;
    return n;
  }
};

bool id_char(char c) { return std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.' || uint8_t(c) >= 0x80; }

// Reads "id", "id[label]", "id(label)", "id{label}"… at i. Returns -1 if
// none. In a state diagram [*] is the start as a source, the end as a target.
int read_node(Graph& g, std::string_view s, size_t& i, bool state, bool target) {
  while (i < s.size() && s[i] == ' ') i++;
  if (state && s.compare(i, 3, "[*]") == 0) {
    i += 3;
    const int n = g.node(target ? "[*]end" : "[*]");
    g.nodes[size_t(n)].label = target ? "\xE2\x97\x89" : "\xE2\x97\x8F";  // ◉ ●
    g.nodes[size_t(n)].shape = '(';
    return n;
  }
  size_t j = i;
  while (j < s.size() && id_char(s[j])) {
    // "-->" begins an edge, not more of the id.
    if ((s[j] == '-' || s[j] == '.') && (s.compare(j, 2, "--") == 0 || s.compare(j, 2, "-.") == 0)) break;
    j++;
  }
  if (j == i) return -1;
  const int n = g.node(std::string(s.substr(i, j - i)));
  i = j;
  if (i < s.size() && std::strchr("[({>", s[i])) {
    static constexpr std::pair<std::string_view, std::string_view> kShapes[] = {
        {"(((", ")))"}, {"((", "))"}, {"([", "])"}, {"[[", "]]"}, {"[(", ")]"}, {"{{", "}}"}, {"[/", "/]"},
        {"[\\", "\\]"}, {"[/", "\\]"}, {"[\\", "/]"}, {"[", "]"},  {"(", ")"},   {"{", "}"},   {">", "]"}};
    for (const auto& [open, close] : kShapes) {
      if (s.compare(i, open.size(), open) != 0) continue;
      const size_t e = s.find(close, i + open.size());
      if (e == std::string_view::npos) continue;
      g.nodes[size_t(n)].label = clean(s.substr(i + open.size(), e - i - open.size()));
      g.nodes[size_t(n)].shape = open[0] == '{' ? '{' : open[0] == '(' || open == "([" ? '(' : '[';
      i = e + close.size();
      break;
    }
  }
  if (s.compare(i, 3, ":::") == 0) {  // a class, which only styles it
    i += 3;
    while (i < s.size() && id_char(s[i])) i++;
  }
  return n;
}

// Reads an edge operator at i: -->, ---, -.->, ==>, -->|label|, -- label -->…
bool read_edge(std::string_view s, size_t& i, Edge& e) {
  while (i < s.size() && s[i] == ' ') i++;
  const size_t start = i;
  bool back_head = false;
  if (i < s.size() && (s[i] == '<' || ((s[i] == 'x' || s[i] == 'o') && i + 1 < s.size() && s[i + 1] == '-'))) {
    back_head = true;
    i++;
  }
  const size_t op_at = i;
  while (i < s.size() && (s[i] == '-' || s[i] == '=' || s[i] == '.')) i++;
  if (i == op_at || i - op_at < 2) {
    i = start;
    return false;
  }
  const std::string_view op = s.substr(op_at, i - op_at);
  e.style = op.find('.') != std::string_view::npos ? 1 : op.find('=') != std::string_view::npos ? 2 : 0;
  e.head_to = false;
  if (i < s.size() && (s[i] == '>' || ((s[i] == 'x' || s[i] == 'o') && (i + 1 == s.size() || !id_char(s[i + 1]))))) {
    e.head_to = true;
    i++;
  } else if (op == "--" || op == "==" || op == "-.") {
    // "-- label -->": the label runs to the closing half of the arrow.
    size_t close = std::string_view::npos;
    for (std::string_view c : {"-->", "---", "==>", "===", ".->", ".-"}) {
      const size_t k = s.find(c, i);
      if (k < close) close = k;
    }
    if (close != std::string_view::npos) {
      e.label = clean(s.substr(i, close - i));
      i = close;
      while (i < s.size() && (s[i] == '-' || s[i] == '=' || s[i] == '.')) i++;
      if (i < s.size() && s[i] == '>') { e.head_to = true; i++; }
    }
  }
  e.head_from = back_head;
  while (i < s.size() && s[i] == ' ') i++;
  if (i < s.size() && s[i] == '|') {
    const size_t close = s.find('|', i + 1);
    if (close != std::string_view::npos) {
      e.label = clean(s.substr(i + 1, close - i - 1));
      i = close + 1;
    }
  }
  return true;
}

bool parse_flow(const std::vector<std::string_view>& lines, Graph& g, bool state) {
  const std::string_view head = lines.front();
  g.lr = head.find("LR") != std::string_view::npos || head.find("RL") != std::string_view::npos;
  int depth = 0;  // subgraphs open
  const auto open = [&](std::string title) {
    if (depth++ == 0) {
      g.clusters.push_back(std::move(title));
      g.open_cluster = int(g.clusters.size()) - 1;
    }
  };
  const auto close = [&] {
    if (depth > 0 && --depth == 0) g.open_cluster = -1;
  };
  for (size_t li = 1; li < lines.size(); li++) {
    const std::string_view l = lines[li];
    if (!state && (l == "subgraph" || l.starts_with("subgraph "))) {
      // subgraph id, subgraph id [Title], subgraph "Title"
      std::string_view rest = trim(l.substr(8));
      std::string title(rest);
      if (const size_t b = rest.find('['); b != std::string_view::npos && rest.ends_with("]"))
        title = clean(rest.substr(b + 1, rest.size() - b - 2));
      else
        title = clean(rest);
      open(title);
      continue;
    }
    if (!state && l == "end") {
      close();
      continue;
    }
    if (state && l.starts_with("state ") && l.ends_with("{")) {
      // A composite state: its states drawn inside a box named for it.
      std::string_view name = trim(l.substr(6, l.size() - 7));
      if (const size_t as = name.find(" as "); as != std::string_view::npos) name = name.substr(0, as);
      open(clean(name));
      continue;
    }
    if (state && l == "}") {
      close();
      continue;
    }
    if (l.starts_with("subgraph") || l == "end" || l.starts_with("classDef") || l.starts_with("class ") ||
        l.starts_with("style") || l.starts_with("linkStyle") || l.starts_with("click") ||
        l.starts_with("direction") || l.starts_with("note") || l.starts_with("accTitle") ||
        l.starts_with("accDescr") || l == "}" || l == "--")
      continue;
    if (state && l.starts_with("state ")) {
      // state "Long name" as S, or state S { … }
      std::string_view rest = trim(l.substr(6));
      if (rest.ends_with("{")) rest = trim(rest.substr(0, rest.size() - 1));
      const size_t as = rest.find(" as ");
      if (as != std::string_view::npos) {
        const int n = g.node(std::string(trim(rest.substr(as + 4))));
        g.nodes[size_t(n)].label = clean(rest.substr(0, as));
        g.nodes[size_t(n)].shape = '(';
      }
      continue;
    }
    // A state diagram's "A : description" names a state.
    if (state && l.find("-->") == std::string_view::npos) {
      const size_t colon = l.find(':');
      if (colon != std::string_view::npos) {
        const int n = g.node(std::string(trim(l.substr(0, colon))));
        g.nodes[size_t(n)].label = clean(l.substr(colon + 1));
        g.nodes[size_t(n)].shape = '(';
      } else if (!l.empty() && id_char(l[0])) {
        g.nodes[size_t(g.node(std::string(l)))].shape = '(';
      }
      continue;
    }
    std::string_view rest = l;
    std::string state_label;
    if (state) {
      const size_t colon = l.rfind(':');
      if (colon != std::string_view::npos && colon > l.find("-->")) {
        state_label = clean(l.substr(colon + 1));
        rest = trim(l.substr(0, colon));
      }
    }
    size_t i = 0;
    const auto read_group = [&](bool target) {
      std::vector<int> out;
      for (;;) {
        const int n = read_node(g, rest, i, state, target);
        if (n < 0) break;
        if (state && g.nodes[size_t(n)].id.substr(0, 3) != "[*]") g.nodes[size_t(n)].shape = '(';
        out.push_back(n);
        while (i < rest.size() && rest[i] == ' ') i++;
        if (i < rest.size() && rest[i] == '&') { i++; continue; }
        break;
      }
      return out;
    };
    std::vector<int> from = read_group(false);
    if (from.empty()) continue;
    for (;;) {
      Edge e{};
      if (!read_edge(rest, i, e)) break;
      std::vector<int> to = read_group(true);
      if (to.empty()) break;
      if (!state_label.empty()) e.label = state_label;
      for (int a : from)
        for (int b : to) {
          Edge x = e;
          x.from = a;
          x.to = b;
          g.edges.push_back(x);
        }
      from = to;
    }
  }
  return !g.nodes.empty() && g.nodes.size() <= 150 && g.edges.size() <= 400;
}

struct Arrow {
  int u, v;
  bool forward;
};
struct Label {
  int edge;  // index into the laid-out edges
  int x1, x2, lane;
};

// Lays out and draws `g` (its nodes and edges are rewritten). `compact`
// packs each layer tight instead of placing nodes near their neighbours.
// False when it would be wider than `cols`.
bool layout_flow(Graph g, int cols, bool compact, Grid& grid) {
  const bool lr = g.lr;
  const int n0 = int(g.nodes.size());

  // Self-loops are noted by the node, not drawn as edges.
  std::vector<std::pair<int, std::string>> selfs;
  {
    std::vector<Edge> keep;
    for (const Edge& e : g.edges) {
      if (e.from == e.to) selfs.emplace_back(e.from, e.label);
      else keep.push_back(e);
    }
    g.edges = std::move(keep);
  }

  // Cycles: an edge back to a node still on the depth-first path turns around.
  {
    std::vector<std::vector<int>> out(static_cast<size_t>(n0));
    for (size_t k = 0; k < g.edges.size(); k++) out[size_t(g.edges[k].from)].push_back(int(k));
    std::vector<int> mark(static_cast<size_t>(n0), 0);
    std::vector<int> back;
    const std::function<void(int)> dfs = [&](int v) {
      mark[size_t(v)] = 1;
      for (int k : out[size_t(v)]) {
        const int t = g.edges[size_t(k)].to;
        if (mark[size_t(t)] == 1) back.push_back(k);
        else if (mark[size_t(t)] == 0) dfs(t);
      }
      mark[size_t(v)] = 2;
    };
    for (int v = 0; v < n0; v++)
      if (!mark[size_t(v)]) dfs(v);
    for (int k : back) {
      Edge& e = g.edges[size_t(k)];
      std::swap(e.from, e.to);
      std::swap(e.head_to, e.head_from);
      e.rev = true;
    }
  }

  // Layers by longest path, in topological order; then a source with only
  // far-off children moves down next to them.
  {
    std::vector<int> indeg(static_cast<size_t>(n0), 0), topo;
    for (const Edge& e : g.edges) indeg[size_t(e.to)]++;
    std::vector<int> left = indeg;
    for (int v = 0; v < n0; v++)
      if (!left[size_t(v)]) topo.push_back(v);
    for (size_t k = 0; k < topo.size(); k++)
      for (const Edge& e : g.edges)
        if (e.from == topo[k] && --left[size_t(e.to)] == 0) topo.push_back(e.to);
    if (int(topo.size()) != n0) return false;  // cannot happen once cycles are turned
    for (int v : topo)
      for (const Edge& e : g.edges)
        if (e.from == v) g.nodes[size_t(e.to)].layer = std::max(g.nodes[size_t(e.to)].layer, g.nodes[size_t(v)].layer + 1);
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
      if (indeg[size_t(*it)]) continue;
      int lowest = INT32_MAX;
      for (const Edge& e : g.edges)
        if (e.from == *it) lowest = std::min(lowest, g.nodes[size_t(e.to)].layer);
      if (lowest != INT32_MAX) g.nodes[size_t(*it)].layer = lowest - 1;
    }
  }

  // Edges longer than one layer pass through stand-ins.
  {
    std::vector<Edge> edges;
    for (const Edge& e : g.edges) {
      const int span = g.nodes[size_t(e.to)].layer - g.nodes[size_t(e.from)].layer;
      if (span <= 1) {
        edges.push_back(e);
        continue;
      }
      if (g.nodes.size() > 3000) return false;
      int prev = e.from;
      for (int L = g.nodes[size_t(e.from)].layer + 1; L < g.nodes[size_t(e.to)].layer; L++) {
        Node d;
        d.dummy = true;
        d.layer = L;
        if (g.nodes[size_t(e.from)].cluster == g.nodes[size_t(e.to)].cluster) d.cluster = g.nodes[size_t(e.from)].cluster;
        g.nodes.push_back(d);
        Edge part = e;
        part.from = prev;
        part.to = int(g.nodes.size()) - 1;
        part.head_to = false;
        if (prev != e.from) part.head_from = false;
        part.label.clear();
        edges.push_back(part);
        prev = part.to;
      }
      Edge last = e;
      last.from = prev;
      last.head_from = false;
      edges.push_back(last);
    }
    g.edges = std::move(edges);
  }
  for (const Edge& e : g.edges)
    if (e.rev) g.nodes[size_t(e.from)].rev_port = g.nodes[size_t(e.to)].rev_port = true;

  const int N = int(g.nodes.size());
  int layers = 0;
  for (const Node& nd : g.nodes) layers = std::max(layers, nd.layer + 1);
  std::vector<std::vector<int>> by_layer(static_cast<size_t>(layers));
  for (int v = 0; v < N; v++) by_layer[size_t(g.nodes[size_t(v)].layer)].push_back(v);
  std::vector<std::vector<int>> up(static_cast<size_t>(N)), down(static_cast<size_t>(N));
  for (const Edge& e : g.edges) {
    down[size_t(e.from)].push_back(e.to);
    up[size_t(e.to)].push_back(e.from);
  }

  // A subgraph's members in a layer stand together, where on average they
  // would have stood, so a box can go round them.
  const auto group = [&](std::vector<int>& L) {
    if (g.clusters.empty()) return;
    std::map<int, std::pair<double, int>> mean;  // cluster: sum of positions, count
    for (size_t i = 0; i < L.size(); i++)
      if (const int c = g.nodes[size_t(L[i])].cluster; c >= 0) mean[c].first += double(i), mean[c].second++;
    std::vector<std::pair<double, int>> key(L.size());
    for (size_t i = 0; i < L.size(); i++) {
      const int c = g.nodes[size_t(L[i])].cluster;
      key[i] = {c >= 0 ? mean[c].first / mean[c].second : double(i), c};
    }
    std::vector<size_t> idx(L.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
      if (key[a].first != key[b].first) return key[a].first < key[b].first;
      return key[a].second < key[b].second;
    });
    std::vector<int> out;
    for (size_t i : idx) out.push_back(L[i]);
    L = out;
  };

  // Order within layers: barycentres of the neighbours above, then below.
  for (auto& L : by_layer)
    for (size_t i = 0; i < L.size(); i++) g.nodes[size_t(L[i])].order = int(i);
  for (int sweep = 0; sweep < 8; sweep++) {
    const bool dn = sweep % 2 == 0;
    for (int Li = dn ? 1 : layers - 2; dn ? Li < layers : Li >= 0; Li += dn ? 1 : -1) {
      auto& L = by_layer[size_t(Li)];
      std::vector<double> key(L.size());
      for (size_t i = 0; i < L.size(); i++) {
        const auto& nb = dn ? up[size_t(L[i])] : down[size_t(L[i])];
        double sum = 0;
        for (int o : nb) sum += g.nodes[size_t(o)].order;
        key[i] = nb.empty() ? g.nodes[size_t(L[i])].order : sum / double(nb.size());
      }
      std::vector<size_t> idx(L.size());
      for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
      std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return key[a] < key[b]; });
      std::vector<int> sorted;
      for (size_t i : idx) sorted.push_back(L[i]);
      L = sorted;
      group(L);
      for (size_t i = 0; i < L.size(); i++) g.nodes[size_t(L[i])].order = int(i);
    }
  }
  if (layers == 1) group(by_layer[0]);
  for (auto& L : by_layer)
    for (size_t i = 0; i < L.size(); i++) g.nodes[size_t(L[i])].order = int(i);

  // Sizes: u along the layer, v across.
  for (Node& nd : g.nodes) {
    if (nd.dummy) continue;
    nd.lines = wrap(nd.label, kWrap);
    const int tw = std::max(1, widest(nd.lines)) + 4, th = int(nd.lines.size()) + 2;
    if (!lr) {
      nd.ulen = std::max(5, tw);
      nd.vlen = th;
    } else {
      nd.ulen = std::max(th, nd.rev_port ? 4 : 3);
      nd.vlen = std::max(5, tw);
    }
  }
  // A subgraph's box stands `margin` from its members; neighbours outside
  // it keep their distance from the box.
  const int margin = lr ? 1 : 2;
  const auto gap_between = [&](int a, int b) {
    const Node &na = g.nodes[size_t(a)], &nb = g.nodes[size_t(b)];
    const bool real = !na.dummy && !nb.dummy;
    int gap = lr ? 1 : real ? 3 : 2;
    if (na.cluster != nb.cluster) gap += (na.cluster >= 0 ? margin + 1 : 0) + (nb.cluster >= 0 ? margin + 1 : 0);
    return gap;
  };

  // Places along u: packed, then (unless compact) pulled towards the centres
  // of the neighbours, keeping order and spacing.
  for (auto& L : by_layer) {
    int u = 0;
    for (size_t i = 0; i < L.size(); i++) {
      if (i) u += gap_between(L[i - 1], L[i]);
      g.nodes[size_t(L[i])].u = u;
      u += g.nodes[size_t(L[i])].ulen;
    }
  }
  if (!compact) {
    // Layers start centred on the widest.
    int widest_layer = 0;
    for (auto& L : by_layer)
      if (!L.empty()) widest_layer = std::max(widest_layer, g.nodes[size_t(L.back())].u + g.nodes[size_t(L.back())].ulen);
    for (auto& L : by_layer) {
      if (L.empty()) continue;
      const int shift = (widest_layer - (g.nodes[size_t(L.back())].u + g.nodes[size_t(L.back())].ulen)) / 2;
      for (int v : L) g.nodes[size_t(v)].u += shift;
    }
    const auto centre = [&](int v) { return g.nodes[size_t(v)].u + (g.nodes[size_t(v)].ulen - 1) / 2.0; };
    for (int iter = 0; iter < 6; iter++) {
      const bool dn = iter % 2 == 0;
      for (int Li = dn ? 1 : layers - 2; dn ? Li < layers : Li >= 0; Li += dn ? 1 : -1) {
        auto& L = by_layer[size_t(Li)];
        const size_t m = L.size();
        std::vector<double> want(m), a(m), b(m);
        for (size_t i = 0; i < m; i++) {
          const Node& nd = g.nodes[size_t(L[i])];
          const auto& nb = dn ? up[size_t(L[i])] : down[size_t(L[i])];
          double sum = 0;
          for (int o : nb) sum += centre(o);
          want[i] = nb.empty() ? nd.u : sum / double(nb.size()) - (nd.ulen - 1) / 2.0;
        }
        for (size_t i = 0; i < m; i++)
          a[i] = i ? std::max(want[i], a[i - 1] + g.nodes[size_t(L[i - 1])].ulen + gap_between(L[i - 1], L[i])) : want[i];
        for (size_t i = m; i-- > 0;)
          b[i] = i + 1 < m ? std::min(want[i], b[i + 1] - g.nodes[size_t(L[i])].ulen - gap_between(L[i], L[i + 1])) : want[i];
        for (size_t i = 0; i < m; i++) g.nodes[size_t(L[i])].u = int(std::floor((a[i] + b[i]) / 2));
      }
    }
  }
  {
    int lo = INT32_MAX;
    for (const Node& nd : g.nodes) lo = std::min(lo, nd.u - (nd.cluster >= 0 ? margin + 1 : 0));
    for (Node& nd : g.nodes) nd.u -= lo;
  }
  int U = 0;
  for (const Node& nd : g.nodes) U = std::max(U, nd.u + nd.ulen + (nd.cluster >= 0 ? margin + 1 : 0));

  // Ports, and edges made straight where a box is wide enough to allow it.
  std::vector<int> out_f(static_cast<size_t>(N), 0), out_r(static_cast<size_t>(N), 0),
      in_f(static_cast<size_t>(N), 0), in_r(static_cast<size_t>(N), 0);
  for (const Edge& e : g.edges) {
    (e.rev ? out_r : out_f)[size_t(e.from)]++;
    (e.rev ? in_r : in_f)[size_t(e.to)]++;
  }
  const auto interior = [&](const Node& nd, bool rev, int x) {
    if (nd.dummy) return x == nd.u;
    int lo = nd.u + 1, hi = nd.u + nd.ulen - 2;
    if (nd.rev_port) (rev ? lo : hi) = rev ? nd.fport() + 1 : nd.rport() - 1;
    return x >= lo && x <= hi;
  };
  const size_t E = g.edges.size();
  std::vector<int> x1(E), x2(E), lane(E, -1);
  for (size_t k = 0; k < E; k++) {
    const Edge& e = g.edges[k];
    const Node &a = g.nodes[size_t(e.from)], &b = g.nodes[size_t(e.to)];
    x1[k] = a.port(e.rev);
    x2[k] = b.port(e.rev);
    if (x1[k] == x2[k]) continue;
    if ((e.rev ? in_r : in_f)[size_t(e.to)] == 1 && interior(b, e.rev, x1[k])) x2[k] = x1[k];
    else if ((e.rev ? out_r : out_f)[size_t(e.from)] == 1 && interior(a, e.rev, x2[k])) x1[k] = x2[k];
  }

  // Across layers: each layer as thick as its thickest node, then a gap with
  // a stem, a lane per source that bends, room for labels, and the arrow.
  std::vector<int> V(static_cast<size_t>(layers), g.clusters.empty() ? 0 : 1), T(static_cast<size_t>(layers), 1),
      lane0(static_cast<size_t>(layers), 0), label_room(static_cast<size_t>(layers), 0);
  for (const Node& nd : g.nodes)
    if (!nd.dummy) T[size_t(nd.layer)] = std::max(T[size_t(nd.layer)], nd.vlen);
  for (int L = 0; L + 1 < layers; L++) {
    // Lanes: one per (source, direction) whose edges bend. A stem must not
    // run along another source's drop, so a source whose stem stands where
    // another's edge comes down takes the lane above it.
    std::map<std::pair<int, bool>, std::vector<size_t>> keys;
    for (size_t k = 0; k < E; k++)
      if (g.nodes[size_t(g.edges[k].from)].layer == L && x1[k] != x2[k])
        keys[{g.edges[k].from, g.edges[k].rev}].push_back(k);
    std::vector<std::pair<int, bool>> ks;
    for (const auto& kv : keys) ks.push_back(kv.first);
    std::sort(ks.begin(), ks.end(), [&](const auto& p, const auto& q) {
      return g.nodes[size_t(p.first)].u < g.nodes[size_t(q.first)].u;
    });
    const size_t K = ks.size();
    std::vector<std::vector<size_t>> before(K);  // before[q]: keys that must come above q
    for (size_t p = 0; p < K; p++)
      for (size_t q = 0; q < K; q++) {
        if (p == q) continue;
        const int stem = x1[keys[ks[p]].front()];
        for (size_t k : keys[ks[q]])
          if (x2[k] == stem) before[q].push_back(p);
      }
    std::vector<int> placed(K, -1);
    int next = 0;
    for (size_t round = 0; round < K; round++) {
      // The first not yet placed whose predecessors are; failing that (a
      // cycle), the first not yet placed.
      size_t pick = K;
      for (size_t q = 0; q < K && pick == K; q++) {
        if (placed[q] >= 0) continue;
        bool ready = true;
        for (size_t p : before[q]) ready = ready && placed[p] >= 0;
        if (ready) pick = q;
      }
      if (pick == K)
        for (size_t q = 0; q < K && pick == K; q++)
          if (placed[q] < 0) pick = q;
      placed[pick] = next++;
      for (size_t k : keys[ks[pick]]) lane[k] = placed[pick];
    }
    int room = 0;
    for (size_t k = 0; k < E; k++) {
      const Edge& e = g.edges[k];
      if (e.label.empty() || g.nodes[size_t(e.to)].layer != L + 1) continue;
      room = lr ? std::max(room, text::str_width(e.label) + 2) : 1;
    }
    label_room[size_t(L)] = room;
    const int stem = lr ? 2 : 1, arrow = lr ? 2 : 1;
    lane0[size_t(L)] = V[size_t(L)] + T[size_t(L)] + stem;
    V[size_t(L + 1)] = lane0[size_t(L)] + int(K) + room + arrow;
  }
  const int Vend = V[size_t(layers - 1)] + T[size_t(layers - 1)] + (g.clusters.empty() ? 0 : 1);
  for (Node& nd : g.nodes) {
    const int L = nd.layer;
    if (nd.dummy) {
      nd.v = V[size_t(L)];
      nd.vlen = T[size_t(L)];
    } else {
      nd.v = lr ? V[size_t(L)] + (T[size_t(L)] - nd.vlen) / 2 : V[size_t(L)];
    }
  }

  // Room to the side for self-loop notes, and (top-down) for labels beside
  // the edges at either edge of the diagram, as far as `cols` allows; blank
  // columns are trimmed off afterwards.
  int note = 0;
  for (const auto& [v, label] : selfs) note = std::max(note, 3 + text::str_width(label) + (label.empty() ? 0 : 1));
  const int core = (lr ? Vend : U) + note, H = lr ? U : Vend;
  if (core > cols || core <= 0) return false;
  int pad_l = 0, pad_r = 0;
  if (!lr) {
    int need = 0;
    for (const Edge& e : g.edges)
      if (!e.label.empty()) need = std::max(need, text::str_width(e.label) + 2);
    pad_r = std::min(need, cols - core);
    pad_l = std::min(need, cols - core - pad_r);
    for (Node& nd : g.nodes) nd.u += pad_l;
    for (size_t k = 0; k < E; k++) x1[k] += pad_l, x2[k] += pad_l;
  }
  grid.resize(core + pad_l + pad_r, H + 1);

  // In screen terms: L runs across layers, O along one.
  const auto Lline = [&](int u, int v0, int v1, uint8_t st) {
    lr ? grid.hline(v0, v1, u, st) : grid.vline(u, v0, v1, st);
  };
  const auto Oline = [&](int u0, int u1, int v, uint8_t st) {
    lr ? grid.vline(v, u0, u1, st) : grid.hline(u0, u1, v, st);
  };
  const auto sx = [&](int u, int v) { return lr ? v : u; };
  const auto sy = [&](int u, int v) { return lr ? u : v; };

  for (const Node& nd : g.nodes)
    if (!nd.dummy) grid.box(sx(nd.u, nd.v), sy(nd.u, nd.v), lr ? nd.vlen : nd.ulen, lr ? nd.ulen : nd.vlen, nd.shape, nd.lines);
  std::vector<Arrow> arrows;
  for (size_t k = 0; k < E; k++) {
    const Edge& e = g.edges[k];
    const Node &a = g.nodes[size_t(e.from)], &b = g.nodes[size_t(e.to)];
    const int s0 = a.v + a.vlen, ar = b.v - 1;
    if (x1[k] == x2[k]) {
      Lline(x1[k], s0, ar, e.style);
    } else {
      const int vl = lane0[size_t(a.layer)] + lane[k];
      Lline(x1[k], s0, vl, e.style);
      Oline(x1[k], x2[k], vl, e.style);
      Lline(x2[k], vl, ar, e.style);
    }
    if (e.head_to && !b.dummy) arrows.push_back({x2[k], ar, true});
    if (e.head_from && !a.dummy) arrows.push_back({x1[k], s0, false});
  }
  for (size_t k = 0; k < E; k++) {
    const Node& b = g.nodes[size_t(g.edges[k].to)];
    if (b.dummy) Lline(b.u, b.v - 1, b.v + b.vlen, g.edges[k].style);
  }
  grid.finish();
  for (const Arrow& a : arrows)
    grid.put(sx(a.u, a.v), sy(a.u, a.v), lr ? (a.forward ? U'▶' : U'◀') : (a.forward ? U'▼' : U'▲'), kInkLine);

  // Edge labels: top-down, beside the edge just above its arrow, or set into
  // its own stretch of lane; left-right, set into the line before the arrow.
  for (size_t k = 0; k < E; k++) {
    const Edge& e = g.edges[k];
    if (e.label.empty()) continue;
    const Node& b = g.nodes[size_t(e.to)];
    const int w = text::str_width(e.label), ar = b.v - 1;
    const auto fits = [&](int x, int y, int n, bool on_line) {
      for (int i = 0; i < n; i++)
        if (on_line ? !grid.bare_hline(x + i, y) : !grid.blank(x + i, y)) return false;
      return true;
    };
    if (lr) {
      const int x = ar - 1 - (w + 2), y = x2[k];
      if (fits(x, y, w + 2, true)) grid.text(x, y, " " + e.label + " ", kInkText);
      continue;
    }
    const int y = ar - 1, x = x2[k];
    const auto beside = [&] {
      if (fits(x + 1, y, w + 2, false)) return grid.text(x + 2, y, e.label, kInkText), true;
      if (fits(x - w - 2, y, w + 2, false)) return grid.text(x - w - 1, y, e.label, kInkText), true;
      return false;
    };
    // Into the lane: only the stretch no other edge of the same source shares.
    const auto in_lane = [&] {
      if (x1[k] == x2[k]) return false;
      const int vl = lane0[size_t(g.nodes[size_t(e.from)].layer)] + lane[k];
      const bool right = x2[k] > x1[k];
      int lo = x1[k];
      for (size_t j = 0; j < E; j++) {
        if (j == k || lane[j] != lane[k] || g.edges[j].from != e.from || g.edges[j].rev != e.rev) continue;
        if (right ? x2[j] > x2[k] : x2[j] < x2[k]) return false;
        if (right ? x2[j] > lo : x2[j] < lo) lo = x2[j];
      }
      const int a0 = std::min(lo, x2[k]) + 1, a1 = std::max(lo, x2[k]) - 1, span = a1 - a0 + 1;
      if (span < w + 2) return false;
      const int at = a0 + (span - (w + 2)) / 2;
      if (!fits(at, vl, w + 2, true)) return false;
      grid.text(at, vl, " " + e.label + " ", kInkText);
      return true;
    };
    // Beside the drop, unless other edges come down it too: then it would
    // not say which of them it names.
    bool shared = false;
    for (size_t j = 0; j < E && !shared; j++)
      shared = j != k && g.edges[j].to == e.to && x2[j] == x2[k];
    if (shared ? !in_lane() && !beside() : !beside() && !in_lane()) continue;
  }
  for (const auto& [v, label] : selfs) {
    const Node& nd = g.nodes[size_t(v)];
    const int x = sx(nd.u, nd.v) + (lr ? nd.vlen : nd.ulen) + 1;
    const int y = sy(nd.u, nd.v) + (lr ? nd.ulen : nd.vlen) / 2;
    const std::string t = label.empty() ? "\xE2\x86\xBA" : "\xE2\x86\xBA " + label;  // ↺
    bool free = true;
    for (int i = 0; i < text::str_width(t) && free; i++) free = grid.blank(x + i, y);
    if (free) grid.text(x, y, t, kInkLine);
  }

  // Subgraph boxes, dashed, on the cells the drawing left blank: edges that
  // leave a subgraph pass through its border, and a label may sit on it. The title sits in the top edge.
  for (int c = 0; c < int(g.clusters.size()); c++) {
    int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
    for (const Node& nd : g.nodes) {
      if (nd.cluster != c || nd.dummy) continue;
      u0 = std::min(u0, nd.u - margin);
      u1 = std::max(u1, nd.u + nd.ulen - 1 + margin);
      v0 = std::min(v0, nd.v - 1);
      v1 = std::max(v1, nd.v + nd.vlen);
    }
    if (u0 == INT32_MAX) continue;
    const int x0 = sx(u0, v0), y0 = sy(u0, v0), x1b = sx(u1, v1), y1b = sy(u1, v1);
    const auto mark = [&](int x, int y, char32_t ch) {
      if (grid.blank(x, y)) grid.put(x, y, ch, kInkLine);
    };
    for (int x = x0 + 1; x < x1b; x++) mark(x, y0, U'╌'), mark(x, y1b, U'╌');
    for (int y = y0 + 1; y < y1b; y++) mark(x0, y, U'╎'), mark(x1b, y, U'╎');
    mark(x0, y0, U'┌');
    mark(x1b, y0, U'┐');
    mark(x0, y1b, U'└');
    mark(x1b, y1b, U'┘');
    // The title in the first stretch of the top edge no edge crosses that
    // holds it, else of the bottom edge; failing both, shortened into the
    // top edge's longest stretch.
    const std::string& title = g.clusters[size_t(c)];
    const int tw = text::str_width(title);
    const auto stretch = [&](int y, int* best_len) {
      int best_at = -1;
      *best_len = 0;
      for (int at = x0 + 1; at < x1b;) {
        if (grid.cp[grid.at(at, y)] != U'╌') { at++; continue; }
        int len = 0;
        while (at + len < x1b && grid.cp[grid.at(at + len, y)] == U'╌') len++;
        if (len >= tw + 2) { *best_len = len; return at; }
        if (len > *best_len) best_at = at, *best_len = len;
        at += len;
      }
      return best_at;
    };
    int top_len = 0, bottom_len = 0;
    const int top_at = stretch(y0, &top_len), bottom_at = stretch(y1b, &bottom_len);
    if (title.empty()) continue;
    if (top_at >= 0 && top_len >= tw + 2) grid.text(top_at, y0, " " + title + " ", kInkTitle);
    else if (bottom_at >= 0 && bottom_len >= tw + 2) grid.text(bottom_at, y1b, " " + title + " ", kInkTitle);
    else if (top_at >= 0 && top_len >= 6) grid.text(top_at, y0, " " + text::ellipsize(title, top_len - 2) + " ", kInkTitle);
  }
  return true;
}

// --- sequence diagrams --------------------------------------------------

bool draw_sequence(const std::vector<std::string_view>& lines, int cols, Grid& grid) {
  struct Msg {
    int a = -1, b = -1;
    std::string text;
    bool dashed = false, note = false, block = false;
  };
  std::vector<std::string> names, labels;
  const auto who = [&](std::string_view id) {
    id = trim(id);
    for (size_t i = 0; i < names.size(); i++)
      if (names[i] == id) return int(i);
    names.emplace_back(id);
    labels.emplace_back(id);
    return int(names.size()) - 1;
  };
  std::vector<Msg> msgs;
  for (size_t li = 1; li < lines.size(); li++) {
    const std::string_view l = lines[li];
    if (l.starts_with("participant ") || l.starts_with("actor ")) {
      std::string_view rest = trim(l.substr(l.find(' ') + 1));
      const size_t as = rest.find(" as ");
      const int i = who(as == std::string_view::npos ? rest : rest.substr(0, as));
      if (as != std::string_view::npos) labels[size_t(i)] = clean(rest.substr(as + 4));
      continue;
    }
    if (l.starts_with("autonumber") || l.starts_with("activate") || l.starts_with("deactivate") ||
        l.starts_with("title") || l.starts_with("box") || l.starts_with("create ") || l.starts_with("destroy "))
      continue;
    bool block = false;
    for (std::string_view kw : {"loop", "alt", "else", "opt", "par", "and", "critical", "option", "break", "rect", "end"}) {
      if (l == kw || (l.starts_with(kw) && l.size() > kw.size() && l[kw.size()] == ' ')) {
        Msg m;
        m.block = true;
        if (l != "end" && kw != "rect") m.text = std::string(l);
        if (kw != "rect") msgs.push_back(m);
        block = true;
        break;
      }
    }
    if (block) continue;
    if (l.starts_with("Note ") || l.starts_with("note ")) {
      const size_t colon = l.find(':');
      if (colon == std::string_view::npos) continue;
      std::string_view where = trim(l.substr(5, colon - 5));
      for (std::string_view p : {"over ", "left of ", "right of "})
        if (where.starts_with(p)) where.remove_prefix(p.size());
      Msg m;
      m.note = true;
      const size_t comma = where.find(',');
      m.a = who(comma == std::string_view::npos ? where : where.substr(0, comma));
      m.b = comma == std::string_view::npos ? m.a : who(where.substr(comma + 1));
      m.text = clean(l.substr(colon + 1));
      msgs.push_back(m);
      continue;
    }
    // A->>B: text, A-->>B, A->B, A-xB, A-)B
    static constexpr std::string_view kOps[] = {"-->>", "->>", "-->", "->", "--x", "-x", "--)", "-)"};
    for (std::string_view op : kOps) {
      const size_t k = l.find(op);
      if (k == std::string_view::npos || k == 0) continue;
      const size_t colon = l.find(':', k + op.size());
      Msg m;
      m.a = who(l.substr(0, k));
      std::string_view to = l.substr(k + op.size(), (colon == std::string_view::npos ? l.size() : colon) - k - op.size());
      while (!to.empty() && (to.front() == '+' || to.front() == '-')) to.remove_prefix(1);  // activation marks
      m.b = who(to);
      m.dashed = op.starts_with("--");
      m.text = colon == std::string_view::npos ? std::string() : clean(l.substr(colon + 1));
      msgs.push_back(m);
      break;
    }
  }
  const int n = int(names.size());
  if (n == 0 || n > 12) return false;
  // Columns far enough apart for each message's text between them.
  std::vector<int> gap(static_cast<size_t>(std::max(0, n - 1)), 6);
  for (const Msg& m : msgs) {
    if (m.block || m.a == m.b || m.a < 0) continue;
    const int lo = std::min(m.a, m.b), hi = std::max(m.a, m.b);
    const int need = text::str_width(m.text) + 4;
    int have = 0;
    for (int i = lo; i < hi; i++) have += gap[size_t(i)];
    if (have < need)
      for (int i = lo; i < hi; i++) gap[size_t(i)] += (need - have + (hi - lo) - 1) / (hi - lo);
  }
  const auto half = [&](int i) { return (text::str_width(labels[size_t(i)]) + 4) / 2; };
  std::vector<int> x(static_cast<size_t>(n), 0);
  x[0] = half(0);
  for (int i = 1; i < n; i++) x[size_t(i)] = x[size_t(i - 1)] + std::max(gap[size_t(i - 1)], half(i) + half(i - 1) + 2);
  int W = x[size_t(n - 1)] + half(n - 1) + 1;
  for (const Msg& m : msgs) {
    if (m.a == m.b && m.a >= 0 && !m.note) W = std::max(W, x[size_t(m.a)] + 4 + text::str_width(m.text));
    if (m.block) W = std::max(W, text::str_width(m.text) + 4);
  }
  if (W > cols) return false;
  int H = 3;
  for (const Msg& m : msgs) H += m.block || m.note ? 1 : 2;
  grid.resize(W, H + 1);
  for (int i = 0; i < n; i++) {
    const int bw = text::str_width(labels[size_t(i)]) + 4;
    grid.box(x[size_t(i)] - bw / 2, 0, bw, 3, '[', {labels[size_t(i)]});
  }
  int y = 3;
  for (const Msg& m : msgs) {
    if (m.block) {
      // A loop, alt… opens with a dotted rule carrying its text; end closes it.
      for (int i = 0; i < W; i++) grid.put(i, y, U'┄', kInkLine);
      if (!m.text.empty()) grid.text(1, y, " " + m.text + " ", kInkTitle);
      y += 1;
      continue;
    }
    if (m.note) {
      const int cx = (x[size_t(m.a)] + x[size_t(m.b)]) / 2;
      const std::string t = "\xE2\x9F\xA6 " + m.text + " \xE2\x9F\xA7";  // ⟦ ⟧
      grid.text(std::clamp(cx - text::str_width(t) / 2, 0, std::max(0, W - text::str_width(t))), y, t, kInkTitle);
      y += 1;
      continue;
    }
    const int xa = x[size_t(m.a)], xb = x[size_t(m.b)];
    if (m.a == m.b) {
      grid.text(xa + 2, y, "\xE2\x86\xBA " + m.text, kInkText);  // ↺
      y += 2;
      continue;
    }
    const int lo = std::min(xa, xb), hi = std::max(xa, xb);
    grid.text(lo + (hi - lo + 1 - text::str_width(m.text)) / 2, y, m.text, kInkText);
    for (int i = lo + 1; i < hi; i++) grid.put(i, y + 1, m.dashed ? U'┄' : U'─', kInkLine);
    grid.put(xb > xa ? xb - 1 : xb + 1, y + 1, xb > xa ? U'▶' : U'◀', kInkLine);
    y += 2;
  }
  // Lifelines under every participant, behind the messages that cross them.
  for (int i = 0; i < n; i++)
    for (int r = 3; r < H; r++)
      if (grid.cp[grid.at(x[size_t(i)], r)] == U' ') grid.put(x[size_t(i)], r, U'│', kInkLine);
  return true;
}

}  // namespace

bool draw(std::string_view src, int cols, chart::Figure& out) {
  const std::vector<std::string_view> lines = lines_of(src);
  if (lines.empty() || cols < 8) return false;
  const std::string_view head = lines.front();
  Grid grid;
  if (head.starts_with("graph") || head.starts_with("flowchart") || head.starts_with("stateDiagram")) {
    Graph g;
    const bool state = head.starts_with("stateDiagram");
    if (!parse_flow(lines, g, state)) return false;
    // As written, then packed tight, then the other way round.
    bool ok = layout_flow(g, cols, false, grid) || layout_flow(g, cols, true, grid);
    if (!ok) {
      g.lr = !g.lr;
      ok = layout_flow(g, cols, false, grid) || layout_flow(g, cols, true, grid);
    }
    if (!ok) return false;
    grid.to_figure(out);
    return true;
  }
  if (head.starts_with("sequenceDiagram")) {
    if (!draw_sequence(lines, cols, grid)) return false;
    grid.to_figure(out);
    return true;
  }
  if (head.starts_with("pie")) {
    // A pie is drawn as the bar chart it is easier to read as.
    chart::Spec spec;
    spec.kind = chart::Spec::Kind::Bar;
    const size_t t = head.find("title");
    if (t != std::string_view::npos) spec.title = clean(head.substr(t + 5));
    chart::Series ser;
    for (size_t i = 1; i < lines.size(); i++) {
      const std::string_view l = lines[i];
      if (l.starts_with("title")) { spec.title = clean(l.substr(5)); continue; }
      const size_t colon = l.rfind(':');
      if (colon == std::string_view::npos) continue;
      spec.labels.push_back(clean(l.substr(0, colon)));
      ser.y.push_back(std::atof(std::string(trim(l.substr(colon + 1))).c_str()));
    }
    if (ser.y.empty()) return false;
    spec.series.push_back(ser);
    chart::figure(spec, src, cols, out);
    return true;
  }
  return false;
}

}  // namespace mico::diagram
