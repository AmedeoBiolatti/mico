#include "views/notebook.h"

#include <algorithm>
#include <vector>

#include "base/json.h"

namespace mico::notebook {
namespace {

constexpr size_t kMaxTableRows = 60;

std::string unesc(const js::Value& v) {
  std::string out;
  if (v.is_string()) js::unescape_append(v.body(), out);
  return out;
}

// A notebook string field: one string, or an array of lines to join.
std::string joined(const js::Value& v) {
  if (v.is_string()) return unesc(v);
  std::string out;
  if (v.is_array())
    js::scan_array(v.raw, [&](const js::Value& p) {
      out += unesc(p);
      return true;
    });
  return out;
}

js::Value member(const js::Value& obj, std::string_view key) {
  js::Value found{};
  if (obj.is_object())
    js::scan_object(obj.raw, [&](std::string_view k, const js::Value& v) {
      if (k != key) return true;
      found = v;
      return false;
    });
  return found;
}

std::string strip_ansi(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      i += 2;
      while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) i++;
      continue;
    }
    out += s[i];
  }
  return out;
}

std::string trim_newlines(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  size_t a = 0;
  while (a < s.size() && s[a] == '\n') a++;
  return s.substr(a);
}

// A fenced block holding `body`; a line of it that would read as a fence is
// kept from closing the block by a zero-width space.
void fenced(std::string& md, std::string_view lang, std::string_view body) {
  md += "```";
  md += lang;
  md += '\n';
  size_t pos = 0;
  while (pos <= body.size()) {
    size_t nl = body.find('\n', pos);
    if (nl == std::string_view::npos) nl = body.size();
    std::string_view line = body.substr(pos, nl - pos);
    const size_t lead = line.find_first_not_of(' ');
    if (lead != std::string_view::npos && (line.substr(lead).starts_with("```") || line.substr(lead).starts_with("~~~")))
      md += "\xE2\x80\x8B";
    md += line;
    md += '\n';
    pos = nl + 1;
  }
  md += "```\n\n";
}

// pandas' text for a DataFrame, back into a table: columns are where a gutter
// of two or more spaces runs down every line, the header's index cell is
// blank and every row has an index. A wrapped frame (lines ending in "\\")
// or a Series (its "dtype:" line) is left as text.
bool frame_table(std::string_view text, std::string& md) {
  std::vector<std::string> lines;
  std::string footer;
  for (size_t pos = 0; pos <= text.size();) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string l(text.substr(pos, nl - pos));
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
    pos = nl + 1;
    if (l.empty()) continue;
    if (l.front() == '[' && l.find(" rows x ") != std::string::npos) { footer = l; continue; }
    if (l.back() == '\\' || l.find("dtype:") != std::string::npos || l.find('\t') != std::string::npos) return false;
    lines.push_back(std::move(l));
  }
  if (lines.size() < 3 || lines.size() > 400) return false;
  size_t width = 0;
  for (const auto& l : lines) width = std::max(width, l.size());
  if (width > 400) return false;
  // Gutters: columns blank on every line, in runs of two or more (or the
  // run that starts the header, under the index).
  std::vector<bool> blank(width, true);
  for (const auto& l : lines)
    for (size_t i = 0; i < l.size(); i++)
      if (l[i] != ' ') blank[i] = false;
  std::vector<std::pair<size_t, size_t>> fields;  // [begin, end)
  for (size_t i = 0; i < width;) {
    if (blank[i]) {
      size_t j = i;
      while (j < width && blank[j]) j++;
      if (j - i == 1 && !fields.empty()) fields.back().second = j;  // one space: part of the field
      i = j;
      continue;
    }
    size_t j = i;
    while (j < width && !blank[j]) j++;
    if (!fields.empty() && fields.back().second == i) fields.back().second = j;
    else fields.emplace_back(i, j);
    i = j;
  }
  if (fields.size() < 2 || fields.size() > 40) return false;
  const auto cells = [&](const std::string& l) {
    std::vector<std::string> out;
    for (const auto& [a, b] : fields) {
      std::string c = a < l.size() ? l.substr(a, std::min(b, l.size()) - a) : std::string();
      const size_t x = c.find_first_not_of(' ');
      c = x == std::string::npos ? std::string() : c.substr(x, c.find_last_not_of(' ') - x + 1);
      for (char& ch : c)
        if (ch == '|') ch = '/';
      out.push_back(std::move(c));
    }
    return out;
  };
  std::vector<std::vector<std::string>> rows;
  for (const auto& l : lines) rows.push_back(cells(l));
  if (!rows[0][0].empty()) return false;  // pandas leaves the index's header blank
  // An index name has a row of its own under the header.
  if (!rows[1][0].empty() && std::all_of(rows[1].begin() + 1, rows[1].end(), [](const std::string& c) { return c.empty(); })) {
    rows[0][0] = rows[1][0];
    rows.erase(rows.begin() + 1);
  }
  if (rows.size() < 3) return false;
  for (size_t i = 1; i < rows.size(); i++) {
    if (rows[i][0].empty()) return false;
    int filled = 0;
    for (const auto& c : rows[i]) filled += !c.empty();
    if (filled < 2) return false;
  }
  const auto line = [&](const std::vector<std::string>& r) {
    md += '|';
    for (const auto& c : r) md += " " + c + " |";
    md += '\n';
  };
  line(rows[0]);
  md += '|';
  for (size_t k = 0; k < fields.size(); k++) md += "---|";
  md += '\n';
  for (size_t i = 1; i < rows.size(); i++) line(rows[i]);
  md += '\n';
  if (!footer.empty()) md += "*" + footer + "*\n\n";
  return true;
}

