#include "core/work.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>

#include "adapters/screen.h"
#include "base/text.h"
#include "core/session.h"
#include "core/workspace.h"

namespace mico {
namespace {
constexpr size_t kOutputBytes = 64u << 10;
constexpr size_t kMaxItems = 96;

std::string tool_key(uint64_t id) {
  char key[32];
  snprintf(key, sizeof key, "tool:%016llx", (unsigned long long)id);
  return key;
}

bool terminal_tool(std::string_view name) {
  return name == "Bash" || name == "bash" || name == "shell" || name == "exec" || name == "exec_command" ||
         name == "Monitor" || name == "monitor" || name.ends_with(".exec_command");
}

std::string progress_text(const BackgroundTask& t) {
  std::string s;
  if (t.fraction >= 0) s = std::to_string(int(std::clamp(t.fraction, 0.0, 1.0) * 100 + .5)) + "%";
  if (t.done >= 0 && t.total > 0) s += (s.empty() ? "" : " · ") + std::to_string(t.done) + "/" + std::to_string(t.total);
  if (t.eta_s >= 0) {
    char b[48];
    if (t.eta_s >= 60) snprintf(b, sizeof b, "%dm %02ds left", t.eta_s / 60, t.eta_s % 60);
    else snprintf(b, sizeof b, "%ds left", t.eta_s);
    s += (s.empty() ? "" : " · ") + std::string(b);
  }
  return s;
}

std::string tail_file(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return {};
  struct stat st{};
  std::string text;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
    const size_t len = std::min<uint64_t>(uint64_t(st.st_size), kOutputBytes);
    text.resize(len);
    const ssize_t got = pread(fd, text.data(), len, st.st_size - off_t(len));
    text.resize(got > 0 ? size_t(got) : 0);
  }
  close(fd);
  return text;
}
}  // namespace

const WorkItem* WorkSet::find(const std::string& id) const {
  for (const auto& i : items_) if (i.id == id) return &i;
  return nullptr;
}

void WorkSet::refresh(const Conversation& conv, const LiveSession* live, const Workspace& ws, bool force) {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (path_ != conv.path()) { items_.clear(); child_.clear(); path_ = conv.path(); force = true; }
  if (!force && now - refreshed_ms_ < 500) return;
  refreshed_ms_ = now;
  supports_btw_ = false;
  if (live && live->adapter()) supports_btw_ = live->adapter()->native_side_questions() ||
      (live->adapter()->can_fork() && !live->session_id().empty());

  // Retain completed work and side answers for this view's lifetime.
  for (auto& i : items_) if (i.status == "working") i.status = "done";
  auto put = [&](WorkItem i) {
    for (auto& old : items_) if (old.id == i.id) {
      // The launch call remains after the background task ends. Keep its
      // known output file so completed work still opens the actual log.
      if (i.kind == "terminal") {
        if (i.output_file.empty()) i.output_file = old.output_file;
        if (i.text.empty()) i.text = old.text;
      }
      old = std::move(i); return;
    }
    items_.push_back(std::move(i));
  };
  const auto& events = conv.events();
  const auto& arena = conv.arena();
  std::unordered_map<uint64_t, const Event*> results;
  const size_t from = events.size() > 2048 ? events.size() - 2048 : 0;
  for (size_t n = events.size(); n > from; n--) {
    const Event& e = events[n - 1];
    if (e.kind == EventKind::ToolResult) results.try_emplace(e.tool_id, &e);
  }
  const auto& pending = conv.pending_tools();
  size_t calls = 0;
  for (size_t n = events.size(); n > from && calls < 64; n--) {
    const Event& e = events[n - 1];
    if (e.kind != EventKind::ToolCall) continue;
    calls++;
    const bool running = live && !live->exited() && std::find(pending.begin(), pending.end(), e.tool_id) != pending.end();
    const uint64_t at = conv.file().line_offset(e.src_line);
    const auto result = results.find(e.tool_id);
    if (terminal_tool(arena.view(e.name))) {
      WorkItem i;
      i.id = tool_key(e.tool_id); i.kind = "terminal"; i.tool = e.tool_id; i.at = at;
      i.title = text::oneline(arena.view(e.summary), 160);
      if (i.title.empty()) i.title = std::string(arena.view(e.name));
      i.status = running ? "working" : result != results.end() && !result->second->ok ? "failed" : "done";
      put(std::move(i));
    }
    const auto name = arena.view(e.name);
    if (!conv.adapter() || (name != "Agent" && name != "Task" && name != "task" &&
        name != "spawn_agent" && name != "agent" && !name.ends_with(".spawn_agent"))) continue;
    std::vector<SubagentRun> runs;
    conv.adapter()->call_subagents(conv.path(), conv.file().line(e.src_line), e.tool_id, runs);
    for (const auto& r : runs) {
      WorkItem i;
      i.id = "agent:" + r.path; i.kind = "agent"; i.path = r.path; i.title = r.name;
      i.tool = e.tool_id; i.at = at; i.status = running ? "working" : "done"; i.info = r.kind;
      put(std::move(i));
    }
  }
  for (const Project& p : ws.store().projects()) for (const SessionRef& s : p.sessions) {
    if (s.parent != conv.path()) continue;
    const std::string id = "agent:" + s.path;
    if (find(id)) continue;
    WorkItem i;
    i.id = id; i.kind = "agent"; i.path = s.path; i.title = s.subagent.empty() ? s.title : s.subagent;
    i.status = "done"; put(std::move(i));
  }
  if (live) for (const auto& q : ws.side_questions()) if (q.parent == live->serial() && q.context == live->session_id()) {
    for (const auto& child : ws.live()) if (child->serial() == q.child) {
      WorkItem i;
      i.id = "side:" + std::to_string(q.child); i.key = q.child; i.kind = "btw";
      i.path = child->transcript(); i.title = q.question;
      i.status = child->needs_input() ? "waiting" : child->busy() || !child->queued().empty() ? "working" : "done";
      if (child->exited() && child->exit_status() != 0) i.status = "failed";
      put(std::move(i));
    }
  }
  if (live && !live->exited()) {
    for (const BackgroundTask& t : live->background()) {
      WorkItem i;
      i.id = "background:" + t.id; i.kind = t.kind == "agent" ? "agent" : "terminal";
      i.title = t.what.empty() ? t.id : t.what; i.at = t.offset; i.status = "working";
      i.info = progress_text(t); i.output_file = t.output; i.text = t.last_event;
      auto old = std::find_if(items_.begin(), items_.end(), [&](const WorkItem& a) { return a.id == i.id; });
      if (i.kind == "agent") {
        const auto resolved = std::find_if(items_.begin(), items_.end(), [&](const WorkItem& a) {
          return a.kind == "agent" && !a.path.empty() &&
              (a.path.find(t.id) != std::string::npos || a.title == t.what || a.title == t.id);
        });
        if (resolved != items_.end()) old = resolved;
      }
      if (old == items_.end()) old = std::find_if(items_.begin(), items_.end(), [&](const WorkItem& a) {
        return a.kind == i.kind && ((i.kind == "terminal" && a.at == t.offset && a.title == t.what) ||
               (i.kind == "agent" && (a.path.find(t.id) != std::string::npos || a.title == t.id || a.title == t.what)));
      });
      if (old == items_.end() && i.kind == "terminal" &&
          std::count_if(items_.begin(), items_.end(), [&](const WorkItem& a) { return a.kind == i.kind && a.at == t.offset; }) == 1)
        old = std::find_if(items_.begin(), items_.end(), [&](const WorkItem& a) { return a.kind == i.kind && a.at == t.offset; });
      if (old != items_.end()) {
        i.id = old->id; i.path = old->path; i.tool = old->tool;
      }
      const bool resolved_agent = i.kind == "agent" && !i.path.empty();
      put(std::move(i));
      if (resolved_agent) std::erase_if(items_, [&](const WorkItem& a) { return a.id == "background:" + t.id && a.path.empty(); });
    }
    BtwPanel btw;
    if (live->driver().side_panel(live->vt(), btw) && btw.current >= 0 && btw.current < int(btw.questions.size())) {
      WorkItem i;
      i.id = "btw:" + btw.questions[size_t(btw.current)]; i.kind = "btw";
      i.title = btw.questions[size_t(btw.current)]; i.text = btw.answer;
      i.status = btw.answering ? "working" : "done";
      put(std::move(i));
    }
  }
  std::stable_sort(items_.begin(), items_.end(), [](const WorkItem& a, const WorkItem& b) {
    const auto rank = [](const WorkItem& i) { return i.status == "failed" || i.status == "waiting" ? 0 : i.status == "working" ? 1 : 2; };
    return rank(a) != rank(b) ? rank(a) < rank(b) : a.at > b.at;
  });
  if (items_.size() > kMaxItems) items_.resize(kMaxItems);
}

