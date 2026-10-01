#include "views/chat_render.h"

#include "core/settings.h"

#include <sys/stat.h>
#include <ctime>
#include "core/activity.h"
#include "core/search.h"
#include "core/images.h"
#include "math/math.h"
#include "math/picture.h"
#include "views/code.h"
#include "views/json_view.h"
#include "views/notebook.h"
#include "term/links.h"
#include "views/views.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace mico {
namespace {

// Lines parsed per backward step. Big enough that scrolling does not stutter,
// small enough that opening a 200 MB rollout stays instant.
constexpr size_t kChunkLines = 512;
// A single message can be enormous; cap what a collapsed one contributes so one
// pathological tool result cannot cost a second of layout.
constexpr size_t kMaxRowsPerEvent = 200;
constexpr size_t kCollapsedResultRows = 3;
// Ceiling on retained transcript text. Scrolling back through a 200 MB rollout
// used to keep every byte it had walked past.
constexpr size_t kArenaBudget = 32u << 20;

Style base_style(int rs, const Theme& th) {
  switch (rs) {
    case 1: return Style{th.user, th.panel};
    case 2: return Style{th.assistant, th.panel};
    case 3: return Style{th.thinking, th.panel, attr::kItalic};
    case 4: return Style{th.tool, th.panel};
    case 5: return Style{th.dim, th.panel};
    case 6: return Style{th.err, th.panel};
    case 7: return Style{th.dim, th.panel, attr::kItalic};
    case 8: return Style{th.accent, th.panel, attr::kBold};   // question header
    case 9: return Style{th.text, th.panel};                  // question text
    case 10: return Style{th.text, th.panel};                 // option label
    case 11: return Style{th.accent, th.panel, attr::kBold};  // submit row
    case 12: return Style{th.dim, th.panel, attr::kItalic};   // hint
    case 13: return Style{th.dim, th.panel};                  // work: said between steps
    case 14: return Style{th.dim, th.panel};                  // a folded turn's steps
    default: return Style{th.text, th.panel};
  }
}

// The row supplies the colour context, the segment's ink modifies it. Keeping
// them separate is why a bold run inside a user turn still reads as the user.
Style ink_style(Style base, md::Ink ink, const Theme& th) {
  switch (ink) {
    case md::Ink::Bold: base.a |= attr::kBold; break;
    case md::Ink::Italic: base.a |= attr::kItalic; break;
    case md::Ink::Code: base.fg = th.code; break;
    case md::Ink::Link: base.fg = th.link; base.a |= attr::kUnderline; break;
    case md::Ink::Heading: base.fg = th.heading; base.a |= attr::kBold; break;
    case md::Ink::Quote: base.fg = th.quote; base.a |= attr::kItalic; break;
    case md::Ink::Bullet: base.fg = th.accent; break;
    case md::Ink::Rule: base.fg = th.border; break;
    case md::Ink::Added: base.fg = th.added; break;
    case md::Ink::Removed: base.fg = th.removed; break;
    case md::Ink::Hunk: base.fg = th.hunk; base.a |= attr::kBold; break;
    case md::Ink::Math: base.fg = th.math; break;
    case md::Ink::CodeText: base.fg = th.code_text; break;
    case md::Ink::CodeKeyword: base.fg = th.code_keyword; break;
    case md::Ink::CodeString: base.fg = th.code_string; break;
    case md::Ink::CodeComment: base.fg = th.code_comment; base.a |= attr::kItalic; break;
    case md::Ink::CodeNumber: base.fg = th.code_number; break;
    case md::Ink::CodeType: base.fg = th.code_type; break;
    case md::Ink::CodeFunc: base.fg = th.code_func; break;
    case md::Ink::CodeMark: base.fg = th.code_mark; break;
    case md::Ink::Ansi0: case md::Ink::Ansi1: case md::Ink::Ansi2: case md::Ink::Ansi3:
    case md::Ink::Ansi4: case md::Ink::Ansi5: case md::Ink::Ansi6: case md::Ink::Ansi7:
    case md::Ink::Ansi8: case md::Ink::Ansi9: case md::Ink::Ansi10: case md::Ink::Ansi11:
    case md::Ink::Ansi12: case md::Ink::Ansi13: case md::Ink::Ansi14: case md::Ink::Ansi15:
      base.fg = th.ansi[int(ink) - int(md::Ink::Ansi0)];
      break;
    // Charts: dim axes, and series in colours that stay apart on the panel.
    case md::Ink::ChartAxis: base.fg = th.dim; break;
    case md::Ink::Series1: base.fg = th.accent; break;
    case md::Ink::Series2: base.fg = th.link; break;
    case md::Ink::Series3: base.fg = th.code; break;
    case md::Ink::Series4: base.fg = th.removed; break;
    case md::Ink::Series5: base.fg = th.thinking; break;
    case md::Ink::Series6: base.fg = th.added; break;
    default: break;
  }
  return base;
}

std::string unescape_str(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}

// Parses a Question event's `questions` array into a card. Options without a
// label are dropped; the array is capped so a pathological payload cannot cost
// a second of layout.
void parse_question_card(std::string_view json, ChatRenderer::QuestionCard& card) {
  js::scan_array(json, [&](const js::Value& qv) {
    if (!qv.is_object()) return true;
    ChatRenderer::QuestionSpec q;
    js::Value options{};
    js::scan_object(qv.raw, [&](std::string_view k, const js::Value& v) {
      if (k == "question" || k == "title") q.text = unescape_str(v);
      else if (k == "header") q.header = unescape_str(v);
      else if ((k == "multiSelect" || k == "multi") && v.type == js::Type::Bool)
        q.multi = v.is_true();
      else if (k == "recommended" && v.type == js::Type::Number)
        q.recommended = std::atoi(std::string(v.raw).c_str());
      else if (k == "options") options = v;
      return true;
    });
    js::scan_array(options.raw, [&](const js::Value& ov) {
      ChatRenderer::QuestionOption o;
      // The async form lists its options as bare strings.
      if (ov.is_string()) {
        o.label = unescape_str(ov);
        if (!o.label.empty()) q.options.push_back(std::move(o));
        return true;
      }
      if (!ov.is_object()) return true;
      js::scan_object(ov.raw, [&](std::string_view k, const js::Value& v) {
        if (k == "label") o.label = unescape_str(v);
        else if (k == "description") o.description = unescape_str(v);
        return true;
      });
      if (!o.label.empty()) q.options.push_back(std::move(o));
      return true;
    });
    if (!q.text.empty() || !q.options.empty()) card.questions.push_back(std::move(q));
    return card.questions.size() < 8;
  });
}

// Counts source lines remaining after `consumed` bytes, for the "… N more"
// marker. memchr, not a parse.
size_t count_lines(std::string_view s, size_t from) {
  size_t n = 0;
  while (from < s.size()) {
    const void* p = memchr(s.data() + from, '\n', s.size() - from);
    if (!p) return n + 1;
    from = size_t(static_cast<const char*>(p) - s.data()) + 1;
    n++;
  }
  return n;
}

// Appends `s` with whitespace runs collapsed, stopping at `max_cols` display
// columns. Used for the one-line preview beside a tool name.
void put_oneline(Arena& a, std::string_view s, int max_cols) {
  int w = 0;
  bool pending_space = false, started = false;
  size_t i = 0;
  while (i < s.size() && w < max_cols) {
    // A run of printable ASCII goes over in one append, not one per byte.
    if (const unsigned char b = uint8_t(s[i]); b > 0x20 && b < 0x7F) {
      if (pending_space) { a.put(" "); w++; pending_space = false; }
      size_t j = i;
      const size_t room = size_t(std::max(0, max_cols - w));
      while (j < s.size() && j - i < room && uint8_t(s[j]) > 0x20 && uint8_t(s[j]) < 0x7F) j++;
      if (j == i) break;  // the space above used the last column
      a.put(s.substr(i, j - i));
      w += int(j - i);
      started = true;
      i = j;
      continue;
    }
    size_t start = i;
    char32_t cp = text::decode(s, i);
    if (cp == U'\n' || cp == U'\r' || cp == U' ' || cp == U'\t') {
      pending_space = started;
      continue;
    }
    if (cp < 0x20) continue;
    if (pending_space) { a.put(" "); w++; pending_space = false; }
    int cw = text::cp_width(cp);
    if (w + cw > max_cols) break;
    a.put(s.substr(start, i - start));
    w += cw;
    started = true;
  }
}

}  // namespace

std::string_view ChatRenderer::seg_text(const md::Seg& s) const {
  if (s.len == 0) return {};
  return (s.off & kScratch) ? scratch_.view(Str{s.off & ~kScratch, s.len})
                            : arena_.view(Str{s.off, s.len});
}

bool ChatRenderer::expanded(uint64_t id) const {
  return id != 0 && std::binary_search(expanded_.begin(), expanded_.end(), id);
}

void ChatRenderer::toggle(uint64_t id) {
  if (id == 0) return;
  auto it = std::lower_bound(expanded_.begin(), expanded_.end(), id);
  const bool opening = !(it != expanded_.end() && *it == id);
  if (!opening) expanded_.erase(it);
  else expanded_.insert(it, id);
  expand_gen_++;
  if (opening && (id & kFoldBit))
    for (size_t i = 0; i < events_.size(); i++)
      if (fold_of_[i] == id && events_[i].bare) {
        reload_ = true;
        break;
      }
}

void ChatRenderer::reset() {
  activity_tool_ = 0;
  roles_.clear();
  fold_of_.clear();
  open_from_ = 0;
  outline_.clear();
  outline_lo_ = UINT64_MAX;
  outline_hi_ = 0;
  outline_calls_.clear();
  outline_orphans_.clear();
  events_.clear();
  expanded_.clear();
  arena_.clear();
  state_.clear();
  scroll_ = 0;
  pending_.clear();
  questions_all_.clear();
  agent_queue_.clear();
  sent_.clear();
  pending_tools_.clear();
  chart_tools_.clear();
  qstate_.clear();
  async_.clear();
  relayout_ = false;
  answer_ready_ = false;
  // Matches belong to a file; the query stays lit across files.
  find_done_ = false;
  find_lines_.clear();
  find_tools_.clear();
  find_cur_ = -1;
  find_line_ = UINT32_MAX;
  pending_find_ = pending_user_ = 0;
  invalidate_rows();
}

void ChatRenderer::check_chart_files() {
  if (chart_files_.empty()) return;
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const int64_t now = int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
  if (now - chart_check_ms_ < 1000) return;
  chart_check_ms_ = now;
  for (const auto& [path, seen] : chart_files_) {
    struct stat st{};
    const int64_t mtime = stat(path.c_str(), &st) == 0
                              ? int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec
                              : 0;
    if (mtime != seen) {
      invalidate_rows();  // relaid out on this frame, charts re-read
      return;
    }
  }
}

void ChatRenderer::invalidate_rows() {
  draft_rows_ = 0;
  chart_files_.clear();  // the layout that follows records them again
  rows_.clear();
  segs_.clear();
  scratch_.clear();
  cards_.clear();  // re-parsed as each Question is laid out again
  rows_from_event_ = rows_to_event_ = events_.size();
}

bool ChatRenderer::open(const std::string& path, const Adapter* adapter) {
  if (path == open_path_ && file_.is_open()) return true;
  open_path_ = path;
  adapter_ = adapter;
  reset();
  if (!file_.open(path)) {
    adapter_ = nullptr;
    return false;
  }
  arena_.reserve(1u << 20);
  parsed_from_ = parsed_to_ = file_.line_count();
  invalidate_rows();
  return true;
}

void ChatRenderer::poll_growth() {
  if (!file_.is_open() || !adapter_) return;
  if (!file_.refresh()) return;
  grow_forwards();
}

// Images in a line, whichever agent wrote it: one Image event each, a tool
// result's carrying its tool's id. Their pixels stay in the file.
void ChatRenderer::add_images(std::string_view line, size_t before) {
  const int n = count_images(line);
  if (n == 0) return;
  uint64_t tool = 0;
  for (size_t j = before; j < batch_.size(); j++)
    if (batch_[j].kind == EventKind::ToolResult) tool = batch_[j].tool_id;
  for (int k = 0; k < n; k++) {
    Event e;
    e.kind = EventKind::Image;
    e.tool_id = tool;
    e.summary = arena_.add(std::to_string(k));
    batch_.push_back(e);
  }
}

void ChatRenderer::grow_forwards(size_t max_lines) {
  const size_t end = max_lines == size_t(-1)
                         ? file_.line_count()
                         : std::min(file_.line_count(), parsed_to_ + max_lines);
  batch_.clear();
  file_.will_read(parsed_to_, end);
  for (size_t i = parsed_to_; i < end; i++) {
    std::string_view line = file_.line(i);
    if (uint32_t(i) >= state_.from_line) {
      adapter_->observe(line, state_);
      state_.from_line = uint32_t(i);
    }
    size_t before = batch_.size();
    adapter_->parse(line, arena_, batch_);
    add_images(line, before);
    for (size_t j = before; j < batch_.size(); j++) batch_[j].src_line = uint32_t(i);
  }
  parsed_to_ = end;
  events_.append(batch_.data(), batch_.data() + batch_.size());
  for (const Event& e : batch_) note_event(e);
  classify(0);
}

