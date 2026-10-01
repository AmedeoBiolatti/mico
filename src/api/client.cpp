#include "api/client.h"

#include <cstdlib>

#include "adapters/adapters.h"
#include "base/json.h"
#include "base/json_write.h"

namespace mico::api {
namespace {

// A window's first read and each read of older history: about a screen of
// chat, without reading a long transcript whole.
constexpr size_t kChunkLines = 512;
constexpr size_t kTailEvents = 200;

const char* kind_name(EventKind k) {
  switch (k) {
    case EventKind::Meta: return "meta";
    case EventKind::User: return "user";
    case EventKind::Assistant: return "assistant";
    case EventKind::Thinking: return "thinking";
    case EventKind::ToolCall: return "tool_call";
    case EventKind::ToolResult: return "tool_result";
    case EventKind::TaskStatus: return "task";
    case EventKind::Question: return "question";
    case EventKind::TurnEnd: return "turn_end";
    case EventKind::Notice: return "notice";
    case EventKind::QueueAdd: return "queue_add";
    case EventKind::QueueTake: return "queue_take";
    case EventKind::Chart: return "chart";
    case EventKind::Image: return "image";
  }
  return "meta";
}

const char* status_name(LiveSession::Status s) {
  switch (s) {
    case LiveSession::Status::Working: return "working";
    case LiveSession::Status::Idle: return "idle";
    case LiveSession::Status::Waiting: return "waiting";
    case LiveSession::Status::Exited: return "exited";
  }
  return "idle";
}

std::string text_of(const js::Value& v) {
  std::string s;
  if (v.is_string()) js::unescape_append(v.body(), s);
  return s;
}

}  // namespace

Client::Client(Workspace& ws) : ws_(ws) {}
Client::~Client() = default;

void Client::receive(std::string_view message) {
  std::string type, path, rid, agent, cwd, id, text;
  uint64_t key = 0;
  bool fork = false;
  bool object = false;
  js::scan_object(message, [&](std::string_view k, const js::Value& v) {
    object = true;
    if (k == "type") type = text_of(v);
    else if (k == "path") path = text_of(v);
    else if (k == "rid") rid = v.is_string() ? text_of(v) : std::string(v.raw);
    else if (k == "agent") agent = text_of(v);
    else if (k == "cwd") cwd = text_of(v);
    else if (k == "id") id = text_of(v);
    else if (k == "text") text = text_of(v);
    else if (k == "key") key = std::strtoull(std::string(v.is_string() ? v.body() : v.raw).c_str(), nullptr, 10);
    else if (k == "fork") fork = v.is_true();
    return true;
  });
  if (!object) return error("a message is a JSON object");
  if (type == "open") return open_chat(path);
  if (type == "older") return older(path);
  if (type == "close") {
    chats_.erase(path);
    return;
  }
  if (type == "start") return start(rid, agent, cwd);
  if (type == "resume") return resume(rid, agent, id, fork);
  if (type == "stop") return stop(rid, key);
  if (type == "send") return send(rid, key, text);
  error("unknown message type: " + type);
}

void Client::poll(std::vector<std::string>& out) {
  if (!hello_) {
    hello_ = true;
    std::string m;
    jw::Writer(m).begin_object().field("type", "hello").field("protocol", kProtocol).end_object();
    out.push_back(std::move(m));
  }
  if (ws_.store().version() != store_seen_) send_folders(out);
  send_agents(out);
  for (auto& m : queued_) out.push_back(std::move(m));
  queued_.clear();
  for (auto& [path, chat] : chats_) {
    Conversation& conv = *chat.conv;
    if (conv.refresh()) {
      const size_t before = conv.events().size();
      const size_t added = conv.grow_forwards();
      if (added) send_events(path, chat, before, before + added, "newer", out);
    }
    send_facts(path, chat, out);
  }
}

// ------------------------------------------------------------------- chats

const Adapter* Client::chat_adapter(const std::string& path) const {
  if (path.empty()) return nullptr;
  for (const auto& s : ws_.live())
    if (s->transcript() == path) return s->adapter();
  for (const auto& p : ws_.store().projects())
    for (const auto& s : p.sessions)
      if (s.path == path) return adapter_for(s.agent);
  return nullptr;
}

void Client::open_chat(const std::string& path) {
  const Adapter* adapter = chat_adapter(path);
  if (!adapter) return error("no chat at " + path);
  Chat& chat = chats_[path];
  chat.conv = std::make_unique<Conversation>();
  chat.facts.clear();
  Conversation& conv = *chat.conv;
  if (!conv.open(path, adapter)) {
    chats_.erase(path);
    return error("cannot read " + path);
  }
  while (conv.events().size() < kTailEvents) {
    const size_t from = conv.parsed_from();
    if (conv.grow_backwards(kChunkLines) == 0 && conv.parsed_from() == from) break;
  }
  conv.replay_facts();
  send_events(path, chat, 0, conv.events().size(), "tail", queued_);
  send_facts(path, chat, queued_);
}

void Client::older(const std::string& path) {
  auto it = chats_.find(path);
  if (it == chats_.end()) return error("not open: " + path);
  Conversation& conv = *it->second.conv;
  const size_t added = conv.grow_backwards(kChunkLines);
  conv.replay_facts();
  send_events(path, it->second, 0, added, "older", queued_);
}

void Client::send_events(const std::string& path, Chat& chat, size_t from, size_t to,
                         std::string_view where, std::vector<std::string>& out) {
  const Conversation& conv = *chat.conv;
  const Arena& arena = conv.arena();
  std::string m;
  jw::Writer w(m);
  w.begin_object().field("type", "chat").field("path", path).field("where", where);
  w.field("start", conv.parsed_from() == 0 && conv.file().complete());
  w.key("events").begin_array();
  for (size_t i = from; i < to; i++) {
    const Event& e = conv.events()[i];
    w.begin_object().field("k", kind_name(e.kind));
    w.field("at", uint64_t(conv.file().line_offset(e.src_line)));
    if (!e.text.empty()) w.field("text", arena.view(e.text));
    if (!e.name.empty()) w.field("name", arena.view(e.name));
    if (!e.summary.empty()) w.field("summary", arena.view(e.summary));
    if (!e.detail.empty()) w.field("detail", arena.view(e.detail));
    if (e.tool_id) w.key("tool").id(e.tool_id);
    if (!e.ok) w.field("ok", false);
    w.end_object();
  }
  w.end_array().end_object();
  out.push_back(std::move(m));
}

void Client::send_facts(const std::string& path, Chat& chat, std::vector<std::string>& out) {
  const Conversation& conv = *chat.conv;
  std::string f;
  jw::Writer w(f);
  w.key("state").begin_object();
  for (const auto& field : conv.state().fields) w.field(field.key, field.value);
  w.end_object();
  w.key("waiting").begin_array();
  if (conv.at_tail())
    for (uint64_t id : conv.pending_questions()) w.id(id);
  w.end_array();
  w.key("optional").begin_array();
  for (const auto& a : conv.async_questions())
    if (a.status == Conversation::AsyncStatus::Open) w.id(a.id);
  w.end_array();
  w.key("running").begin_array();
  if (conv.at_tail())
    for (uint64_t id : conv.pending_tools()) w.id(id);
  w.end_array();
  w.key("held").begin_array();
  for (const Str& s : conv.agent_queue()) w.str(conv.arena().view(s));
  w.end_array();
  if (f == chat.facts) return;
  chat.facts = f;
  std::string m;
  jw::Writer(m).begin_object().field("type", "chat_state").field("path", path);
  m += ',';
  m += f;
  m += '}';
  out.push_back(std::move(m));
}

// ------------------------------------------------------------ the listings

void Client::send_folders(std::vector<std::string>& out) {
  const Store& store = ws_.store();
  store_seen_ = store.version();
  std::string m;
  jw::Writer w(m);
  w.begin_object().field("type", "folders").key("folders").begin_array();
  for (const Project& p : store.projects()) {
    w.begin_object().field("path", p.path).field("name", p.name);
    w.key("subs").begin_array();
    for (const SubProject& sp : p.subs) w.begin_object().field("name", sp.name).field("path", sp.path).end_object();
    w.end_array();
    w.key("chats").begin_array();
    for (const SessionRef& s : p.sessions) {
      const std::string* name = store.custom_name(s.agent, s.id);
      w.begin_object()
          .field("agent", s.agent)
          .field("id", s.id)
          .field("path", s.path)
          .field("title", name ? *name : s.title)
          .field("cwd", s.cwd)
          .field("mtime", s.mtime)
          .field("bytes", s.bytes)
          .field("sub", s.sub)
          .field("archived", store.archived(s.agent, s.id))
          .end_object();
    }
    w.end_array().end_object();
  }
  w.end_array().end_object();
  out.push_back(std::move(m));
}

void Client::send_agents(std::vector<std::string>& out) {
  // What could have changed: the set of sessions, or any one of them.
  uint64_t seen = ws_.sessions_version();
  for (const auto& s : ws_.live()) seen = seen * 1000003 + s->generation();
  if (seen == agents_seen_ && !agents_sent_.empty()) return;
  agents_seen_ = seen;
  std::string m;
  jw::Writer w(m);
  w.begin_object().field("type", "agents").key("agents").begin_array();
  for (const auto& s : ws_.live()) {
    w.begin_object()
        .field("key", s->serial())
        .field("agent", s->agent())
        .field("title", ws_.title_of(*s))
        .field("cwd", s->cwd())
        .field("session_id", s->session_id())
        .field("transcript", s->transcript())
        .field("status", status_name(s->status()));
    w.key("queued").begin_array();
    for (const auto& parts : s->queued()) {
      std::string text;
      for (const auto& part : parts) text += part.text;
      w.str(text);
    }
    w.end_array().end_object();
  }
  w.end_array().end_object();
  if (m == agents_sent_) return;
  agents_sent_ = m;
  out.push_back(std::move(m));
}

// ---------------------------------------------------------------- commands

LiveSession* Client::session(uint64_t key) const {
  for (const auto& s : ws_.live())
    if (s->serial() == key) return s.get();
  return nullptr;
}

bool Client::tracked_dir(const std::string& dir) const {
  for (const auto& f : ws_.store().folders())
    if (dir == f || dir.starts_with(f + "/")) return true;
  return false;
}

void Client::start(std::string_view rid, const std::string& agent, const std::string& cwd) {
  // Only agents mico knows, only in folders the user tracks: a client asks
  // for an agent, never for a command line.
  if (!adapter_for(agent)) return result(rid, false, "no adapter for " + agent);
  if (!tracked_dir(cwd)) return result(rid, false, "not a tracked folder: " + cwd);
  const Workspace::Started r = ws_.start_agent(agent, cwd);
  if (!r.session) return result(rid, false, r.error);
  result(rid, true, {}, r.session->serial());
}

void Client::resume(std::string_view rid, const std::string& agent, const std::string& id, bool fork) {
  const SessionRef* ref = nullptr;
  for (const auto& p : ws_.store().projects())
    for (const auto& s : p.sessions)
      if (s.agent == agent && s.id == id) ref = &s;
  if (!ref) return result(rid, false, "no stored chat " + agent + " " + id);
  const Workspace::Started r = ws_.continue_session(agent, id, ref->cwd, fork);
  if (!r.session) return result(rid, false, r.error.empty() ? "cannot continue it" : r.error);
  result(rid, true, {}, r.session->serial());
}

void Client::stop(std::string_view rid, uint64_t key) {
  LiveSession* s = session(key);
  if (!s) return result(rid, false, "no agent " + std::to_string(key));
  ws_.close(s);
  result(rid, true, {}, key);
}

void Client::send(std::string_view rid, uint64_t key, const std::string& text) {
  LiveSession* s = session(key);
  if (!s || s->exited()) return result(rid, false, "no running agent " + std::to_string(key));
  if (text.empty()) return result(rid, false, "nothing to send");
  if (!s->send_parts({MessagePart{false, text}})) return result(rid, false, "the agent is not ready");
  result(rid, true, {}, key);
}

void Client::result(std::string_view rid, bool ok, std::string_view error, uint64_t key) {
  std::string m;
  jw::Writer w(m);
  w.begin_object().field("type", "result").field("rid", rid).field("ok", ok);
  if (!error.empty()) w.field("error", error);
  if (key) w.field("key", key);
  w.end_object();
  queued_.push_back(std::move(m));
}

void Client::error(std::string_view message) {
  std::string m;
  jw::Writer(m).begin_object().field("type", "error").field("message", message).end_object();
  queued_.push_back(std::move(m));
}

}  // namespace mico::api