std::string WorkSet::detail(const WorkItem& item, const Conversation& conv) {
  if (item.kind == "btw" && item.path.empty()) return item.text;
  if (!item.output_file.empty()) if (auto text = tail_file(item.output_file); !text.empty()) return text;
  if ((item.kind == "agent" || item.kind == "btw") && !item.path.empty() && conv.adapter()) {
    if (child_.path() != item.path || !child_.is_open()) {
      if (!child_.open(item.path, conv.adapter())) return "Waiting for the agent's transcript…";
      child_.grow_backwards(256);
    } else {
      child_.refresh();
      if (!child_.at_tail()) child_.grow_forwards(256);
    }
    std::string text;
    bool side_prompt = false;
    const auto& events = child_.events();
    for (size_t n = events.size(); n > 0 && text.size() < kOutputBytes; n--) {
      const Event& e = events[n - 1];
      if (e.kind != EventKind::Assistant && e.kind != EventKind::User && e.kind != EventKind::Peer) continue;
      const std::string role = e.kind == EventKind::User ? "You" : e.kind == EventKind::Peer ? "Agent" : "Assistant";
      auto body = child_.arena().view(e.text);
      constexpr std::string_view kSidePrompt = "Answer this side question using the conversation context. Do not edit files or continue the main task.\n\n";
      const bool question = e.kind == EventKind::User && item.kind == "btw" && body.starts_with(kSidePrompt);
      if (question) { body.remove_prefix(kSidePrompt.size()); side_prompt = true; }
      text.insert(0, "**" + role + "**\n\n" + std::string(body) + "\n\n");
      if (question) break;  // inherited parent history belongs in the main chat
    }
    if (item.kind == "btw" && !side_prompt) return "Waiting for the side conversation…";
    if (text.size() > kOutputBytes) text.erase(0, text.size() - kOutputBytes);
    return text.empty() ? "Waiting for the agent's first message…" : text;
  }
  if (!item.text.empty()) return item.text;
  for (size_t n = conv.events().size(); n > 0; n--) {
    const Event& e = conv.events()[n - 1];
    if (e.kind == EventKind::ToolResult && e.tool_id == item.tool) {
      const auto text = conv.arena().view(e.text);
      return std::string(text.substr(text.size() > kOutputBytes ? text.size() - kOutputBytes : 0));
    }
  }
  return item.status == "working" ? "Waiting for output…" : "No output recorded.";
}
}  // namespace mico