void output_text(std::string& md, std::string_view text) {
  const std::string t = trim_newlines(strip_ansi(text));
  if (t.empty()) return;
  if (!frame_table(t, md)) fenced(md, "out", t);
}

void image(std::string& md, std::string_view media, std::string_view b64) {
  std::string clean;
  clean.reserve(b64.size());
  for (char c : b64)
    if (c != '\n' && c != '\r' && c != ' ' && c != '\\') clean += c;
  if (clean.empty()) return;
  md += "![output](data:";
  md += media;
  md += ";base64,";
  md += clean;
  md += ")\n\n";
}

// --- HTML tables (a DataFrame's text/html) to markdown -------------------

std::string html_text(std::string_view s) {
  std::string out;
  bool space = false;
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '<') {
      const size_t e = s.find('>', i);
      if (e == std::string_view::npos) break;
      i = e;
      space = true;
      continue;
    }
    if (c == '&') {
      static constexpr std::pair<std::string_view, std::string_view> kEnt[] = {
          {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}};
      bool hit = false;
      for (const auto& [ent, rep] : kEnt)
        if (s.compare(i, ent.size(), ent) == 0) {
          if (space && !out.empty()) out += ' ';
          space = false;
          out += rep;
          i += ent.size() - 1;
          hit = true;
          break;
        }
      if (hit) continue;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c == '|' ? '/' : c;
  }
  return out;
}