void ChatRenderer::grow_backwards() {
  if (!adapter_) return;
  // Scrolling back fills the arena up to its budget. Growing there by doubling
  // copies the whole window at each step and overshoots to twice the budget;
  // one reservation does neither. Capped at the file's size, which bounds the
  // text it can yield, so a short chat does not reserve for a long one.
  arena_.reserve(std::min<size_t>(kArenaBudget + (kArenaBudget >> 3), file_.size_bytes()));
  if (parsed_from_ == 0) {
    // The file is only indexed from the tail; pull in an older slab first.
    // Every existing line index shifts up by however many lines that added.
    size_t added_lines = file_.extend_back();
    if (added_lines == 0) return;
    shift_lines(added_lines);
    parsed_from_ += added_lines;
    parsed_to_ += added_lines;
  }
  size_t chunk = std::min(parsed_from_, kChunkLines);
  size_t start = parsed_from_ - chunk;
  const size_t mark = arena_.bytes();

  batch_.clear();
  file_.will_read(start, parsed_from_);
  for (size_t i = start; i < parsed_from_; i++) {
    std::string_view line = file_.line(i);
    // Reading backwards: only fill in what nothing newer has already said.
    if (uint32_t(i) >= state_.from_line) {
      adapter_->observe(line, state_);
      state_.from_line = uint32_t(i);
    }
    size_t before = batch_.size();
    adapter_->parse(line, arena_, batch_);
    add_images(line, before);
    for (size_t j = before; j < batch_.size(); j++) batch_[j].src_line = uint32_t(i);
  }
  events_.prepend(batch_.data(), batch_.data() + batch_.size());
  parsed_from_ = start;
  classify(batch_.size());
  if (rows_density_ != Density::Full) strip_folded(batch_.size(), mark);
  // Prepending history must replay activity in transcript order. A result
  // already in the newer window must resolve its newly loaded older call.
  pending_.clear();
  pending_tools_.clear();
  chart_tools_.clear();
  questions_all_.clear();
  agent_queue_.clear();
  async_.clear();
  auto sent = std::move(sent_);
  auto qstate = std::move(qstate_);
  // Replaying reaches the same verdicts the laid-out cards already show.
  const bool relayout = relayout_;
  for (const Event& e : events_) note_event(e);
  relayout_ = relayout;
  std::erase_if(sent, [&](uint64_t id) {
    const AsyncSlot* a = async_slot(id);
    if (a) return a->status != AsyncStatus::Open;
    return std::find(pending_.begin(), pending_.end(), id) == pending_.end();
  });
  std::erase_if(qstate, [&](const QStateSlot& slot) {
    const AsyncSlot* a = async_slot(slot.id);
    if (a) return a->status != AsyncStatus::Open;
    return std::find(pending_.begin(), pending_.end(), slot.id) == pending_.end();
  });
  sent_ = std::move(sent);
  qstate_ = std::move(qstate);
}

void ChatRenderer::shift_lines(size_t count) {
  for (auto& e : events_) e.src_line += uint32_t(count);
  for (auto& r : rows_) r.src_line += uint32_t(count);
  if (!state_.empty()) state_.from_line += uint32_t(count);
  cur_line_ += uint32_t(count);
  menu_line_ += uint32_t(count);
}

bool ChatRenderer::laid_out(size_t i, const Filters& f) const {
  return visible(i, f) || (folding(f) && (roles_[i] & kFoldHead));
}

bool ChatRenderer::visible(size_t i, const Filters& f) const {
  const Event& e = events_[i];
  // Pictures are their own setting: on, they show at every density, even
  // among a folded turn's steps.
  if (e.kind == EventKind::Image && render_settings().pictures) return true;
  if (folded_away(i, f)) return false;
  switch (e.kind) {
    case EventKind::User:
    case EventKind::Assistant:
      return arena_.view(e.text).find_first_not_of(" \t\r\n") != std::string_view::npos;
    case EventKind::TurnEnd: return false;
    case EventKind::QueueAdd:
    case EventKind::QueueTake: return false;
    case EventKind::Meta: return f.show_meta();
    case EventKind::Thinking: return f.show_thinking();
    case EventKind::ToolCall: return f.show_tools() && (!activity_tool_ || e.tool_id != activity_tool_);
    case EventKind::TaskStatus: return f.show_tools() || !e.ok;
    case EventKind::Question: return true;
    // A tool's image (a screenshot) shows with tool calls; the user's always.
    case EventKind::Image: return !e.tool_id || f.show_tools();
    case EventKind::ToolResult:
      // An optional question's result is only codex's receipt; the answer
      // arrives later as a message and is drawn on the card.
      if (async_slot(e.tool_id)) return false;
      // A chart that was drawn needs no "drawn" under it; one that could not
      // be still shows why.
      if (e.ok && std::find(chart_tools_.begin(), chart_tools_.end(), e.tool_id) != chart_tools_.end())
        return false;
      // A question's answer is part of the card, so it shows whatever the
      // density filters say — unless the card already folded it in.
      if (std::find(questions_all_.begin(), questions_all_.end(), e.tool_id) !=
          questions_all_.end()) {
        std::vector<std::string> answers;
        return !question_answers(e.tool_id, answers);
      }
      return f.show_tools() && (f.show_results() || expanded(e.tool_id));
    default: return true;
  }
}

// Moves the markdown output in mdlines_/segs_ into rows_.
void ChatRenderer::flush_lines(RowStyle base, int extra_indent, uint64_t tool_id,
                               Gutter gutter) {
  for (const auto& L : mdlines_) {
    rows_.push_back(Row{tool_id, L.seg_first, cur_line_, L.seg_count,
                        uint8_t(L.indent + extra_indent), base, gutter});
    rows_.back().code = L.flags & md::kCodeRow;
    rows_.back().tint = (L.flags & md::kAddRow) ? 1 : (L.flags & md::kDelRow) ? 2 : 0;
    if (mdline_nodes_.size() == mdlines_.size()) rows_.back().node = mdline_nodes_[size_t(&L - mdlines_.data())];
  }
  mdline_nodes_.clear();
}

void ChatRenderer::emit_text(Str text, RowStyle base, int indent, int w, size_t cap,
                             uint64_t tool_id, bool markdown, bool diff, Gutter gutter) {
  std::string_view body = arena_.view(text);
  if (base == RowStyle::User) {
    // Drop leading blank lines without stripping indentation from the first
    // content line or changing any spacing inside the message.
    size_t start = 0;
    while (start < body.size()) {
      const size_t end = body.find('\n', start);
      if (end == std::string_view::npos ||
          body.substr(start, end - start).find_first_not_of(" \t\r") != std::string_view::npos) break;
      start = end + 1;
    }
    body.remove_prefix(start);
    text.off += uint32_t(start);
    text.len -= uint32_t(start);
  }
  if (body.empty()) return;

  mdlines_.clear();
  md::Out out{cap, &scratch_, &segs_, &mdlines_, &spans_, &inline_, &md_work_};
  chart_env_.watched = &chart_files_;
  out.charts = &chart_env_;
  const int cols = std::max(4, w - indent);

  if (diff) md::render_diff(body, text.off, false, cols, out, output_lang_);
  else if (markdown) md::render(body, text.off, false, cols, out);
  else {
    // Tool output is not prose: no block structure, no inline markup; its
    // colours, links and (for a file's contents) its language are kept.
    // A JSON result is laid out and coloured: an outline while the result
    // is collapsed, the whole of it indented once expanded.
    std::string pretty;
    // A notebook the agent read is shown as one: cells, outputs, plots.
    if (base == RowStyle::Result && render_settings().notebooks &&
        ((notebook::is_claude_read(body) && notebook::from_claude(file_.line(cur_line_), pretty)) ||
         (body.size() < (16u << 20) && body.find("\"nbformat\"") != std::string_view::npos &&
          notebook::from_ipynb(body, pretty)))) {
      if (tool_id) notebook_tools_.insert(tool_id);
      const Str s = scratch_.add(pretty);
      md::render(pretty, s.off, true, cols, out);
    } else if (json_view::Folding fold;
               render_settings().json && !output_lang_ && body.size() < (8u << 20) &&
               json_view::pretty(body, cap <= kCollapsedResultRows, pretty,
                                 cap > kCollapsedResultRows && tool_id ? &fold : nullptr)) {
      if (cap > kCollapsedResultRows && tool_id)
        if (const auto it = json_flips_.find(tool_id); it != json_flips_.end()) {
          // Laid out again with the reader's own choices.
          fold.flipped = &it->second;
          json_view::pretty(body, false, pretty, &fold);
        }
      const Str s = scratch_.add(pretty);
      static const code::Lang* const json = code::lang_of("json");
      // The rows point into scratch; the text is read from the local copy,
      // which stays put while scratch grows.
      md::render_output(pretty, s.off, true, cols, out, json, true);
      if (!fold.line_nodes.empty()) {
        // Which line each row shows, from where its text starts.
        std::vector<uint32_t> starts{0};
        for (uint32_t i = 0; i < pretty.size(); i++)
          if (pretty[i] == '\n') starts.push_back(i + 1);
        mdline_nodes_.assign(mdlines_.size(), 0);
        for (size_t k = 0; k < mdlines_.size(); k++) {
          const md::Line& L = mdlines_[k];
          if (!L.seg_count) continue;
          const uint32_t at = (segs_[L.seg_first].off & ~kScratch) - s.off;
          const size_t line = size_t(std::upper_bound(starts.begin(), starts.end(), at) - starts.begin()) - 1;
          if (line < fold.line_nodes.size()) mdline_nodes_[k] = fold.line_nodes[line];
        }
      }
    } else {
      md::render_output(body, text.off, false, cols, out, output_lang_);
    }
  }

  flush_lines(base, indent, tool_id, gutter);

  if (mdlines_.size() >= cap) {
    // Report what is left in source lines, which is what a reader expects to
    // see when they expand it.
    size_t shown_bytes = 0;
    if (!segs_.empty()) {
      const md::Seg& last = segs_.back();
      if (!(last.off & kScratch) && last.off >= text.off)
        shown_bytes = size_t(last.off - text.off) + last.len;
    }
    size_t more = count_lines(body, std::min(shown_bytes, body.size()));
    if (more > 0) {
      char buf[48];
      int n = snprintf(buf, sizeof buf, "\xE2\x80\xA6 %zu more lines", more);
      Str m = scratch_.add(std::string_view(buf, size_t(n)));
      segs_.push_back(md::Seg{m.off | kScratch, m.len, md::Ink::Text});
      rows_.push_back(Row{tool_id, uint32_t(segs_.size() - 1), cur_line_, 1,
                          uint8_t(indent), RowStyle::Dim, gutter});
    }
  }
}

// The language of a tool result that is a file's contents: what its call
// read (Read's path), or what a shell command printed (cat, head, tail, sed
// -n on one file). Null for anything else.
const code::Lang* ChatRenderer::result_lang(size_t index) const {
  const Event& r = events_[index];
  for (size_t k = index; k-- > 0 && index - k < 400;) {
    const Event& c = events_[k];
    if (c.kind != EventKind::ToolCall || c.tool_id != r.tool_id) continue;
    const std::string_view name = arena_.view(c.name);
    std::string_view arg = arena_.view(c.summary);
    std::string_view path;
    if (name == "Read" || name == "read" || name == "view" || name == "read_file") {
      path = arg;
    } else {
      // A shell command that only prints one file.
      for (std::string_view tool : {"cat ", "head ", "tail ", "sed -n ", "bat ", "less "}) {
        if (!arg.starts_with(tool)) continue;
        if (arg.find_first_of("|;&><") != std::string_view::npos) break;
        const size_t sp = arg.rfind(' ');
        path = arg.substr(sp + 1);
        break;
      }
    }
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos) return nullptr;
    return code::lang_of(path.substr(dot + 1));
  }
  return nullptr;
}

// An image from the transcript, read out of its line now: a picture where
// the terminal shows them, a line saying there is one where it does not.
void ChatRenderer::layout_image(const Event& e, int w) {
  const bool tool = e.tool_id != 0;
  const int indent = tool ? 4 : 2;
  const std::string_view line = file_.line(e.src_line);
  const int n = std::atoi(std::string(arena_.view(e.summary)).c_str());
  std::string media;
  std::string_view b64;
  const math::Image* im = nullptr;
  mdlines_.clear();
  md::Out out{kMaxRowsPerEvent, &scratch_, &segs_, &mdlines_, &spans_, &inline_, &md_work_};
  if (render_settings().pictures && transcript_image(line, n, &media, &b64)) {
    // Named by the data itself, so the same screenshot twice is drawn once,
    // and a window shifted by older history still finds it.
    const std::string key = "tx:" + std::to_string(b64.size()) + ":" +
                            std::string(b64.substr(0, 40)) + std::string(b64.substr(b64.size() - std::min<size_t>(40, b64.size())));
    im = math::picture(key, [b64](std::string& bytes) { return math::base64_decode(b64, bytes); },
                       std::max(4, w - indent - 3), 24, "[image]");
  }
  if (im) {
    md::picture_rows(*im, 0, out);
  } else {
    const Str t = scratch_.add("\xE2\x96\xA3 image" + (media.empty() ? std::string() : " (" + media + ")"));
    segs_.push_back(md::Seg{t.off | kScratch, t.len, md::Ink::Text});
    mdlines_.push_back(md::Line{uint32_t(segs_.size() - 1), 1, 0});
  }
  flush_lines(tool ? RowStyle::Dim : RowStyle::Assistant, indent, 0, Gutter::None);
}

