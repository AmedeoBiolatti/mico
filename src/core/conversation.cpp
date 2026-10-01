#include "core/conversation.h"

#include <algorithm>
#include <chrono>

#include "base/text.h"
#include "core/activity.h"
#include "core/images.h"

namespace mico {
namespace {

std::string unescape_str(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}

}  // namespace

// Parses a Question event's `questions` array into a card. Options without a
// label are dropped; the array is capped so a pathological payload cannot cost
// a second of layout.
void parse_question_card(std::string_view json, QuestionCard& card) {
  js::scan_array(json, [&](const js::Value& qv) {
    if (!qv.is_object()) return true;
    QuestionSpec q;
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
      QuestionOption o;
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

bool Conversation::open(const std::string& path, const Adapter* adapter) {
  path_ = path;
  adapter_ = adapter;
  clear();
  if (!file_.open(path)) {
    adapter_ = nullptr;
    return false;
  }
  arena_.reserve(1u << 20);
  parsed_from_ = parsed_to_ = file_.line_count();
  return true;
}

void Conversation::clear() {
  events_.clear();
  arena_.clear();
  state_.clear();
  clear_facts();
  outline_.clear();
  outline_lo_ = UINT64_MAX;
  outline_hi_ = 0;
  outline_calls_.clear();
  outline_orphans_.clear();
}

void Conversation::clear_facts() {
  pending_.clear();
  pending_tools_.clear();
  chart_tools_.clear();
  questions_all_.clear();
  agent_queue_.clear();
  async_.clear();
}

// Images in a line, whichever agent wrote it: one Image event each, a tool
// result's carrying its tool's id. Their pixels stay in the file.
void Conversation::add_images(std::string_view line, size_t before) {
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

// Parses lines [from, to) into batch_, each event stamped with its line. Read
// backwards or forwards, the session state takes only what nothing newer has
// already said.
void Conversation::parse_lines(size_t from, size_t to) {
  batch_.clear();
  file_.will_read(from, to);
  for (size_t i = from; i < to; i++) {
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
}

size_t Conversation::grow_forwards(size_t max_lines) {
  if (!adapter_) return 0;
  const size_t end = max_lines == size_t(-1)
                         ? file_.line_count()
                         : std::min(file_.line_count(), parsed_to_ + max_lines);
  parse_lines(parsed_to_, end);
  parsed_to_ = end;
  events_.append(batch_.data(), batch_.data() + batch_.size());
  for (const Event& e : batch_) note_event(e);
  return batch_.size();
}

size_t Conversation::grow_backwards(size_t max_lines, size_t* mark) {
  if (mark) *mark = arena_.bytes();
  if (!adapter_) return 0;
  if (parsed_from_ == 0) {
    // The file is only indexed from the tail; pull in an older slab first.
    // Every existing line index shifts up by however many lines that added.
    const size_t added = file_.extend_back();
    if (added == 0) return 0;
    shift_lines(added);
  }
  const size_t start = parsed_from_ - std::min(parsed_from_, max_lines);
  if (mark) *mark = arena_.bytes();
  parse_lines(start, parsed_from_);
  events_.prepend(batch_.data(), batch_.data() + batch_.size());
  parsed_from_ = start;
  return batch_.size();
}

void Conversation::replay_facts() {
  clear_facts();
  // Replaying reaches the same verdicts the cards already show.
  const uint64_t gen = cards_gen_;
  for (const Event& e : events_) note_event(e);
  cards_gen_ = gen;
}

void Conversation::reanchor(size_t line) {
  events_.clear();
  arena_.clear();
  clear_facts();
  parsed_from_ = parsed_to_ = std::min(line, file_.line_count());
}

void Conversation::shift_lines(size_t count) {
  for (auto& e : events_) e.src_line += uint32_t(count);
  if (!state_.empty()) state_.from_line += uint32_t(count);
  parsed_from_ += count;
  parsed_to_ += count;
  if (on_shift) on_shift(count);
}

void Conversation::index_back_to(size_t byte) {
  // Every slab the index adds shifts the line numbers the window is holding.
  while (!file_.complete() && file_.indexed_from() > byte) {
    const size_t added = file_.extend_back();
    if (added == 0) break;
    shift_lines(added);
  }
}

// Keeps the live-question sets in step with the parsed window: a Question makes
// an id pending, its ToolResult resolves it.
void Conversation::note_event(const Event& e) {
  if (e.kind == EventKind::Chart && e.tool_id) chart_tools_.push_back(e.tool_id);
  if (e.kind == EventKind::TurnEnd) {
    // An optional question outlives the turn that asked it.
    pending_.clear();
    pending_tools_.clear();
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
      if (open_async_questions()) ++cards_gen_;
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
      ++cards_gen_;
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
    ++cards_gen_;
  auto drop = [&](std::vector<uint64_t>& v) {
    v.erase(std::remove(v.begin(), v.end(), e.tool_id), v.end());
  };
  drop(pending_);
  drop(pending_tools_);
}

bool Conversation::question_answers(uint64_t id, std::vector<std::string>& out) const {
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

bool Conversation::tool_pending(uint64_t id) const {
  return parsed_to_ == file_.line_count() && id != 0 &&
         std::find(pending_tools_.begin(), pending_tools_.end(), id) != pending_tools_.end();
}

const Conversation::AsyncSlot* Conversation::async_slot(uint64_t id) const {
  for (const auto& a : async_)
    if (a.id == id) return &a;
  return nullptr;
}

int Conversation::open_async_questions() const {
  return int(std::count_if(async_.begin(), async_.end(),
                           [](const AsyncSlot& a) { return a.status == AsyncStatus::Open; }));
}

bool Conversation::question_open(uint64_t id) const {
  if (const AsyncSlot* a = async_slot(id)) return a->status == AsyncStatus::Open;
  return std::find(pending_.begin(), pending_.end(), id) != pending_.end();
}

// ------------------------------------------------------------------ outline

void Conversation::outline_scan(size_t a, size_t b, std::vector<OutlineEntry>& into) {
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

bool Conversation::outline(std::vector<OutlineEntry>& out, int budget_ms) {
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

bool Conversation::user_text_at(uint64_t offset, std::string* out) {
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

}  // namespace mico