bool html_table(std::string_view html, std::string& md) {
  const size_t t0 = html.find("<table");
  if (t0 == std::string_view::npos) return false;
  const size_t t1 = html.find("</table>", t0);
  html = html.substr(t0, t1 == std::string_view::npos ? std::string_view::npos : t1 - t0);
  std::vector<std::vector<std::string>> rows;
  for (size_t pos = 0;;) {
    const size_t r0 = html.find("<tr", pos);
    if (r0 == std::string_view::npos) break;
    size_t r1 = html.find("</tr>", r0);
    if (r1 == std::string_view::npos) r1 = html.size();
    const std::string_view row = html.substr(r0, r1 - r0);
    std::vector<std::string> cells;
    for (size_t c = 0;;) {
      const size_t th = row.find("<th", c), td = row.find("<td", c);
      const size_t a = std::min(th, td);
      if (a == std::string_view::npos) break;
      const size_t open_end = row.find('>', a);
      if (open_end == std::string_view::npos) break;
      const bool head = a == th;
      size_t b = row.find(head ? "</th>" : "</td>", open_end);
      if (b == std::string_view::npos) b = row.size();
      cells.push_back(html_text(row.substr(open_end + 1, b - open_end - 1)));
      c = b;
    }
    if (!cells.empty()) rows.push_back(std::move(cells));
    pos = r1;
  }
  if (rows.size() < 2) return false;
  // pandas writes an index-name row under the header; fold it into the header.
  if (rows.size() > 2 && std::all_of(rows[1].begin() + 1, rows[1].end(), [](const std::string& c) { return c.empty(); }) &&
      !rows[1][0].empty()) {
    rows[0][0] = rows[1][0];
    rows.erase(rows.begin() + 1);
  }
  size_t ncols = 0;
  for (const auto& r : rows) ncols = std::max(ncols, r.size());
  const auto line = [&](const std::vector<std::string>& r) {
    md += '|';
    for (size_t k = 0; k < ncols; k++) {
      md += ' ';
      if (k < r.size()) md += r[k];
      md += " |";
    }
    md += '\n';
  };
  line(rows[0]);
  md += '|';
  for (size_t k = 0; k < ncols; k++) md += "---|";
  md += '\n';
  for (size_t i = 1; i < rows.size() && i <= kMaxTableRows; i++) line(rows[i]);
  if (rows.size() - 1 > kMaxTableRows) md += "\n\xE2\x80\xA6 " + std::to_string(rows.size() - 1 - kMaxTableRows) + " more rows\n";
  md += '\n';
  return true;
}

void code_cell(std::string& md, std::string_view lang, std::string_view source) {
  const std::string src = trim_newlines(std::string(source));
  if (!src.empty()) fenced(md, lang.empty() ? "python" : lang, src);
}

void markdown_cell(std::string& md, std::string_view source) {
  const std::string src = trim_newlines(std::string(source));
  if (src.empty()) return;
  md += src;
  md += "\n\n";
}

// Claude's text for its reading: cells, each followed by its outputs' text.
void claude_text(std::string& md, std::string_view s) {
  size_t pos = 0;
  while (pos < s.size()) {
    const size_t open = s.find("<cell id=\"", pos);
    output_text(md, s.substr(pos, open == std::string_view::npos ? std::string_view::npos : open - pos));
    if (open == std::string_view::npos) return;
    const size_t id_end = s.find("\">", open + 10);
    if (id_end == std::string_view::npos) return;
    const std::string close = "</cell id=\"" + std::string(s.substr(open + 10, id_end - open - 10)) + "\">";
    const size_t body_at = id_end + 2;
    size_t end = s.find(close, body_at);
    if (end == std::string_view::npos) end = s.size();
    std::string_view body = s.substr(body_at, end - body_at);
    std::string type = "code", lang;
    const auto tag = [&](std::string_view name, std::string& into) {
      const std::string o = "<" + std::string(name) + ">", c = "</" + std::string(name) + ">";
      if (!body.starts_with(o)) return;
      const size_t e = body.find(c);
      if (e == std::string_view::npos) return;
      into = std::string(body.substr(o.size(), e - o.size()));
      body.remove_prefix(e + c.size());
    };
    tag("cell_type", type);
    tag("language", lang);
    if (type == "markdown") markdown_cell(md, body);
    else if (type == "code") code_cell(md, lang, body);
    else fenced(md, "", trim_newlines(std::string(body)));
    pos = end == s.size() ? end : end + close.size();
  }
}

}  // namespace