void ChatRenderer::layout_event(size_t index, int w, const Filters& f) {
  const Event& e = events_[index];
  cur_line_ = e.src_line;
  auto gap = [&] { rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None}); };

  // A finished turn's steps start with the line they fold into.
  if (folding(f) && (roles_[index] & kFoldHead)) {
    layout_fold(index, w);
    if (!visible(index, f)) return;
  }

  switch (e.kind) {
    // No "You" / agent name above a turn: the user's gutter bar and tint
    // already say who is speaking, and the label cost a row per turn.
    case EventKind::User:
      gap();
      // Indented behind a gutter bar, the way a quoted block reads: the turn
      // has a visible left edge for its whole height, however long it is.
      emit_text(e.text, RowStyle::User, 2, w, kMaxRowsPerEvent, 0, true, false, Gutter::User);
      break;

    // The answer has an edge to find it by; what the agent said on the way
    // there is quieter.
    case EventKind::Assistant:
      gap();
      if (roles_[index] & kAnswer)
        emit_text(e.text, RowStyle::Assistant, 1, w, kMaxRowsPerEvent, 0, true, false, Gutter::Answer);
      else
        emit_text(e.text, (roles_[index] & kWork) ? RowStyle::Work : RowStyle::Assistant, 0, w, kMaxRowsPerEvent,
                  0, true, false);
      break;

    case EventKind::Thinking:
      emit_text(e.text, RowStyle::Thinking, 2, w, 40, 0, true, false);
      break;

    case EventKind::Image:
      if (e.tool_id && notebook_tools_.count(e.tool_id)) break;
      layout_image(e, w);
      break;

    // mico's plot tool: the call is the chart, drawn like one in a message.
    case EventKind::Chart:
      gap();
      emit_text(e.text, RowStyle::Assistant, 0, w, kMaxRowsPerEvent, 0, true, false);
      break;

    case EventKind::ToolCall: {
      const bool open = expanded(e.tool_id);
      // The bullet/spinner is drawn at render time (it depends on whether the
      // call is still executing), so the text starts past it.
      uint32_t off = scratch_.open();
      scratch_.put(arena_.view(e.name));
      if (!e.summary.empty()) {
        scratch_.put("  ");
        put_oneline(scratch_, arena_.view(e.summary), std::max(4, w - 2));
      }
      emit_scratch(scratch_.close(off), RowStyle::Tool, 2, e.tool_id, 0, 0xFF);

      if (open && !e.summary.empty()) {
        // An expanded Edit or apply_patch is a diff; show it as one, in the
        // language of the file it edits.
        const bool diff = !e.detail.empty();
        {
          const std::string_view path = arena_.view(e.summary);
          const size_t dot = path.rfind('.');
          output_lang_ = dot != std::string_view::npos && path.find('/', dot) == std::string_view::npos
                             ? code::lang_of(path.substr(dot + 1))
                             : nullptr;
        }
        emit_text(diff ? e.detail : e.summary, RowStyle::Dim, 4, w, kMaxRowsPerEvent,
                  e.tool_id, false, diff);
        output_lang_ = nullptr;
      }
      break;
    }

    case EventKind::Question:
      layout_question(e, w);
      break;

    case EventKind::Notice: {
      // A divider rather than a message: it marks a break in the conversation.
      gap();
      uint32_t off = scratch_.open();
      scratch_.put("\xE2\x94\x80\xE2\x94\x80 ");  // ──
      put_oneline(scratch_, arena_.view(e.text), std::max(4, w - 8));
      scratch_.put(" \xE2\x94\x80\xE2\x94\x80");
      emit_scratch(scratch_.close(off), RowStyle::Dim, 0, 0, 0, 0xFF);
      break;
    }

    case EventKind::TaskStatus: {
      uint32_t off = scratch_.open();
      scratch_.put(e.ok ? "✓ " : "! ");
      put_oneline(scratch_, arena_.view(e.text), std::max(4, w - 6));
      emit_scratch(scratch_.close(off), e.ok ? RowStyle::Dim : RowStyle::ResultErr,
                   2, e.tool_id, 0, 0xFF);
      break;
    }

    case EventKind::ToolResult: {
      const bool open = expanded(e.tool_id);
      output_lang_ = result_lang(index);
      emit_text(e.text, e.ok ? RowStyle::Result : RowStyle::ResultErr, 4, w,
                open ? kMaxRowsPerEvent : kCollapsedResultRows, e.tool_id, false, false);
      output_lang_ = nullptr;
      break;
    }

    default:
      emit_text(e.text, RowStyle::Dim, 0, w, 2, 0, false, false);
      break;
  }
}

namespace {

// What of a message survives an agent drawing it: letters and digits, in
// order. Markup, bullets, box drawing and typographic punctuation come and go
// between the markdown and the screen; the words do not.
std::string reading(std::string_view s, size_t cap) {
  std::string out;
  size_t i = 0;
  while (i < s.size() && out.size() < cap) {
    const uint8_t b = uint8_t(s[i]);
    if (b < 0x80) {
      if (std::isalnum(b)) out.push_back(char(b));
      i++;
      continue;
    }
    const size_t at = i;
    const char32_t cp = text::decode(s, i);
    // Letters outside ASCII count; punctuation, arrows, shapes and box
    // drawing (U+2000 to U+2BFF) do not, nor a no-break space.
    if (cp > 0xA0 && !(cp >= 0x2000 && cp <= 0x2BFF)) out.append(s.substr(at, i - at));
    if (i == at) i++;
  }
  return out;
}

// True when `draft` reads as the start of `text`, near enough letter for
// letter: the screen may drop a code fence's language or a link's target, and
// a glyph may not survive the emulator. A match that has to skip far ahead
// for each letter is chance, not the same words.
bool reads_as(std::string_view text, std::string_view draft) {
  size_t i = 0, skipped = 0, missed = 0;
  for (const char c : draft) {
    const size_t k = text.find(c, i);
    if (k != std::string_view::npos && k - i <= 24) {
      skipped += k - i;
      i = k + 1;
    } else {
      missed++;
    }
  }
  return missed <= draft.size() / 20 + 1 && skipped <= draft.size() / 8 + 24;
}

}  // namespace

bool ChatRenderer::draft_shown() {
  if (draft_.empty()) return false;
  if (draft_checked_ && draft_events_ == events_.size()) return !draft_committed_;
  draft_checked_ = true;
  draft_events_ = events_.size();
  // Held by the transcript once a message of the current turn reads the same.
  const std::string d = reading(draft_, 8000);
  draft_committed_ = d.empty();
  for (size_t i = events_.size(); i-- > 0 && !draft_committed_;) {
    const Event& e = events_[i];
    if (e.kind == EventKind::User) break;
    if (e.kind != EventKind::Assistant) continue;
    // Either way round: the screen can hold more than the block, when rows
    // of whatever follows it were taken for its own.
    const std::string m = reading(arena_.view(e.text), 12000);
    if (reads_as(m, d) || (m.size() >= 16 && reads_as(d, m))) draft_committed_ = true;
  }
  return !draft_committed_;
}

void ChatRenderer::layout_draft(int w) {
  draft_rows_ = 0;
  // Only under the newest message: a window re-anchored further back does not
  // end where the draft goes.
  if (parsed_to_ != file_.line_count()) return;
  const bool draft = draft_shown();
  if (!draft && aside_q_.empty()) return;
  const size_t before = rows_.size();
  draft_segs_ = segs_.size();
  draft_scratch_ = scratch_.bytes();
  cur_line_ = uint32_t(parsed_to_ ? parsed_to_ - 1 : 0);
  if (draft) {
    rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None});
    // The text goes into scratch, where the rows find it; the renderer reads
    // it from draft_, which the markdown pass does not grow under it.
    const Str s = scratch_.add(draft_);
    mdlines_.clear();
    md::Out out{kMaxRowsPerEvent, &scratch_, &segs_, &mdlines_, &spans_, &inline_, &md_work_};
    md::render(draft_, s.off, true, std::max(4, w), out);
    // Answer or not is known only once the turn ends.
    flush_lines(RowStyle::Work, 0, 0, Gutter::None);
  }
  if (!aside_q_.empty()) {
    // The side question reads as an aside, not a turn: headed like a question
    // card, the answer indented under it, the keys in a hint line.
    rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None});
    const std::string head = "\xE2\x97\x87 btw  " + aside_q_;  // ◇
    std::vector<text::Span> spans;
    text::wrap_spans(head, std::max(4, w), spans, 4);
    for (const auto& sp : spans)
      emit_scratch(scratch_.add(std::string_view(head).substr(sp.off, sp.len)), RowStyle::Question, 0, 0, 0,
                   0xFF, false);
    if (!aside_a_.empty()) {
      const Str s = scratch_.add(aside_a_);
      mdlines_.clear();
      md::Out out{kMaxRowsPerEvent, &scratch_, &segs_, &mdlines_, &spans_, &inline_, &md_work_};
      md::render(aside_a_, s.off, true, std::max(4, w - 2), out);
      flush_lines(RowStyle::Assistant, 2, 0, Gutter::None);
    }
    if (!aside_status_.empty())
      emit_scratch(scratch_.add(aside_status_), RowStyle::QHint, 2, 0, 0, 0xFF, false);
  }
  draft_rows_ = rows_.size() - before;
}

void ChatRenderer::drop_draft_rows() {
  if (!draft_rows_) return;
  rows_.resize(rows_.size() - draft_rows_);
  segs_.resize(draft_segs_);
  scratch_.truncate(draft_scratch_);
  draft_rows_ = 0;
}

void ChatRenderer::emit_scratch(Str s, RowStyle base, int indent, uint64_t tool_id, uint8_t q,
                                uint8_t opt, bool cont) {
  if (s.len == 0) return;
  segs_.push_back(md::Seg{s.off | kScratch, s.len, md::Ink::Text});
  rows_.push_back(Row{tool_id, uint32_t(segs_.size() - 1), cur_line_, 1, uint8_t(indent),
                      base, Gutter::None, q, opt, cont});
}

// Keeps the live-question sets in step with the parsed window: a Question makes
// an id pending, its ToolResult resolves it.
void ChatRenderer::note_event(const Event& e) {
  if (e.kind == EventKind::Chart && e.tool_id) chart_tools_.push_back(e.tool_id);
  if (e.kind == EventKind::TurnEnd) {
    // An optional question outlives the turn that asked it.
    pending_.clear();
    pending_tools_.clear();
    std::erase_if(sent_, [&](uint64_t id) { return !async_slot(id); });
    std::erase_if(qstate_, [&](const QStateSlot& slot) { return !async_slot(slot.id); });
    return;
  }
  if (e.kind == EventKind::QueueAdd) {
    agent_queue_.push_back(e.text);
    return;
  }
  if (e.kind == EventKind::QueueTake) {
    // By its text when the record names it, else the oldest.
    const std::string_view text = arena_.view(e.text);
    auto it = agent_queue_.begin();
    if (!text.empty())
      it = std::find_if(agent_queue_.begin(), agent_queue_.end(),
                        [&](const Str& s) { return arena_.view(s) == text; });
    if (it != agent_queue_.end()) agent_queue_.erase(it);
    return;
  }
  if (e.kind == EventKind::Question) {
    questions_all_.push_back(e.tool_id);
    if (is_async_question_tool(arena_.view(e.name))) {
      // A newer optional question takes the keys from an older one, whose card
      // then has to drop its live rows.
      if (open_async_questions()) relayout_ = true;
      async_.push_back(AsyncSlot{e.tool_id, e.text, {}, AsyncStatus::Open});
    } else {
      pending_.push_back(e.tool_id);
    }
    return;
  }
  if (e.kind == EventKind::User && open_async_questions()) {
    // A reply names the question it answers by its call (codex 0.159's
    // envelope, read by the adapter), or quotes it, the way older codex wrote
    // it. Any other message moves past everything still open.
    const std::string_view text = arena_.view(e.text);
    AsyncSlot* answered = nullptr;
    for (auto& a : async_) {
      if (a.status != AsyncStatus::Open) continue;
      if (e.tool_id) {
        if (a.id == e.tool_id) answered = &a;
        continue;
      }
      std::string_view title = arena_.view(a.title);
      title = title.substr(0, title.find('\n'));
      if (!title.empty() && text.starts_with("> ") && text.substr(2).starts_with(title))
        answered = &a;
    }
    // Another answer to a question already settled (codex answers a card's
    // questions one by one) leaves the rest open.
    if (!answered && e.tool_id) return;
    const auto settle = [&](AsyncSlot& a, AsyncStatus status) {
      a.status = status;
      std::erase(sent_, a.id);
      std::erase_if(qstate_, [&](const QStateSlot& slot) { return slot.id == a.id; });
      relayout_ = true;
    };
    if (answered) {
      const size_t gap = text.find("\n\n");
      if (gap != std::string_view::npos)
        answered->answer = Str{uint32_t(e.text.off + gap + 2), uint32_t(text.size() - gap - 2)};
      settle(*answered, AsyncStatus::Answered);
    } else {
      for (auto& a : async_)
        if (a.status == AsyncStatus::Open) settle(a, AsyncStatus::Skipped);
    }
    return;
  }
  if (e.kind == EventKind::ToolCall) {
    if (e.tool_id) pending_tools_.push_back(e.tool_id);
    return;
  }
  if (e.kind != EventKind::ToolResult && e.kind != EventKind::TaskStatus) return;
  // The card was laid out in full while it waited; its answer folds it down.
  if (e.kind == EventKind::ToolResult &&
      std::find(questions_all_.begin(), questions_all_.end(), e.tool_id) != questions_all_.end())
    relayout_ = true;
  auto drop = [&](std::vector<uint64_t>& v) {
    v.erase(std::remove(v.begin(), v.end(), e.tool_id), v.end());
  };
  drop(pending_);
  drop(sent_);
  drop(pending_tools_);
  for (auto it = qstate_.begin(); it != qstate_.end(); ++it)
    if (it->id == e.tool_id) { qstate_.erase(it); break; }
}

