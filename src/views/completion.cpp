#include "views/completion.h"

#include <algorithm>

#include "base/text.h"

namespace mico {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// The last path component, a folder's without its trailing slash.
std::string_view base_name(std::string_view p) {
  std::string_view s = p;
  if (s.ends_with('/')) s.remove_suffix(1);
  const size_t slash = s.rfind('/');
  return slash == std::string_view::npos ? s : s.substr(slash + 1);
}

}  // namespace

Trigger find_trigger(std::string_view text, size_t cursor) {
  Trigger t;
  if (cursor > text.size()) return t;
  size_t from = cursor;
  while (from > 0 && !space(text[from - 1])) from--;
  if (from >= text.size()) return t;
  const char c = text[from];
  // A command is the message's first word; a mention can be any word.
  if (!(c == '@' || (c == '/' && from == 0))) return t;
  size_t end = cursor;
  while (end < text.size() && !space(text[end])) end++;
  t.kind = c;
  t.from = from;
  t.end = end;
  t.query.assign(text.substr(from + 1, cursor - from - 1));
  return t;
}

std::vector<int> rank_paths(const std::vector<std::string>& paths, std::string_view query, size_t max) {
  std::vector<std::pair<int, int>> hits;
  if (query.empty()) {
    for (size_t i = 0; i < paths.size(); i++) {
      std::string_view p = paths[i];
      const size_t slash = p.find('/');
      if (slash == std::string_view::npos || slash + 1 == p.size())
        hits.push_back({p.ends_with('/') ? 1 : 0, int(i)});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  } else {
    // A query naming folders is matched against the whole path; otherwise
    // the name is what a person means, and the path only backs it up.
    const bool pathy = query.find('/') != std::string_view::npos;
    for (size_t i = 0; i < paths.size(); i++) {
      const std::string& p = paths[i];
      int s = fuzzy_match(p, query);
      if (s < 0) continue;
      if (!pathy) {
        const int b = fuzzy_match(base_name(p), query);
        if (b >= 0) s = std::max(s, b + 600);
      }
      // Shallower and shorter first among equals.
      s -= int(std::count(p.begin(), p.end(), '/')) * 4 + int(std::min<size_t>(p.size(), 200)) / 16;
      hits.push_back({s, int(i)});
    }
    const size_t keep = std::min(max, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + ptrdiff_t(keep), hits.end(),
                      [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    hits.resize(keep);
  }
  std::vector<int> out;
  out.reserve(std::min(max, hits.size()));
  for (size_t i = 0; i < hits.size() && out.size() < max; i++) out.push_back(hits[i].second);
  return out;
}

Completion::Completion() {
  Picker::Options o;
  o.frame = Picker::Frame::Popup;
  o.filter = false;  // the prompt box is the query field
  pick_ = Picker(std::move(o));
}

void Completion::set_commands(const std::vector<SlashCommand>* cmds, uint64_t version) {
  cmds_ = cmds;
  cmds_version_ = version;
}

void Completion::set_files(const std::vector<std::string>* paths, uint64_t version) {
  files_ = paths;
  files_version_ = version;
}

void Completion::update(std::string_view text, size_t cursor, bool allowed) {
  trig_ = allowed ? find_trigger(text, cursor) : Trigger{};
  const std::string key = trig_.kind ? std::string(1, trig_.kind) + std::to_string(trig_.from) : std::string();
  if (key != dismissed_) dismissed_.clear();
  if (!trig_.kind) {
    built_kind_ = 0;
    return;
  }
  rebuild();
}

void Completion::rebuild() {
  const uint64_t version = trig_.kind == '/' ? cmds_version_ : files_version_;
  const bool fresh = built_kind_ != trig_.kind || built_version_ != version;
  Picker::Options& o = pick_.options();
  if (trig_.kind == '/') {
    if (fresh) {
      o.title = "Commands";
      o.ranked = true;
      // Names only, as the agents' own menus match: descriptions mention
      // other commands and links, and a two-letter query finds them all.
      o.match_detail = false;
      o.footer_text = "tab complete \xC2\xB7 enter run \xC2\xB7 esc close";
      std::vector<PickItem> items;
      if (cmds_) {
        items.reserve(cmds_->size());
        for (const SlashCommand& c : *cmds_) {
          PickItem it;
          it.label = "/" + c.name;
          it.detail = c.description;
          it.hint = c.hint;
          it.id = c.name;
          items.push_back(std::move(it));
        }
      }
      pick_.set_items(std::move(items));
    }
    // The label carries the slash, so the query does too: "/co" is matched
    // as typed, a run at the start of "/compact".
    pick_.set_query(trig_.query.empty() ? std::string() : "/" + trig_.query);
  } else {
    if (fresh || built_query_ != trig_.query) {
      o.title = "Files";
      o.ranked = false;
      o.match_detail = false;
      o.footer_text = "tab/enter insert \xC2\xB7 esc close";
      std::vector<PickItem> items;
      if (files_) {
        for (int i : rank_paths(*files_, trig_.query, 200)) {
          PickItem it;
          it.label = (*files_)[size_t(i)];
          it.id = it.label;
          items.push_back(std::move(it));
        }
      }
      // New items for a new query: the cursor goes back to the best one.
      pick_.set_query({});
      pick_.set_items(std::move(items));
      pick_.set_cursor(0);
      pick_.set_query(trig_.query);
    }
  }
  built_kind_ = trig_.kind;
  built_version_ = version;
  built_query_ = trig_.query;
}

bool Completion::visible() const {
  if (!trig_.kind || !dismissed_.empty()) return false;
  return pick_.visible_count() > 0;
}

void Completion::dismiss() {
  if (trig_.kind) dismissed_ = std::string(1, trig_.kind) + std::to_string(trig_.from);
}

Completion::Take Completion::take() const {
  Take t;
  const int i = pick_.cursor();
  if (i < 0 || !trig_.kind) return t;
  const PickItem& it = pick_.items()[size_t(i)];
  if (trig_.kind == '/') {
    t.text = "/" + it.id + " ";
    // A command whose arguments are required waits for them.
    t.send = it.hint.empty() || it.hint[0] != '<';
  } else {
    t.keep_open = it.id.ends_with('/');
    t.text = "@" + it.id + (t.keep_open ? "" : " ");
  }
  return t;
}

}  // namespace mico