bool is_claude_read(std::string_view text) {
  return text.find("<cell id=\"") != std::string_view::npos && text.find("</cell id=\"") != std::string_view::npos;
}

bool from_claude(std::string_view raw_line, std::string& md) {
  md.clear();
  while (!raw_line.empty() && (raw_line.back() == '\n' || raw_line.back() == '\r')) raw_line.remove_suffix(1);
  if (raw_line.empty() || raw_line[0] != '{') return false;
  bool found = false;
  // message.content[] → the tool_result whose blocks hold cells.
  const js::Value root{raw_line, js::Type::Object};
  const js::Value content = member(member(root, "message"), "content");
  if (!content.is_array()) return false;
  js::scan_array(content.raw, [&](const js::Value& part) {
    const js::Value inner = member(part, "content");
    if (!inner.is_array() || inner.raw.find("<cell id=") == std::string_view::npos) return true;
    // Text blocks are joined, as the transcript reader joins them; an image
    // ends the text before it and is drawn there.
    std::string text;
    js::scan_array(inner.raw, [&](const js::Value& b) {
      const std::string type = unesc(member(b, "type"));
      if (type == "text") {
        if (!text.empty()) text += '\n';
        text += unesc(member(b, "text"));
      } else if (type == "image") {
        claude_text(md, text);
        text.clear();
        const js::Value src = member(b, "source");
        const js::Value data = member(src, "data");
        if (data.is_string()) image(md, unesc(member(src, "media_type")), data.body());
      }
      return true;
    });
    claude_text(md, text);
    found = true;
    return false;
  });
  return found && !md.empty();
}

bool from_ipynb(std::string_view json, std::string& md) {
  md.clear();
  size_t a = 0;
  while (a < json.size() && (json[a] == ' ' || json[a] == '\n' || json[a] == '\r' || json[a] == '\t')) a++;
  if (a >= json.size() || json[a] != '{') return false;
  const js::Value root{json.substr(a), js::Type::Object};
  const js::Value cells = member(root, "cells");
  if (!cells.is_array() || member(root, "nbformat").type == js::Type::Null) return false;
  const js::Value meta = member(root, "metadata");
  std::string lang = unesc(member(member(meta, "language_info"), "name"));
  if (lang.empty()) lang = unesc(member(member(meta, "kernelspec"), "language"));
  js::scan_array(cells.raw, [&](const js::Value& cell) {
    const std::string type = unesc(member(cell, "cell_type"));
    const std::string source = joined(member(cell, "source"));
    if (type == "markdown") markdown_cell(md, source);
    else if (type == "code") code_cell(md, lang, source);
    else fenced(md, "", trim_newlines(source));
    const js::Value outputs = member(cell, "outputs");
    if (!outputs.is_array()) return true;
    js::scan_array(outputs.raw, [&](const js::Value& o) {
      const std::string kind = unesc(member(o, "output_type"));
      if (kind == "stream") {
        output_text(md, joined(member(o, "text")));
      } else if (kind == "error") {
        std::string tb;
        js::scan_array(member(o, "traceback").raw, [&](const js::Value& l) {
          tb += unesc(l) + "\n";
          return true;
        });
        output_text(md, tb.empty() ? unesc(member(o, "ename")) + ": " + unesc(member(o, "evalue")) : tb);
      } else {
        const js::Value data = member(o, "data");
        bool shown = false;
        for (std::string_view mt : {"image/png", "image/jpeg"}) {
          const js::Value img = member(data, mt);
          if (img.type == js::Type::Null || shown) continue;
          image(md, mt, joined(img));
          shown = true;
        }
        if (!shown) {
          const std::string html = joined(member(data, "text/html"));
          if (!html.empty() && html_table(html, md)) shown = true;
        }
        if (!shown) output_text(md, joined(member(data, "text/plain")));
      }
      return true;
    });
    return true;
  });
  return !md.empty();
}

}  // namespace mico::notebook