// The card itself: a header chip, the question, then one row per option. The
// cursor and check are painted at render time from the live state, so moving
// the choice never re-lays-out the transcript.
bool ChatRenderer::question_answers(uint64_t id, std::vector<std::string>& out) const {
  out.clear();
  if (id == 0) return false;
  const Event* ask = nullptr;
  const Event* result = nullptr;
  for (const Event& ev : events_) {
    if (ev.tool_id != id) continue;
    if (ev.kind == EventKind::Question) ask = &ev;
    else if (ev.kind == EventKind::ToolResult) result = &ev;
  }
  if (!ask || !result || !result->ok) return false;
  if (is_async_question_tool(arena_.view(ask->name))) return false;  // drawn from the reply
  QuestionCard card;
  parse_question_card(arena_.view(ask->detail), card);
  if (card.questions.empty()) return false;

  // claude writes each one as "<question>"="<answer>", a multi-select's labels
  // joined by ", ", and follows the closing quote with punctuation or with
  // notes and a preview — never with more of the answer.
  const std::string_view text = arena_.view(result->text);
  for (const QuestionSpec& q : card.questions) {
    const std::string key = "\"" + q.text + "\"=\"";
    const size_t at = text.find(key);
    if (q.text.empty() || at == std::string_view::npos) return false;
    const size_t from = at + key.size();
    size_t end = from;
    for (;; end++) {
      end = text.find('"', end);
      if (end == std::string_view::npos) return false;
      if (end + 1 == text.size() || std::string_view(".,\n ").find(text[end + 1]) !=
                                        std::string_view::npos)
        break;
    }
    out.emplace_back(text.substr(from, end - from));
  }
  return true;
}

void ChatRenderer::layout_question(const Event& e, int w) {
  cur_line_ = e.src_line;
  rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None, 0, 0xFF});

  QuestionCard card;
  card.tool_id = e.tool_id;
  card.tool = std::string(arena_.view(e.name));
  card.async = is_async_question_tool(card.tool);
  if (card.async) card.call_id = std::string(arena_.view(e.summary));
  parse_question_card(arena_.view(e.detail), card);
  if (card.questions.empty()) {
    // A payload this reader could not read still shows its preview text.
    emit_text(e.text, RowStyle::Tool, 2, w, 4, e.tool_id, false, false);
    rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None, 0, 0xFF});
    return;
  }

  // The newest unanswered card is the one answerable here; its state has to
  // exist before the first paint, or that frame draws no cursor.
  const bool live = q_interactive_ && live_question() == e.tool_id &&
                    std::find(sent_.begin(), sent_.end(), e.tool_id) == sent_.end();
  const AsyncSlot* slot = card.async ? async_slot(e.tool_id) : nullptr;
  if (live) state_for(e.tool_id, card);

  // Once answered, the options have done their job. What is worth keeping in
  // the scrollback is what was asked and what was chosen, so the card folds
  // down to exactly that and the raw result beneath it is hidden.
  std::vector<std::string> answers;
  if (!live && question_answers(e.tool_id, answers)) {
    for (size_t qi = 0; qi < card.questions.size(); qi++) {
      std::string asked = "\xE2\x9D\x93 " + card.questions[qi].text;  // ❓
      std::string chose = "\xE2\x86\xB3 " + answers[qi];               // ↳
      for (std::string* s : {&asked, &chose})
        for (char& ch : *s)
          if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
      for (const auto& [line, style, indent] :
           {std::tuple{&asked, RowStyle::QText, 2}, std::tuple{&chose, RowStyle::Question, 4}}) {
        text::wrap_spans(*line, std::max(4, w - indent), spans_);
        int n = 0;
        for (const auto& sp : spans_) {
          if (n++ >= 12) break;
          Str s = scratch_.add(std::string_view(*line).substr(sp.off, sp.len));
          emit_scratch(s, style, indent, e.tool_id, uint8_t(qi), 0xFF);
        }
      }
    }
    rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None, 0, 0xFF});
    cards_.push_back(std::move(card));
    return;
  }

  // Wraps `text` over as many rows as it needs, up to `cap`. With `cont`, the
  // rows after the first are marked as continuations: an option's cursor and
  // check go on its first row only, while a click anywhere on it still counts.
  const auto emit = [&](const std::string& text, RowStyle base, int indent, int cap,
                        uint8_t q, uint8_t opt, bool first_cont = false) {
    if (text.empty()) return;
    const int cols = std::max(4, w - indent);
    text::wrap_spans(text, cols, spans_);
    int n = 0;
    for (const auto& sp : spans_) {
      if (n >= cap) break;
      Str s = scratch_.add(std::string_view(text).substr(sp.off, sp.len));
      emit_scratch(s, base, indent, e.tool_id, q, opt, first_cont || n > 0);
      n++;
    }
  };
  const auto flat = [](std::string t) {
    for (char& ch : t)
      if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    return t;
  };
  const QState* live_st = live ? qstate_of(e.tool_id) : nullptr;

  for (size_t qi = 0; qi < card.questions.size(); qi++) {
    const QuestionSpec& q = card.questions[qi];
    if (qi) rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None, 0, 0xFF});
    // A header row is clickable (to switch questions) on a live card.
    std::string header = "\xE2\x9D\x93";
    if (!q.header.empty()) header += " " + q.header;
    else if (card.async) header += " Question";
    if (card.async && qi == 0) header += "  \xC2\xB7 optional";
    emit(header, RowStyle::Question, 2, 1, uint8_t(qi), live ? 0xFD : 0xFF);
    emit(q.text, RowStyle::QText, 4, 12, uint8_t(qi), 0xFF);

    // An option that fits goes on one row, label and description together.
    // A longer one wraps: the label first, then its description on rows of
    // its own, all of them answering to a click on the option.
    const int cols = std::max(4, w - 4);
    for (size_t oi = 0; oi < q.options.size(); oi++) {
      const std::string label = flat(q.options[oi].label);
      const std::string desc = flat(q.options[oi].description);
      const std::string both = desc.empty() ? label : label + "  " + desc;
      if (text::str_width(both) <= cols) {
        emit(both, RowStyle::QOption, 4, 1, uint8_t(qi), uint8_t(oi));
        continue;
      }
      emit(label, RowStyle::QOption, 4, 12, uint8_t(qi), uint8_t(oi));
      emit(desc, RowStyle::QHint, 4, 12, uint8_t(qi), uint8_t(oi), true);
    }
    if (live_st && qi < live_st->notes.size() && !live_st->notes[qi].empty())
      emit("\xE2\x9C\x8E " + flat(live_st->notes[qi]), RowStyle::QText, 4, 12, uint8_t(qi), 0xFF);
    // codex always lets an optional question be answered freely, options or not.
    if (card.async && live) {
      uint32_t off = scratch_.open();
      scratch_.put("\xE2\x9C\x8E Answer in your own words");
      emit_scratch(scratch_.close(off), RowStyle::QOption, 4, e.tool_id, uint8_t(qi), 0xFC);
    }
  }
  if (slot && slot->status == AsyncStatus::Answered) {
    std::string reply = "\xE2\x86\xB3 ";
    reply += arena_.view(slot->answer);
    for (char& ch : reply)
      if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    emit(reply, RowStyle::QText, 4, 6, 0, 0xFF);
  } else if (slot && slot->status == AsyncStatus::Skipped) {
    emit("not answered \xE2\x80\x94 the conversation moved on", RowStyle::QHint, 4, 1, 0, 0xFF);
  }
  if (live && card_needs_submit(card)) {
    uint32_t off = scratch_.open();
    scratch_.put("[ Submit ]");
    emit_scratch(scratch_.close(off), RowStyle::QSubmit, 4, e.tool_id,
                 uint8_t(card.questions.size()), 0xFE);
  }
  if (live && card.async) {
    uint32_t off = scratch_.open();
    scratch_.put("the agent keeps working \xC2\xB7 answer now, later, or just keep chatting");
    emit_scratch(scratch_.close(off), RowStyle::QHint, 2, e.tool_id, 0, 0xFF);
  }
  const bool any_multi = std::any_of(card.questions.begin(), card.questions.end(),
                                     [](const QuestionSpec& q) { return q.multi; });
  if (live && !card.async && (card.questions.size() > 1 || any_multi)) {
    uint32_t off = scratch_.open();
    if (card.questions.size() > 1)
      scratch_.put("\xE2\x86\x90/\xE2\x86\x92 question   ");  // ←/→
    scratch_.put("\xE2\x86\x91/\xE2\x86\x93 option   n note   ");  // ↑/↓
    // Tab walks the questions, then Submit: with one question it is Submit.
    if (any_multi) scratch_.put(card.questions.size() > 1 ? "enter toggle   tab next" : "enter toggle   tab submit");
    else scratch_.put("enter select");
    emit_scratch(scratch_.close(off), RowStyle::QHint, 2, e.tool_id, 0, 0xFF);
  } else if (live && !card.async) {
    uint32_t off = scratch_.open();
    scratch_.put("\xE2\x86\x91/\xE2\x86\x93 option   n note   enter select");
    emit_scratch(scratch_.close(off), RowStyle::QHint, 2, e.tool_id, 0, 0xFF);
  }
  rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None, 0, 0xFF});
  cards_.push_back(std::move(card));
}

ChatRenderer::QState* ChatRenderer::state_for(uint64_t id, const QuestionCard& card) {
  for (auto& s : qstate_)
    if (s.id == id) return &s.st;
  QState st;
  st.q = 0;
  st.cursor.resize(card.questions.size(), 0);
  st.chosen.resize(card.questions.size());
  for (size_t qi = 0; qi < card.questions.size(); qi++) {
    const QuestionSpec& q = card.questions[qi];
    const int n = int(q.options.size());
    st.cursor[qi] = std::clamp(q.recommended, 0, std::max(0, n - 1));
    st.chosen[qi].assign(size_t(std::max(0, n)), 0);
  }
  qstate_.push_back(QStateSlot{id, std::move(st)});
  return &qstate_.back().st;
}

const ChatRenderer::QState* ChatRenderer::qstate_of(uint64_t id) const {
  for (const auto& s : qstate_)
    if (s.id == id) return &s.st;
  return nullptr;
}

const ChatRenderer::QuestionCard* ChatRenderer::card(uint64_t id) const {
  for (const auto& c : cards_)
    if (c.tool_id == id) return &c;
  return nullptr;
}

bool ChatRenderer::tool_pending(uint64_t id) const {
  return parsed_to_ == file_.line_count() && id != 0 &&
         std::find(pending_tools_.begin(), pending_tools_.end(), id) != pending_tools_.end();
}

bool ChatRenderer::in_flight_tool(std::string_view* name, std::string_view* summary, uint64_t* id) const {
  if (!working_ || pending_tools_.empty()) return false;
  // The newest call still waiting on a result: the one executing right now.
  for (size_t i = events_.size(); i-- > 0;) {
    const Event& e = events_[i];
    if (e.kind != EventKind::ToolCall || !tool_pending(e.tool_id)) continue;
    if (name) *name = arena_.view(e.name);
    if (summary) *summary = arena_.view(e.summary);
    if (id) *id = e.tool_id;
    return true;
  }
  return false;
}

const ChatRenderer::AsyncSlot* ChatRenderer::async_slot(uint64_t id) const {
  for (const auto& a : async_)
    if (a.id == id) return &a;
  return nullptr;
}

int ChatRenderer::open_async_questions() const {
  return int(std::count_if(async_.begin(), async_.end(),
                           [](const AsyncSlot& a) { return a.status == AsyncStatus::Open; }));
}

uint64_t ChatRenderer::live_question() const {
  if (!q_interactive_ || parsed_to_ != file_.line_count()) return 0;
  if (!pending_.empty()) return pending_.back();
  for (auto it = async_.rbegin(); it != async_.rend(); ++it)
    if (it->status == AsyncStatus::Open) return it->id;
  return 0;
}

int ChatRenderer::cursor_stops(const QuestionCard& card, int q) const {
  if (q < 0 || q >= int(card.questions.size())) return 0;
  return int(card.questions[size_t(q)].options.size()) + (card.async ? 1 : 0);
}

bool ChatRenderer::question_live(uint64_t id) const {
  if (id == 0 || live_question() != id) return false;
  if (std::find(sent_.begin(), sent_.end(), id) != sent_.end()) return false;
  const QuestionCard* c = card(id);
  return c && !c->questions.empty();
}

bool ChatRenderer::question_active() const { return question_live(live_question()); }

bool ChatRenderer::card_needs_submit(const QuestionCard& card) const {
  return card.questions.size() > 1 || (!card.questions.empty() && card.questions[0].multi);
}

void ChatRenderer::question_advance(QState& st, const QuestionCard& card, uint64_t id) {
  const int n = int(card.questions.size());
  if (st.q + 1 < n) { st.q++; return; }
  if (card_needs_submit(card)) { st.q = n; return; }
  commit_answer(id, card, st.chosen);
}

void ChatRenderer::commit_answer(uint64_t id, const QuestionCard& card,
                                 const std::vector<std::vector<uint8_t>>& chosen) {
  answer_.tool_id = id;
  answer_.tool = card.tool;
  answer_.card = card;
  answer_.chosen = chosen;
  answer_.free_text = false;
  answer_.free_q = 0;
  answer_.notes.clear();
  if (const QState* st = qstate_of(id)) answer_.notes = st->notes;
  answer_ready_ = true;
}

int ChatRenderer::question_note_target(std::string* header) const {
  if (!question_active()) return -1;
  const uint64_t id = live_question();
  const QuestionCard* c = card(id);
  const QState* st = qstate_of(id);
  if (!c || !st || c->async || c->questions.empty()) return -1;
  // On the Submit step the note is for the last question.
  const int qi = std::clamp(st->q, 0, int(c->questions.size()) - 1);
  if (header) {
    const QuestionSpec& q = c->questions[size_t(qi)];
    *header = !q.header.empty() ? q.header : q.text;
  }
  return qi;
}

const std::string& ChatRenderer::question_note(int qi) const {
  static const std::string none;
  const QState* st = question_active() ? qstate_of(live_question()) : nullptr;
  if (!st || qi < 0 || size_t(qi) >= st->notes.size()) return none;
  return st->notes[size_t(qi)];
}

bool ChatRenderer::set_question_note(int qi, std::string text) {
  if (!question_active()) return false;
  const uint64_t id = live_question();
  const QuestionCard* c = card(id);
  if (!c || c->async || qi < 0 || qi >= int(c->questions.size())) return false;
  QState* st = state_for(id, *c);
  if (st->notes.size() < c->questions.size()) st->notes.resize(c->questions.size());
  while (!text.empty() && std::isspace(uint8_t(text.back()))) text.pop_back();
  st->notes[size_t(qi)] = std::move(text);
  expand_gen_++;  // the note is drawn on the card
  return true;
}

bool ChatRenderer::question_free_text(uint64_t id, int qi) {
  if (!question_live(id)) return false;
  const QuestionCard* c = card(id);
  if (!c || !c->async || qi < 0 || qi >= int(c->questions.size())) return false;
  commit_answer(id, *c, {});
  answer_.free_text = true;
  answer_.free_q = qi;
  return true;
}

bool ChatRenderer::question_choose(uint64_t id, int qi, int oi, bool confirm) {
  if (!question_live(id)) return false;
  const QuestionCard* c = card(id);
  if (!c || qi < 0 || qi >= int(c->questions.size())) return false;
  const QuestionSpec& q = c->questions[qi];
  const int n = int(q.options.size());
  if (oi < 0 || oi >= n) return false;
  QState* st = state_for(id, *c);
  st->q = qi;
  st->cursor[size_t(qi)] = oi;
  if (st->chosen[qi].size() != size_t(n)) st->chosen[qi].assign(size_t(n), 0);
  if (q.multi) {
    st->chosen[qi][size_t(oi)] ^= 1;
    if (confirm) question_advance(*st, *c, id);
    return true;
  }
  // Single-select: one answer replaces the old one and moves on.
  std::fill(st->chosen[qi].begin(), st->chosen[qi].end(), 0);
  st->chosen[qi][size_t(oi)] = 1;
  question_advance(*st, *c, id);
  return true;
}

bool ChatRenderer::question_switch_to(uint64_t id, int qi) {
  const QuestionCard* c = card(id);
  if (!c) return false;
  const int steps = int(c->questions.size()) + (card_needs_submit(*c) ? 1 : 0);
  QState* st = state_for(id, *c);
  st->q = std::clamp(qi, 0, std::max(0, steps - 1));
  return true;
}

bool ChatRenderer::question_submit(uint64_t id) {
  if (!question_live(id)) return false;
  const QuestionCard* c = card(id);
  if (!c) return false;
  QState* st = state_for(id, *c);
  // Skipping directly to Submit must not silently answer a single-select
  // question with the agent's default. Focus the first unanswered question.
  for (size_t qi = 0; qi < c->questions.size(); ++qi) {
    if (!c->questions[qi].multi &&
        std::none_of(st->chosen[qi].begin(), st->chosen[qi].end(), [](uint8_t on) { return on != 0; })) {
      st->q = int(qi);
      return true;
    }
  }
  commit_answer(id, *c, st->chosen);
  return true;
}

bool ChatRenderer::question_key(const KeyEvent& k) {
  if (!question_active()) return false;
  const uint64_t id = live_question();
  const QuestionCard* c = card(id);
  if (!c) return false;
  QState* st = state_for(id, *c);
  const int nq = int(c->questions.size());
  const int steps = nq + (card_needs_submit(*c) ? 1 : 0);
  const bool on_submit = st->q >= nq;
  const QuestionSpec* q = on_submit ? nullptr : &c->questions[size_t(st->q)];
  const int stops = cursor_stops(*c, st->q);
  switch (k.key) {
    case Key::Up:
      if (stops > 0) st->cursor[size_t(st->q)] = (st->cursor[size_t(st->q)] + stops - 1) % stops;
      return true;
    case Key::Down:
      if (stops > 0) st->cursor[size_t(st->q)] = (st->cursor[size_t(st->q)] + 1) % stops;
      return true;
    case Key::Left: st->q = std::max(0, st->q - 1); return true;
    case Key::Right: st->q = std::min(steps - 1, st->q + 1); return true;
    case Key::Tab: st->q = (st->q + 1) % steps; return true;
    case Key::BackTab: st->q = (st->q + steps - 1) % steps; return true;
    case Key::Enter:
      if (on_submit) return question_submit(id);
      if (c->async && st->cursor[size_t(st->q)] >= int(q->options.size()))
        return question_free_text(id, st->q);
      // On a multi-select question Enter toggles, like Space: pressing it on an
      // option and landing on Submit with nothing chosen reads as the choice
      // being lost. Tab or → moves on once the choices are made.
      if (q->multi) return question_choose(id, st->q, st->cursor[size_t(st->q)], false);
      return question_choose(id, st->q, st->cursor[size_t(st->q)], true);
    case Key::Char: {
      // The prompt box stays the user's while an optional question waits:
      // typing "1." there starts a message, it does not pick option one.
      if (c->async) return false;
      if (k.is(' ') && q && q->multi) {
        const int oi = st->cursor[size_t(st->q)];
        if (oi < int(q->options.size())) st->chosen[size_t(st->q)][size_t(oi)] ^= 1;
        return true;
      }
      if (!k.ctrl && !k.alt && k.ch >= '1' && k.ch <= '9' && q) {
        const int oi = int(k.ch - '1');
        if (oi < int(q->options.size())) return question_choose(id, st->q, oi, false);
      }
      return false;
    }
    default: return false;
  }
}

bool ChatRenderer::take_answer(Answer& out) {
  if (!answer_ready_) return false;
  out = std::move(answer_);
  answer_ready_ = false;
  // Choosing to answer in one's own words sends nothing yet.
  if (!out.free_text) sent_.push_back(out.tool_id);
  return true;
}

void ChatRenderer::classify(size_t prepended) {
  const size_t n = events_.size();
  const size_t appended = n - prepended - roles_.size();
  if (prepended) {
    zeros_.assign(prepended, 0);
    roles_.prepend(zeros_.data(), zeros_.data() + prepended);
    fold_zeros_.assign(prepended, 0);
    fold_of_.prepend(fold_zeros_.data(), fold_zeros_.data() + prepended);
    open_from_ += prepended;
  }
  if (appended) {
    zeros_.assign(appended, 0);
    roles_.append(zeros_.data(), zeros_.data() + appended);
    fold_zeros_.assign(appended, 0);
    fold_of_.append(fold_zeros_.data(), fold_zeros_.data() + appended);
  }
  // Events laid out already, in this numbering: a change to one is stale rows.
  const size_t laid_from = rows_from_event_ + prepended, laid_to = rows_to_event_ + prepended;
  size_t changed = SIZE_MAX;  // the last laid-out event whose role moved
  const auto set = [&](size_t i, uint8_t role, uint64_t fold) {
    if (roles_[i] == role && fold_of_[i] == fold) return;
    if (i >= laid_from && i < laid_to) changed = changed == SIZE_MAX ? i : std::max(changed, i);
    roles_[i] = role;
    fold_of_[i] = fold;
  };

  const auto blank = [&](const Event& e) {
    return arena_.view(e.text).find_first_not_of(" \t\r\n") == std::string_view::npos;
  };
  const auto silent = [&](const Event& e) {
    return e.kind == EventKind::TurnEnd || e.kind == EventKind::Meta || e.kind == EventKind::QueueAdd ||
           e.kind == EventKind::QueueTake || (e.kind == EventKind::Assistant && !e.bare && blank(e));
  };
  // Where the answer of [s, e) starts: the run of text at its end, after
  // its last step. `e` when it ends on anything else.
  const auto answer_from = [&](size_t s, size_t e) {
    size_t a = e;
    for (size_t i = e; i > s; i--) {
      const Event& ev = events_[i - 1];
      if (silent(ev)) continue;
      if (ev.kind != EventKind::Assistant) break;
      a = i - 1;
    }
    return a;
  };
  const auto finish = [&](size_t s, size_t e, bool ended) {
    const size_t a = ended ? answer_from(s, e) : e;
    const bool answered = a < e;
    size_t head = SIZE_MAX;
    uint64_t id = 0;
    if (answered)
      for (size_t i = s; i < a; i++) {
        const Event& ev = events_[i];
        // A question, a chart and a notice are said to the user: they stay.
        if (ev.kind == EventKind::Question || ev.kind == EventKind::Chart || ev.kind == EventKind::Notice ||
            (ev.kind == EventKind::Image && !ev.tool_id) || silent(ev))
          continue;
        head = i;
        // Named by where its first step is in the file, which outlives the window.
        id = kFoldBit | uint64_t(file_.line_offset(ev.src_line));
        break;
      }
    for (size_t i = s; i < e; i++) {
      const Event& ev = events_[i];
      if (answered && i >= a) {
        set(i, ev.kind == EventKind::Assistant ? kAnswer : 0, 0);
        continue;
      }
      uint8_t role = ev.kind == EventKind::Assistant ? kWork : 0;
      const bool folds = head != SIZE_MAX && i >= head && ev.kind != EventKind::Question &&
                         ev.kind != EventKind::Chart && ev.kind != EventKind::Notice &&
                         !(ev.kind == EventKind::Image && !ev.tool_id) && !silent(ev);
      if (folds) role |= kFolded | (i == head ? kFoldHead : 0);
      set(i, role, folds ? id : 0);
    }
  };
  // A turn runs from a message of yours to the next, or to the end the agent
  // records. Claude records an end after its thinking as well as after the
  // text, and codex after its final message and again when the task is
  // done: an end with no text before it ends nothing. Nor does a cancelled
  // one, whose last words were not an answer.
  //
  // Only what the new events can have changed is worked out again: older
  // history changes the turns up to the first that ends inside what was
  // there before, and new events the turn that was still open.
  size_t s = prepended ? 0 : std::min(open_from_, n);
  const size_t stop_at = prepended ? prepended : SIZE_MAX;
  bool stopped = false;
  for (size_t i = s; i < n; i++) {
    const Event& ev = events_[i];
    size_t a = i;
    if (ev.kind == EventKind::User) {
      finish(s, i, false);
    } else if (ev.kind == EventKind::TurnEnd && ev.ok && (a = answer_from(s, i)) < i) {
      finish(s, i, true);
    } else {
      continue;
    }
    s = i + 1;
    if (i >= stop_at && a >= stop_at) {
      stopped = true;
      break;
    }
  }
  if (!stopped) {
    finish(s, n, false);
    open_from_ = s;
  }

  if (changed == SIZE_MAX) return;
  if (!prepended || rows_from_event_ != 0) {
    // A turn that just ended: laid out again whole, once per turn.
    relayout_ = true;
    return;
  }
  // Older history came in at the front. Only the turn it completed can have
  // changed, and its rows are the first ones: those are laid out again, not
  // the window. Scrolling back through a long chat does this at every step.
  const uint32_t line = events_[changed].src_line;
  size_t drop = 0;
  while (drop < rows_.size() && rows_[drop].src_line <= line) drop++;
  rows_.pop_front(drop);
  size_t from = changed + 1;
  while (from < laid_to && events_[from].src_line <= line) from++;
  rows_from_event_ = from - prepended;  // in the old numbering, as ensure_rows expects
}

void ChatRenderer::strip_folded(size_t count, size_t mark) {
  const auto bare = [&](size_t i) {
    const Event& e = events_[i];
    return (roles_[i] & kFolded) && !expanded(fold_of_[i]) && e.kind != EventKind::QueueAdd &&
           e.kind != EventKind::QueueTake && e.kind != EventKind::Image;  // a picture may show in a fold
  };
  size_t i = 0;
  while (i < count && !bare(i)) i++;
  if (i == count) return;
  // What stays is copied out, the batch's text dropped, and that put back.
  std::string keep;
  std::vector<std::pair<Str*, size_t>> moved;
  for (i = 0; i < count; i++) {
    Event& e = events_[i];
    const bool drop = bare(i);
    for (Str* s : {&e.text, &e.name, &e.summary, &e.detail}) {
      if (s->len == 0 || s->off < mark) continue;
      if (drop) {
        *s = Str{0, s == &e.detail ? 1u : 0u};
        continue;
      }
      moved.push_back({s, keep.size()});
      keep.append(arena_.view(*s));
    }
    if (drop) e.bare = 1;
  }
  arena_.truncate(mark);
  const Str at = arena_.add(keep);
  for (auto& [s, off] : moved) s->off = at.off + uint32_t(off);
}

// "▸ 14 steps · 5 edits · 1 failed · 3 comments": what a folded turn did.
void ChatRenderer::layout_fold(size_t index, int w) {
  const uint64_t id = fold_of_[index];
  size_t steps = 0, edits = 0, failed = 0, comments = 0;
  for (size_t i = index; i < events_.size(); i++) {
    if (roles_[i] & kAnswer) break;
    if (fold_of_[i] != id) continue;
    const Event& e = events_[i];
    if (e.kind == EventKind::ToolCall) {
      steps++;
      if (!e.detail.empty()) edits++;
    } else if ((e.kind == EventKind::ToolResult || e.kind == EventKind::TaskStatus) && !e.ok) {
      failed++;
    } else if (e.kind == EventKind::Assistant) {
      comments++;
    }
  }
  // Only thinking: nothing to show at this density, and nothing to fold.
  if (!steps && !failed && !comments) return;
  std::string s = expanded(id) ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ";  // ▾ ▸
  const auto part = [&](size_t v, const char* one, const char* many) {
    if (!v) return;
    if (s.size() > 4) s += " \xC2\xB7 ";
    s += std::to_string(v) + " " + (v == 1 ? one : many);
  };
  part(steps, "step", "steps");
  part(edits, "edit", "edits");
  part(failed, "failed", "failed");
  part(comments, "comment", "comments");
  rows_.push_back(Row{0, 0, cur_line_, 0, 0, RowStyle::Gap, Gutter::None});
  uint32_t off = scratch_.open();
  put_oneline(scratch_, s, std::max(4, w - 2));
  emit_scratch(scratch_.close(off), RowStyle::Fold, 0, id, 0, 0xFF);
}

bool ChatRenderer::unfold_line(uint32_t line) {
  for (size_t i = 0; i < events_.size(); i++)
    if (events_[i].src_line == line && (roles_[i] & kFolded) && !expanded(fold_of_[i])) {
      toggle(fold_of_[i]);
      if (reload_) {
        reload_ = false;
        reanchor(std::min<size_t>(size_t(line) + 40, file_.line_count()));
      }
      return true;
    }
  return false;
}

void ChatRenderer::restore_anchor(uint32_t line, int into, int w, int h, const Filters& f) {
  size_t need = size_t(h + scroll_);
  for (int guard = 0; guard < 64; guard++) {
    ensure_rows(w, need, f);
    if (rows_.empty() || rows_[0].src_line < line || rows_.size() < need) break;
    need += size_t(h) * 4;
  }
  size_t idx = rows_.size();
  for (size_t i = 0; i < rows_.size(); i++)
    if (rows_[i].src_line >= line) {
      idx = i;
      break;
    }
  if (idx == rows_.size()) return;
  // Into the line's rows only while they are the same line.
  if (rows_[idx].src_line == line)
    for (int k = 0; k < into && idx + 1 < rows_.size() && rows_[idx + 1].src_line == line; k++) idx++;
  scroll_ = std::max(0, int(rows_.size()) - h - int(idx));
}

void ChatRenderer::layout_appended(int w, const Filters& f) {
  for (; rows_to_event_ < events_.size(); rows_to_event_++) {
    if (laid_out(rows_to_event_, f)) layout_event(rows_to_event_, w, f);
  }
}

void ChatRenderer::ensure_rows(int w, size_t needed, const Filters& f) {
  if (w != rows_w_ || f.density != rows_density_ || expand_gen_ != rows_gen_ ||
      math::generation() != rows_math_gen_ || links::generation() != rows_links_gen_ ||
      render_settings_generation() != rows_settings_gen_) {
    rows_links_gen_ = links::generation();
    rows_settings_gen_ = render_settings_generation();
    rows_w_ = w;
    rows_density_ = f.density;
    rows_gen_ = expand_gen_;
    // Equations laid out as images, or as Unicode, for another terminal.
    rows_math_gen_ = math::generation();
    invalidate_rows();
  }

  // Events appended by a live transcript extend the layout at the bottom.
  layout_appended(w, f);

  // Scrolling back extends it at the top. An event is laid out onto the back of
  // rows_ (that is where layout_event writes) and then moved to the front. It
  // has to be *moved*, not rotated: std::rotate touches every row in the deque,
  // so rotating once per event makes scrolling back quadratic in window size.
  //
  // Growth stops at the memory budget rather than running to the head of the
  // file. The viewport then sits at the oldest loaded row, trim_window()
  // re-anchors there, and the next scroll continues from that point — the
  // window slides through the file instead of accumulating it.
  while (rows_.size() < needed && arena_.bytes() < kArenaBudget) {
    if (rows_from_event_ == 0) {
      if (parsed_from_ == 0 && file_.complete()) {
        // Nothing older exists. The screen can still be short of rows after a
        // seek near the start of a file, where the opening lines carry no
        // conversation, so take material from the other direction instead of
        // leaving the view empty.
        if (parsed_to_ >= file_.line_count()) break;
        const size_t before_fwd = parsed_to_;
        grow_forwards(kChunkLines);
        layout_appended(w, f);
        if (parsed_to_ == before_fwd) break;
        continue;
      }
      size_t before = events_.size();
      grow_backwards();
      size_t added = events_.size() - before;
      rows_from_event_ += added;
      rows_to_event_ += added;
      // Running out of older material is decided at the top of the loop, which
      // can still fall forward instead of giving up on an empty screen.
      continue;
    }
    if (!laid_out(--rows_from_event_, f)) continue;
    const size_t at = rows_.size();
    layout_event(rows_from_event_, w, f);
    const size_t added = rows_.size() - at;
    if (added) {
      moved_.assign(rows_.begin() + long(at), rows_.end());
      rows_.resize(at);
      rows_.prepend(moved_.data(), moved_.data() + moved_.size());
    }
  }
}

// Re-anchors the window at whatever is on screen and throws the rest away. The
// viewport is unchanged afterwards: the re-seeded window ends at the line the
// viewport ended on, so scroll_ returns to zero meaning "bottom of the window".
void ChatRenderer::trim_window() {
  if (arena_.bytes() < kArenaBudget || rows_.empty()) return;

  const int total = int(rows_.size());
  const int bottom = std::clamp(total - scroll_ - 1, 0, total - 1);
  const uint32_t keep_line = rows_[size_t(bottom)].src_line;

  reanchor(size_t(keep_line) + 1);
  trims_++;
  file_.release_pages();
}

void ChatRenderer::reanchor(size_t line) {
  events_.clear();
  roles_.clear();
  fold_of_.clear();
  open_from_ = 0;
  arena_.clear();
  pending_.clear();
  questions_all_.clear();
  agent_queue_.clear();
  sent_.clear();
  pending_tools_.clear();
  chart_tools_.clear();
  qstate_.clear();
  async_.clear();
  relayout_ = false;
  answer_ready_ = false;
  invalidate_rows();
  // Keep state_: it describes the session, not the window into it.
  parsed_from_ = parsed_to_ = std::min(line, file_.line_count());
  scroll_ = 0;
}

void ChatRenderer::index_back_to(size_t byte) {
  // Every slab the index adds shifts the line numbers the window is holding.
  while (!file_.complete() && file_.indexed_from() > byte) {
    const size_t added = file_.extend_back();
    if (added == 0) break;
    shift_lines(added);
    parsed_from_ += added;
    parsed_to_ += added;
  }
}

// ------------------------------------------------------------------ outline

void ChatRenderer::outline_scan(size_t a, size_t b, std::vector<OutlineEntry>& into) {
  Arena tmp;
  std::vector<Event> evs;
  file_.will_read(a, b);
  for (size_t i = a; i < b; i++) {
    tmp.clear();
    evs.clear();
    adapter_->parse(file_.line(i), tmp, evs);
    const uint64_t at = file_.line_offset(i);
    for (const Event& e : evs) {
      OutlineEntry o;
      o.offset = at;
      switch (e.kind) {
        case EventKind::User: {
          o.kind = 'u';
          o.label = text::oneline(tmp.view(e.text), 200);
          if (o.label.empty()) continue;
          break;
        }
        case EventKind::ToolCall: {
          const std::string_view name = tmp.view(e.name), summary = tmp.view(e.summary);
          std::string label = text::oneline(summary.empty() ? name : summary, 160);
          if (e.tool_id) {
            if (outline_calls_.size() > 50'000) outline_calls_.clear();
            outline_calls_[e.tool_id] = std::string(name) + "\t" + label;
            // A failure read before its call (history read backwards) gets
            // its name now.
            if (auto it = outline_orphans_.find(e.tool_id); it != outline_orphans_.end()) {
              for (auto* list : {&into, &outline_})
                for (auto& x : *list)
                  if (x.offset == it->second && x.kind == 'x' && x.label.empty()) {
                    x.label = label;
                    x.detail = std::string(name) + " failed";
                  }
              outline_orphans_.erase(it);
            }
          }
          if (classify_tool(name, summary, nullptr, nullptr) != ToolKind::Edit) continue;
          o.kind = 'e';
          o.label = std::move(label);
          o.detail = std::string(name);
          break;
        }
        case EventKind::ToolResult: {
          if (e.ok) continue;
          o.kind = 'x';
          if (auto it = outline_calls_.find(e.tool_id); it != outline_calls_.end()) {
            const size_t tab = it->second.find('\t');
            o.label = it->second.substr(tab + 1);
            o.detail = it->second.substr(0, tab) + " failed";
          } else {
            outline_orphans_[e.tool_id] = at;
          }
          break;
        }
        case EventKind::Question: {
          o.kind = 'q';
          QuestionCard card;
          parse_question_card(tmp.view(e.detail), card);
          if (!card.questions.empty()) {
            o.label = text::oneline(card.questions[0].text, 200);
            o.detail = card.questions[0].header;
          }
          if (o.label.empty()) o.label = text::oneline(tmp.view(e.summary).empty() ? tmp.view(e.name) : tmp.view(e.summary), 200);
          if (o.detail.empty()) o.detail = "question";
          break;
        }
        case EventKind::Notice: {
          o.kind = 'n';
          o.label = text::oneline(tmp.view(e.text), 200);
          if (o.label.empty()) continue;
          break;
        }
        default: continue;
      }
      into.push_back(std::move(o));
    }
  }
}

bool ChatRenderer::outline(std::vector<OutlineEntry>& out, int budget_ms) {
  out.clear();
  if (!file_.is_open() || !adapter_) return true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  // Line numbers are only stable once the index covers the file; indexing is
  // a newline scan, far cheaper than the parsing below.
  index_back_to(0);
  const size_t n = file_.line_count();
  if (n == 0) return true;
  auto first_at = [&](uint64_t byte) {  // the first line starting at or after `byte`
    size_t i = file_.line_at_byte(size_t(byte));
    while (i < n && file_.line_offset(i) < byte) i++;
    return i;
  };
  const uint64_t end = file_.line_offset(n - 1) + file_.line(n - 1).size() + 1;
  if (outline_lo_ == UINT64_MAX) outline_lo_ = outline_hi_ = end;
  // What was written since the last look.
  if (outline_hi_ < end) {
    outline_scan(first_at(outline_hi_), n, outline_);
    outline_hi_ = end;
  }
  // Then older history, a slab at a time, newest slab first.
  constexpr size_t kSlab = 2000;
  while (outline_lo_ > 0 && std::chrono::steady_clock::now() < deadline) {
    const size_t e = first_at(outline_lo_);
    const size_t a = e > kSlab ? e - kSlab : 0;
    std::vector<OutlineEntry> older;
    outline_scan(a, e, older);
    outline_.insert(outline_.begin(), std::make_move_iterator(older.begin()), std::make_move_iterator(older.end()));
    outline_lo_ = a == 0 ? 0 : file_.line_offset(a);
  }
  out = outline_;
  for (auto& o : out)
    if (o.kind == 'x' && o.label.empty()) {
      o.label = "a tool call";
      o.detail = "failed";
    }
  return outline_lo_ == 0;
}

bool ChatRenderer::user_text_at(uint64_t offset, std::string* out) {
  if (!file_.is_open() || !adapter_) return false;
  index_back_to(size_t(offset));
  const size_t n = file_.line_count();
  if (n == 0) return false;
  size_t i = file_.line_at_byte(size_t(offset));
  while (i < n && file_.line_offset(i) < offset) i++;
  if (i >= n) return false;
  Arena tmp;
  std::vector<Event> evs;
  adapter_->parse(file_.line(i), tmp, evs);
  for (const Event& e : evs) {
    if (e.kind != EventKind::User) continue;
    std::string t(tmp.view(e.text));
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    if (t.empty()) return false;
    *out = std::move(t);
    return true;
  }
  return false;
}

uint64_t ChatRenderer::view_offset() const {
  if (!file_.is_open() || file_.line_count() == 0) return 0;
  const uint32_t line = viewport_line(last_h_);
  return file_.line_offset(std::min<size_t>(line, file_.line_count() - 1));
}

void ChatRenderer::seek_fraction(double f) {
  if (!file_.is_open() || file_.size_bytes() == 0) return;
  f = std::clamp(f, 0.0, 1.0);
  const size_t target = size_t(f * double(file_.size_bytes()));
  index_back_to(target);
  reanchor(file_.line_at_byte(target) + 1);
}

void ChatRenderer::set_find_query(std::string q) {
  if (q == find_q_) return;
  find_q_ = std::move(q);
  find_fold_ = text::fold(find_q_);
  find_done_ = false;
  find_lines_.clear();
  find_tools_.clear();
  find_cur_ = -1;
  find_line_ = UINT32_MAX;
  pending_find_ = 0;
}

void ChatRenderer::clear_find() { set_find_query({}); }

void ChatRenderer::reveal(uint64_t offset, std::string query) {
  set_find_query(std::move(query));
  pending_reveal_ = offset;
}

void ChatRenderer::compute_matches(const Filters& f) {
  index_back_to(0);
  find_lines_.clear();
  find_tools_.clear();
  Arena tmp;
  std::vector<Event> evs;
  const SearchScope scope{f.show_thinking(), f.show_tools(), f.show_meta()};
  const size_t n = file_.line_count();
  constexpr size_t kSlab = 4096;
  for (size_t i = 0; i < n; i++) {
    if (i % kSlab == 0) file_.will_read(i, std::min(n, i + kSlab));
    const std::string_view line = file_.line(i);
    // The raw line first: most lines are rejected by one scan, unparsed.
    if (text::find_folded(line, find_fold_) == std::string_view::npos) continue;
    tmp.clear();
    evs.clear();
    adapter_->parse(line, tmp, evs);
    for (const Event& e : evs) {
      if (!in_scope(e.kind, scope)) continue;
      const auto has = [&](Str s) {
        return text::find_folded(tmp.view(s), find_fold_) != std::string_view::npos;
      };
      const bool shown = has(e.text) || has(e.summary) || has(e.name);
      if (!shown && !has(e.detail)) continue;
      // Hidden until its call is opened: a collapsed result, a diff, and the
      // rest of a command a collapsed call cuts to one line.
      const bool folded = (e.kind == EventKind::ToolResult && !f.show_results()) ||
                          e.kind == EventKind::ToolCall;
      find_lines_.push_back(uint32_t(i));
      find_tools_.push_back(folded ? e.tool_id : 0);
      break;
    }
  }
  find_done_ = true;
  find_upto_ = n;
}

uint32_t ChatRenderer::viewport_line(int h) const {
  if (rows_.empty()) return uint32_t(parsed_to_);
  const int total = int(rows_.size());
  const int first = std::max(0, total - h - scroll_);
  return rows_[size_t(std::min(total - 1, first + h / 2))].src_line;
}

void ChatRenderer::show_line(uint32_t line, int w, int h, const Filters& f) {
  if (line < parsed_from_ || line >= parsed_to_)
    reanchor(std::min<size_t>(size_t(line) + 40, file_.line_count()));
  // Lay out older rows until the line's first row is in, or nothing is left.
  size_t need = size_t(h);
  for (int guard = 0; guard < 64; guard++) {
    ensure_rows(w, need, f);
    // A line in a folded turn is shown by opening the fold.
    if (folding(f) && unfold_line(line)) continue;
    if (!rows_.empty() && rows_[0].src_line < line) break;
    if (rows_.size() < need) break;
    need += size_t(h) * 4;
  }
  size_t idx = rows_.size();
  for (size_t i = 0; i < rows_.size(); i++)
    if (rows_[i].src_line >= line) { idx = i; break; }
  if (idx == rows_.size()) return;
  // In a long message, the row the match is on, not the message's first.
  if (!find_fold_.empty())
    for (size_t i = idx; i < rows_.size() && rows_[i].src_line == line; i++) {
      row_text_.clear();
      for (uint16_t k = 0; k < rows_[i].seg_count; k++) row_text_ += seg_text(segs_[rows_[i].seg_first + k]);
      if (text::find_folded(row_text_, find_fold_) != std::string_view::npos) { idx = i; break; }
    }
  const int first = std::max(0, int(idx) - h / 3);
  scroll_ = std::max(0, int(rows_.size()) - h - first);
}

void ChatRenderer::resolve_moves(int w, int h, const Filters& f) {
  if (pending_reveal_ != UINT64_MAX) {
    const size_t byte = size_t(pending_reveal_);
    pending_reveal_ = UINT64_MAX;
    // Count the chat's matches now, so the bar reads "3 of 17" from the start
    // and the next step goes on from this one. Counting indexes the whole
    // file, which renumbers lines: the line is looked up only after it.
    if (!find_fold_.empty()) compute_matches(f);
    else index_back_to(byte);
    find_line_ = uint32_t(file_.line_at_byte(byte));
    if (!find_fold_.empty()) {
      const auto it = std::lower_bound(find_lines_.begin(), find_lines_.end(), find_line_);
      find_cur_ = it != find_lines_.end() && *it == find_line_ ? int(it - find_lines_.begin()) : -1;
      if (find_cur_ >= 0)
        if (const uint64_t tool = find_tools_[size_t(find_cur_)]; tool && !expanded(tool)) toggle(tool);
    }
    show_line(find_line_, w, h, f);
  }
  if (pending_find_) {
    const int dir = pending_find_;
    pending_find_ = 0;
    if (!find_done_ || find_upto_ != file_.line_count()) {
      const uint32_t keep = find_cur_ >= 0 ? find_lines_[size_t(find_cur_)] : UINT32_MAX;
      compute_matches(f);
      find_cur_ = -1;
      if (keep != UINT32_MAX) {
        const auto it = std::lower_bound(find_lines_.begin(), find_lines_.end(), keep);
        if (it != find_lines_.end() && *it == keep) find_cur_ = int(it - find_lines_.begin());
      }
    }
    const int n = int(find_lines_.size());
    if (n == 0) {
      find_cur_ = -1;
      find_line_ = UINT32_MAX;
    } else {
      if (find_cur_ < 0) {
        // The first step goes from what is on screen, not from either end.
        const uint32_t at = viewport_line(h);
        if (dir < 0) {
          const auto it = std::upper_bound(find_lines_.begin(), find_lines_.end(), at);
          find_cur_ = it == find_lines_.begin() ? n - 1 : int(it - find_lines_.begin()) - 1;
        } else {
          const auto it = std::upper_bound(find_lines_.begin(), find_lines_.end(), at);
          find_cur_ = it == find_lines_.end() ? 0 : int(it - find_lines_.begin());
        }
      } else {
        find_cur_ = (find_cur_ + dir + n) % n;
      }
      find_line_ = find_lines_[size_t(find_cur_)];
      const uint64_t tool = find_tools_[size_t(find_cur_)];
      if (tool && !expanded(tool)) toggle(tool);
      show_line(find_line_, w, h, f);
    }
  }
  if (pending_user_) {
    const int dir = pending_user_;
    pending_user_ = 0;
    const auto starts_user = [&](size_t i) {
      return rows_[i].gutter == Gutter::User && (i == 0 || rows_[i - 1].gutter != Gutter::User);
    };
    int first = std::max(0, int(rows_.size()) - h - scroll_);
    long target = -1;
    if (dir < 0) {
      for (int guard = 0; guard < 256 && target < 0; guard++) {
        for (long i = long(first) - 1; i >= 0; i--)
          if (starts_user(size_t(i))) { target = i; break; }
        if (target >= 0) break;
        // Nothing above: lay out older history and look again.
        const size_t before = rows_.size();
        ensure_rows(w, rows_.size() + size_t(h) * 4, f);
        if (rows_.size() == before) break;
        first += int(rows_.size() - before);
      }
      if (target < 0) target = 0;  // the top: no older message
    } else {
      // The message a previous jump put at the top sits one row down.
      for (size_t i = size_t(first) + 2; i < rows_.size(); i++)
        if (starts_user(i)) { target = long(i); break; }
    }
    if (target < 0) {
      scroll_ = 0;
    } else {
      const int top = std::max(0, int(target) - 1);  // one row of context above
      scroll_ = std::max(0, int(rows_.size()) - h - top);
    }
  }
}

void ChatRenderer::seek_from_bar(int thumb_top) {
  const int span = std::max(1, last_h_ - bar_h_);
  seek_fraction(double(std::clamp(thumb_top, 0, span)) / double(span));
}

void ChatRenderer::render(Painter& p, const Theme& th, const Filters& f) {
  last_h_ = p.height();
  p.clear(Style{th.text, th.panel});

  if (!file_.is_open()) {
    p.text(1, 0, "no transcript yet", Style{th.dim, th.panel});
    return;
  }
  if (!adapter_) {
    p.text(1, 0, "no adapter for this agent — raw view only", Style{th.dim, th.panel});
    return;
  }

  const int w = std::max(4, p.width() - 3);
  // Last frame's draft comes off before anything lays out: new rows go under
  // the transcript, and the draft goes back under them.
  const size_t draft_before = draft_rows_;
  drop_draft_rows();
  check_chart_files();
  // A relayout under someone reading back through the chat, or a fold opened
  // or closed, keeps the line at the top of the view where it was; one at
  // the bottom, a turn folding as it ends, stays at the bottom.
  const bool toggled = expand_gen_ != rows_gen_ && w == rows_w_ && f.density == rows_density_;
  uint32_t anchor = UINT32_MAX;
  int anchor_into = 0;
  if ((relayout_ && scroll_ > 0) || toggled || reload_) {
    const int total = int(rows_.size());
    const int top = std::max(0, total - p.height() - scroll_);
    if (top < total) {
      anchor = rows_[size_t(top)].src_line;
      for (int i = top; i > 0 && rows_[size_t(i - 1)].src_line == anchor; i--) anchor_into++;
    }
  }
  if (reload_) {
    reload_ = false;
    reanchor(parsed_to_);
  }
  if (relayout_) {
    relayout_ = false;
    invalidate_rows();
  }
  if (anchor != UINT32_MAX) restore_anchor(anchor, anchor_into, w, p.height(), f);
  ensure_rows(w, size_t(p.height() + scroll_), f);
  // Resolve after loading the lazy transcript window, including on the first
  // frame. When the call finishes, its history row becomes visible again.
  uint64_t activity_tool = 0;
  if (activity_bar_) in_flight_tool(nullptr, nullptr, &activity_tool);
  if (activity_tool != activity_tool_) {
    activity_tool_ = activity_tool;
    invalidate_rows();
    ensure_rows(w, size_t(p.height() + scroll_), f);
  }

  resolve_moves(w, p.height(), f);
  layout_draft(w);
  // Read back through the chat, the view stays put while the draft grows.
  if (scroll_ > 0) scroll_ = std::max(0, scroll_ + int(draft_rows_) - int(draft_before));

  const int total = int(rows_.size());
  scroll_ = std::clamp(scroll_, 0, std::max(0, total - p.height()));
  // A conversation shorter than the pane starts at the top and grows down —
  // the opening messages should read from the first line, not float against
  // the bottom edge.
  const int first = std::max(0, total - p.height() - scroll_);
  last_pad_ = 0;

  question_hits_.clear();
  link_hits_.clear();
  Style cache[kRowStyles];
  for (int i = 0; i < kRowStyles; i++) cache[i] = base_style(i, th);

  for (int row = 0; row < p.height(); row++) {
    size_t i = size_t(first + row);
    if (i >= rows_.size()) break;
    const Row& r = rows_[i];

    // A user turn is a block, so its tint runs the full width of the pane —
    // a background that stops at the end of the text reads as ragged
    // highlighting rather than as a surface.
    const int ry = row;
    const bool tinted = r.gutter == Gutter::User;
    const Color bg = tinted ? th.user_bg : th.panel;
    if (tinted) p.fill(Rect{1, ry, p.width() - 2, 1}, Style{th.text, bg});
    if (r.gutter == Gutter::User) p.put(1, ry, U'▌', Style{th.user, bg, attr::kBold});
    if (r.gutter == Gutter::Answer) p.put(1, ry, U'▎', Style{th.accent, bg, attr::kBold});

    // A tool call's bullet is drawn here: a spinner while it waits on its
    // result, otherwise the expand/collapse marker. Keeping it out of the
    // laid-out text lets the animation run without re-laying-out anything.
    const bool in_flight = working_ && r.base == RowStyle::Tool && tool_pending(r.tool_id);
    if (r.base == RowStyle::Tool && r.tool_id) {
      const char32_t mark =
          in_flight ? spin_ : (expanded(r.tool_id) ? U'\u25BE' : U'\u25B8');
      p.put(2, ry, mark, Style{in_flight ? th.working : th.tool, bg, attr::kBold});
    }

    // A question option paints its own cursor and check from the live state, so
    // moving the choice never has to re-lay-out the transcript.
    const bool question_editable = question_live(r.tool_id);
    bool option_on = false;  // a chosen option, whose label is lit as well as checked
    const bool question_sent = (has_pending_question() || async_slot(r.tool_id)) &&
        std::find(sent_.begin(), sent_.end(), r.tool_id) != sent_.end();
    if (r.tool_id && r.opt != 0xFF && (question_editable || question_sent)) {
      const QState* st = qstate_of(r.tool_id);
      const QuestionCard* qc = card(r.tool_id);
      if (st && qc) {
        const int nq = int(qc->questions.size());
        if (question_editable)
          question_hits_.push_back(QHit{Rect{0, ry, w, 1}, r.tool_id, r.q, r.opt});
        if (r.opt == 0xFD) {
          // A question header: mark the one currently being answered.
          if (question_editable && st->q == r.q) p.put(0, ry, U'\u25B8', Style{th.accent, bg, attr::kBold});
        } else if (r.opt == 0xFE) {
          if (question_editable && st->q >= nq) p.put(0, ry, U'\u276F', Style{th.accent, bg, attr::kBold});
        } else if (r.opt == 0xFC) {
          // The own-words row sits one stop past the last option.
          if (question_editable && st->q == r.q && r.q < st->cursor.size() && r.q < nq &&
              st->cursor[r.q] == int(qc->questions[r.q].options.size()))
            p.put(0, ry, U'\u276F', Style{th.accent, bg, attr::kBold});
        } else if (r.q < nq) {
          const bool multi = qc->questions[r.q].multi;
          const bool on = r.opt < st->chosen[r.q].size() && st->chosen[r.q][r.opt];
          option_on = on;
          // A wrapped option's later rows light up with it but carry no
          // cursor or check of their own.
          if (!r.cont) {
            if (question_editable && st->q == r.q && r.q < st->cursor.size() && st->cursor[r.q] == r.opt)
              p.put(0, ry, U'\u276F', Style{th.accent, bg, attr::kBold});
            const char32_t mark =
                multi ? (on ? U'\u2611' : U'\u2610') : (on ? U'\u25C9' : U'\u25CB');
            p.put(2, ry, mark,
                  Style{on ? th.accent : th.dim, bg, on ? attr::kBold : attr::kNone});
          }
        }
      }
    }

    // A code block's rows share one surface, from its indent to the edge.
    if (r.code) p.fill(Rect{r.indent + 2, ry, std::max(0, p.width() - 1 - (r.indent + 2)), 1},
                       Style{th.code_text, th.code_bg});
    // A diff's added and removed lines, tinted across.
    const Color tint_bg = r.tint == 1 ? th.added_bg : r.tint == 2 ? th.removed_bg : bg;
    if (r.tint) p.fill(Rect{r.indent + 2, ry, std::max(0, p.width() - 1 - (r.indent + 2)), 1}, Style{th.text, tint_bg});
    int x = r.indent + 2;
    for (uint16_t k = 0; k < r.seg_count; k++) {
      const md::Seg& sg = segs_[r.seg_first + k];
      if (x >= p.width() - 1) break;
      // An equation's row: placeholder cells naming the image and the cell
      // within it. The terminal draws the picture; clipping at the pane's edge
      // just leaves its right side off.
      if (uint32_t id; sg.ink == md::Ink::MathImage) {
        int irow, icols;
        if (md::image_ref(scratch_, sg, &id, &irow, &icols)) {
          const Style ist{Color(id), bg, attr::kImage};
          for (int c = 0; c < icols && x + c < p.width() - 1; c++)
            p.put(x + c, ry, char32_t(uint32_t(irow) << 16 | uint32_t(c)), ist);
          x += icols;
          continue;
        }
      }
      Style st = ink_style(cache[int(r.base)], sg.ink, th);
      if (in_flight) st.fg = th.working;  // an executing tool reads as live
      // A check glyph alone is too small to scan a list by, and some fonts
      // draw ☐ and ☑ nearly alike.
      if (option_on) { st.fg = th.accent; st.a |= attr::kBold; }
      // Code blocks have their own surface; inline code is a chip of it.
      st.bg = r.code || sg.ink == md::Ink::Code ? th.code_bg : bg;
      if (r.tint) {
        st.a &= uint16_t(~attr::kItalic);  // code, not an aside: upright, whatever the row's style
        st.bg = (sg.attr & md::kAttrStrong) ? (r.tint == 1 ? th.added_strong : th.removed_strong) : tint_bg;
        if (st.fg == th.dim && sg.ink >= md::Ink::CodeText && sg.ink <= md::Ink::CodeMark) st.fg = th.code_text;
      }
      // A block's padding, wrap marks and label are not code: a selection
      // skips them, and a wrap mark joins its row back onto the one above.
      if (sg.ink == md::Ink::CodeMark) {
        st.a |= attr::kDecor;
        if (seg_text(sg) == "\xE2\x86\xAA") st.a |= attr::kJoin;
      }
      if (sg.attr) {
        if (sg.attr & md::kAttrBold) st.a |= attr::kBold;
        if (sg.attr & md::kAttrDim) st.a |= attr::kDim;
        if (sg.attr & md::kAttrItalic) st.a |= attr::kItalic;
        if (sg.attr & md::kAttrUnderline) st.a |= attr::kUnderline;
        if (sg.attr & md::kAttrStrike) st.a |= attr::kStrike;
      }
      if (sg.link) {
        // A link in code keeps its colour; the underline says it is one.
        st.a |= attr::kUnderline;
        p.set_link(sg.link);
      }
      const int drawn = p.text_clipped(x, ry, seg_text(sg), st, p.width() - 1 - x);
      if (sg.link) {
        p.set_link(0);
        link_hits_.push_back(LinkHit{Rect{x, ry, drawn, 1}, sg.link});
      }
      x += drawn;
    }
    // Search matches, lit over the text just drawn; the current one harder.
    if (!find_fold_.empty() && r.seg_count) {
      row_text_.clear();
      for (uint16_t k = 0; k < r.seg_count; k++) row_text_ += seg_text(segs_[r.seg_first + k]);
      const std::string_view rt = row_text_;
      const bool current = r.src_line == find_line_;
      const Style lit{th.bg, current ? th.attention : th.accent, attr::kBold};
      for (size_t at = 0; (at = text::find_folded(rt, find_fold_, at)) != std::string_view::npos;
           at += find_fold_.size()) {
        const int mx = r.indent + 2 + text::str_width(rt.substr(0, at));
        if (mx >= p.width() - 1) break;
        p.text_clipped(mx, ry, rt.substr(at, find_fold_.size()), lit, p.width() - 1 - mx);
      }
    }
  }

  // Position is reported against the file, not against the parsed window. With
  // tail-first indexing the window is a sliver of a large transcript, and a bar
  // measured against it claims you are at the top when you are at the end.
  const bool fits = file_.complete() && parsed_from_ == 0 &&
                    parsed_to_ == file_.line_count() && total <= p.height();
  if (!fits && p.width() > 2 && !rows_.empty() && file_.size_bytes() > 0) {
    const size_t bytes = file_.size_bytes();
    const uint32_t top_line = rows_[size_t(first)].src_line;
    const uint32_t bot_line = rows_[size_t(std::min(total - 1, first + p.height() - 1))].src_line;
    const double a = double(file_.line_offset(top_line)) / double(bytes);
    const double b = double(file_.line_offset(size_t(bot_line) + 1)) / double(bytes);

    const int h = p.height();
    const int bar_h = std::max(1, int((b - a) * h + 0.5));
    // Positioned across the travel the thumb actually has, so that dragging it
    // to a spot and reading the spot back agree.
    const int bar_y = std::clamp(int(a * double(h - bar_h) + 0.5), 0, std::max(0, h - bar_h));
    thumb_frac_ = a;
    bar_col_ = p.width() - 1;
    bar_y_ = bar_y;
    bar_h_ = bar_h;

    const Color thumb = dragging_ ? th.accent : th.dim;
    for (int y = 0; y < h; y++) {
      const bool on = y >= bar_y && y < bar_y + bar_h;
      p.put(bar_col_, y, on ? U'▐' : U'│', Style{on ? thumb : th.border, th.panel});
    }
  } else {
    bar_col_ = -1;
  }

  trim_window();
}

// A model id carries its vendor for machines, not for readers: the strip is
// short and "opus-5" is as unambiguous here as "claude-opus-5".
static std::string_view short_value(std::string_view key, std::string_view v) {
  if (key != "model") return v;
  for (std::string_view prefix : {"claude-", "anthropic/", "openai/"})
    if (v.size() > prefix.size() && v.compare(0, prefix.size(), prefix) == 0)
      return v.substr(prefix.size());
  return v;
}

void ChatRenderer::render_chips(Painter& p, const Theme& th, std::vector<Chip>& hits) const {
  hits.clear();
  // A background of its own, not the chat's: this strip is controls, not
  // conversation, and the two must never read as one continuous block.
  p.clear(Style{th.dim, th.strip_bg});
  if (state_.fields.empty()) return;

  int x = 1;
  for (size_t i = 0; i < state_.fields.size(); i++) {
    const StateField& f = state_.fields[i];
    const std::string_view value =
        f.value.empty() ? std::string_view("?") : short_value(f.key, f.value);

    if (i) {
      if (x + 3 >= p.width()) break;
      p.text(x, 0, " \xC2\xB7 ", Style{th.border, th.strip_bg});
      x += 3;
    }

    // "label value ▾" — the caret is the affordance; without it a chip reads
    // as decoration rather than a control.
    const int wide = text::str_width(f.label) + 1 + text::str_width(value) + 2;
    if (x + wide > p.width()) break;

    const int start = x;
    x += p.text(x, 0, f.label, Style{th.dim, th.strip_bg}) + 1;
    x += p.text(x, 0, value, Style{th.text, th.strip_bg, attr::kBold}) + 1;
    x += p.text(x, 0, "\xE2\x96\xBE", Style{th.accent, th.strip_bg});
    hits.push_back(Chip{Rect{start, 0, x - start, 1}, f.key});
  }
}

bool ChatRenderer::on_key(const KeyEvent& k) {
  // Alt/Ctrl+↑↓: to the previous or next user message.
  if ((k.alt || k.ctrl) && !k.shift && (k.key == Key::Up || k.key == Key::Down)) {
    jump_user(k.key == Key::Up ? -1 : 1);
    return true;
  }
  switch (k.key) {
    case Key::Up: scroll_ += 1; return true;
    case Key::Down: scroll_ = std::max(0, scroll_ - 1); return true;
    case Key::PageUp: scroll_ += std::max(1, last_h_ - 2); return true;
    case Key::PageDown: scroll_ = std::max(0, scroll_ - std::max(1, last_h_ - 2)); return true;
    case Key::End: scroll_ = 0; return true;
    default: break;
  }
  if (k.is('k')) { scroll_ += 1; return true; }
  if (k.is('j')) { scroll_ = std::max(0, scroll_ - 1); return true; }
  return false;
}

uint64_t ChatRenderer::tool_at(int y) const {
  const int total = int(rows_.size());
  const int first = std::max(0, total - last_h_ - scroll_);
  const size_t i = size_t(first + y - last_pad_);
  if (y < last_pad_ || i >= rows_.size()) return 0;
  return rows_[i].tool_id;
}

bool ChatRenderer::on_mouse(const MouseEvent& m, Point local) {
  if (dragging_) {
    if (m.kind == MouseKind::Drag || m.kind == MouseKind::Move) {
      seek_from_bar(local.y - grab_dy_);
      return true;
    }
    dragging_ = false;  // release, or anything else, ends the drag
    if (m.kind == MouseKind::Release) return true;
  }

  // The scrollbar column: grab the thumb, or jump to where the track was hit.
  if (m.kind == MouseKind::Press && m.button == MouseButton::Left && bar_col_ >= 0 &&
      local.x == bar_col_) {
    dragging_ = true;
    if (local.y >= bar_y_ && local.y < bar_y_ + bar_h_) {
      grab_dy_ = local.y - bar_y_;  // keep the grip point under the cursor
    } else {
      grab_dy_ = bar_h_ / 2;  // a track click centres the thumb on the cursor
      seek_from_bar(local.y - grab_dy_);
    }
    return true;
  }

  // A click on a question option chooses it. Read-only cards are not in
  // question_hits_ at all, so the click still activates whatever is behind.
  if (m.kind == MouseKind::Press && m.button == MouseButton::Left) {
    for (const auto& h : question_hits_) {
      if (!h.rect.contains(local)) continue;
      if (!question_live(h.tool_id)) return true;
      if (h.opt == 0xFD) { question_switch_to(h.tool_id, h.q); return true; }
      if (h.opt == 0xFE) { question_submit(h.tool_id); return true; }
      if (h.opt == 0xFC) { question_free_text(h.tool_id, h.q); return true; }
      const QuestionCard* qc = card(h.tool_id);
      const bool multi = qc && h.q < int(qc->questions.size()) && qc->questions[h.q].multi;
      question_choose(h.tool_id, h.q, int(h.opt), !multi);
      return true;
    }
  }

  if (m.kind == MouseKind::WheelUp) { scroll_ += 3; return true; }
  if (m.kind == MouseKind::WheelDown) { scroll_ = std::max(0, scroll_ - 3); return true; }
  const auto link_at = [&](Point pt) -> uint16_t {
    for (const auto& h : link_hits_)
      if (h.rect.contains(pt)) return h.link;
    return 0;
  };
  if (m.kind == MouseKind::Press && m.button == MouseButton::Left) {
    press_y_ = local.y;
    press_tool_ = tool_at(local.y);
    press_link_ = link_at(local);
    return true;
  }
  if (m.kind == MouseKind::Release && m.button == MouseButton::Left) {
    // A click (press and release on the same link) opens it. A drag across
    // one is a selection, and never gets here: the app swallows its release.
    if (press_link_ && link_at(local) == press_link_) {
      open_url_ = std::string(links::url(press_link_));
      press_link_ = 0;
      press_tool_ = 0;
      press_y_ = -1;
      return true;
    }
    const uint64_t t = tool_at(local.y);
    if (t && t == press_tool_ && local.y == press_y_) {
      // On a JSON container's opening line: open or close just that.
      const int total = int(rows_.size());
      const size_t i = size_t(std::max(0, total - last_h_ - scroll_) + local.y - last_pad_);
      if (i < rows_.size() && rows_[i].node) {
        auto& flips = json_flips_[t];
        const int id = rows_[i].node - 1;
        if (!flips.erase(id)) flips.insert(id);
        expand_gen_++;  // laid out again, the view held where it is
      } else {
        toggle(t);
      }
    }
    press_tool_ = 0;
    press_y_ = -1;
    press_link_ = 0;
    return true;
  }
  return false;
}

std::vector<MenuItem> ChatRenderer::context_menu(Point local, const Filters& f) {
  uint64_t clicked = 0;
  int total = int(rows_.size());
  int first = std::max(0, total - last_h_ - scroll_);
  size_t i = size_t(first + local.y - last_pad_);
  if (local.y >= last_pad_ && i < rows_.size()) {
    clicked = rows_[i].tool_id;
    menu_line_ = rows_[i].src_line;
  }

  std::vector<MenuItem> items;
  items.push_back(MenuItem{"Copy this message", "copy"});
  items.push_back(MenuItem::sep());
  if (clicked) {
    char buf[32];
    snprintf(buf, sizeof buf, "toggle:%llu", (unsigned long long)clicked);
    const bool fold = clicked & kFoldBit;
    items.push_back(MenuItem{expanded(clicked) ? (fold ? "Fold these steps" : "Collapse this tool call")
                                               : (fold ? "Show these steps" : "Expand this tool call"),
                             buf});
  }
  items.push_back(MenuItem{"Minimal — chat only", "density:0", true, false,
                           f.density == Density::Minimal});
  items.push_back(MenuItem{"Normal — + tool calls", "density:1", true, false,
                           f.density == Density::Normal});
  items.push_back(MenuItem{"Full — + thinking, output", "density:2", true, false,
                           f.density == Density::Full});
  items.push_back(MenuItem::sep());
  items.push_back(MenuItem{"Expand all tools", "expand_all"});
  items.push_back(MenuItem{"Collapse all tools", "collapse_all"});
  items.push_back(MenuItem{"Jump to latest", "bottom"});
  return items;
}

bool ChatRenderer::on_action(const std::string& a, Filters& f, std::string* copy_out) {
  if (a == "copy") {
    // Everything the clicked line came from, not just the wrapped row: a
    // clipboard full of hard-wrapped fragments is useless.
    if (copy_out) {
      copy_out->clear();
      for (const auto& e : events_) {
        if (e.src_line != menu_line_ || e.bare) continue;
        if (!copy_out->empty()) copy_out->push_back('\n');
        copy_out->append(arena_.view(e.text.empty() ? e.summary : e.text));
      }
    }
    return true;
  }
  if (a.rfind("density:", 0) == 0) {
    f.density = Density(a[8] - '0');
    return true;
  }
  if (a.rfind("toggle:", 0) == 0) {
    toggle(strtoull(a.c_str() + 7, nullptr, 10));
    return true;
  }
  if (a == "expand_all") {
    for (const auto& e : events_)
      if (e.kind == EventKind::ToolCall && e.tool_id) expanded_.push_back(e.tool_id);
    std::sort(expanded_.begin(), expanded_.end());
    expanded_.erase(std::unique(expanded_.begin(), expanded_.end()), expanded_.end());
    expand_gen_++;
    return true;
  }
  if (a == "collapse_all") { expanded_.clear(); expand_gen_++; return true; }
  if (a == "bottom") { scroll_ = 0; return true; }
  return false;
}

}  // namespace mico
