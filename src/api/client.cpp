#include "api/client.h"

#include <algorithm>
#include <cstdlib>

#include "adapters/adapters.h"
#include "base/json.h"
#include "base/json_write.h"
#include "core/answers.h"
#include "core/images.h"
#include "base/base64.h"

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
    case EventKind::Peer: return "peer";
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
  int index = -1;
  std::string tool, note, file;
  uint64_t at = 0, off = 0, len = 0;
  std::string_view chosen;
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
    else if (k == "index" && v.type == js::Type::Number) index = std::atoi(std::string(v.raw).c_str());
    else if (k == "file") file = text_of(v);
    else if (k == "at" || k == "off" || k == "len")
      (k == "at" ? at : k == "off" ? off : len) = std::strtoull(std::string(v.raw).c_str(), nullptr, 10);
    else if (k == "note") note = text_of(v);
    else if (k == "tool") tool = text_of(v);
    else if (k == "chosen" && v.is_array()) chosen = v.raw;
    return true;
  });
  if (!object) return error("a message is a JSON object");
  if (type == "open") return open_chat(path);
  if (type == "older") return older(path);
  if (type == "close") {
    chats_.erase(path);
    return;
  }
  if (type == "image") return image(path, at, off, len);
  if (type == "file_image") return file_image(path, file);
  if (type == "start") return start(rid, agent, cwd);
  if (type == "resume") return resume(rid, agent, id, fork);
  if (type == "stop") return stop(rid, key);
  if (type == "send") return send(rid, key, text);
  if (type == "answer_permission") return answer_permission(rid, key, index, note);
  if (type == "answer") return answer_question(rid, key, tool, chosen);
  if (type == "interrupt") return interrupt(rid, key);
  error("unknown message type: " + type);
}

void Client::poll(std::vector<std::string>& out) {
  if (!hello_) {
    hello_ = true;
    std::string m;
    jw::Writer w(m);
    w.begin_object().field("type", "hello").field("protocol", kProtocol);
    // The agents a client can start, as menus name them.
    w.key("adapters").begin_array();
    for (const Adapter* a : all_adapters())
      w.begin_object().field("id", a->id()).field("name", a->name()).end_object();
    w.end_array().end_object();
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

// A picture a chat holds, sent when the page asks: found in the line at byte
// `at`, as the images of that line list it, never by a client's own offsets.
void Client::image(const std::string& path, uint64_t at, uint64_t off, uint64_t len) {
  auto it = chats_.find(path);
  if (it == chats_.end()) return error("not open: " + path);
  const Conversation& conv = *it->second.conv;
  const size_t n = conv.file().line_count();
  size_t i = n ? conv.file().line_at_byte(size_t(at)) : 0;
  if (i < n && conv.file().line_offset(i) != at && i > 0 && conv.file().line_offset(i - 1) == at) i--;
  if (i >= n || conv.file().line_offset(i) != at) return;
  const std::string_view line = conv.file().line(i);
  for (const LineImage& im : line_images(line)) {
    if (im.at != off || im.len != len || im.at + im.len > line.size()) continue;
    std::string m;
    jw::Writer w(m);
    w.begin_object().field("type", "image").field("path", path).field("at", at).field("off", off);
    w.field("media", im.media).field("data", line.substr(im.at, im.len)).end_object();
    queued_.push_back(std::move(m));
    return;
  }
}

// A picture file an assistant's markdown points at: only a real image of a
// sensible size, by its bytes and not its name.
void Client::file_image(const std::string& path, const std::string& file) {
  std::string m;
  jw::Writer w(m);
  w.begin_object().field("type", "file_image").field("path", path).field("file", file);
  const auto fail = [&](const char* why) {
    w.field("error", why).end_object();
    queued_.push_back(std::move(m));
  };
  if (!chats_.count(path)) return error("not open: " + path);
  if (file.empty() || file[0] != '/' || file.find("/../") != std::string::npos || file.ends_with("/..")) return fail("not a path");
  constexpr size_t kMax = 12u << 20;
  FILE* f = fopen(file.c_str(), "rb");
  if (!f) return fail("cannot read it");
  std::string bytes;
  char buf[65536];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() <= kMax) bytes.append(buf, got);
  fclose(f);
  if (bytes.size() > kMax) return fail("too large");
  const std::string_view b = bytes;
  const char* media = b.starts_with("\x89PNG") ? "image/png" : b.starts_with("\xFF\xD8\xFF") ? "image/jpeg"
                      : b.starts_with("GIF8") ? "image/gif"
                      : b.size() > 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP" ? "image/webp" : nullptr;
  if (!media) return fail("not an image");
  std::string data;
  base64_append(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), data);
  w.field("media", media).field("data", data).end_object();
  queued_.push_back(std::move(m));
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
    // A dialog the agent is waiting on, read off its screen: the same one the
    // terminal's panel shows.
    PermissionPrompt prompt;
    if (s->needs_input() && s->driver().permission_prompt(s->vt(), prompt)) {
      w.key("permission").begin_object();
      w.key("title").begin_array();
      for (const auto& t : prompt.title) w.str(t);
      w.end_array().field("question", prompt.question);
      w.key("options").begin_array();
      for (const auto& o : prompt.options) w.str(o);
      w.end_array().key("details").begin_array();
      for (const auto& d : prompt.details) w.str(d);
      w.end_array().key("disabled").begin_array();
      for (size_t i = 0; i < prompt.options.size(); i++) w.boolean(i < prompt.disabled.size() && prompt.disabled[i]);
      w.end_array().field("cursor", int64_t(prompt.cursor)).field("amend", prompt.amend).field("plan", prompt.plan);
      w.end_object();
    }
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
  for (const Project& p : ws_.store().projects())
    for (const SubProject& sp : p.subs)
      if (dir == sp.path || dir.starts_with(sp.path + "/")) return true;
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

void Client::answer_permission(std::string_view rid, uint64_t key, int index, const std::string& note) {
  LiveSession* s = session(key);
  if (!s || s->exited()) return result(rid, false, "no running agent " + std::to_string(key));
  PermissionPrompt prompt;
  if (!s->needs_input() || !s->driver().permission_prompt(s->vt(), prompt))
    return result(rid, false, "the agent shows no permission dialog now");
  if (s->answer_sending()) return result(rid, false, "still answering the last one");
  PermissionAnswer answer;
  if (!permission_answer(prompt, index, note, answer)) return result(rid, false, "that choice is not available");
  if (!s->send_answer(answer.steps)) return result(rid, false, "could not answer");
  if (!answer.after.empty()) s->send_after_answer(std::move(answer.after));
  result(rid, true, {}, key);
}

void Client::answer_question(std::string_view rid, uint64_t key, const std::string& tool, std::string_view chosen) {
  LiveSession* s = session(key);
  if (!s || s->exited()) return result(rid, false, "no running agent " + std::to_string(key));
  const uint64_t id = std::strtoull(tool.c_str(), nullptr, 16);
  // The card is read from the chat the client has open: the question as the
  // agent asked it, never as the client says it was.
  const auto it = chats_.find(s->transcript());
  if (it == chats_.end() || !id) return result(rid, false, "open the chat first");
  const Conversation& conv = *it->second.conv;
  const auto& waiting = conv.pending_questions();
  if (!conv.at_tail() || std::find(waiting.begin(), waiting.end(), id) == waiting.end())
    return result(rid, false, "that question is not waiting for an answer");
  QuestionCard card;
  bool found = false;
  const auto& events = conv.events();
  for (size_t i = events.size(); i-- > 0 && !found;)
    if (events[i].kind == EventKind::Question && events[i].tool_id == id) {
      card.tool_id = id;
      parse_question_card(conv.arena().view(events[i].detail), card);
      found = true;
    }
  if (!found || card.questions.empty()) return result(rid, false, "no such question");
  if (card.async) return result(rid, false, "that one is answered with a message");
  if (s->answer_sending()) return result(rid, false, "still answering the last one");

  // Per question, the options picked: a flag for each.
  std::vector<std::vector<uint8_t>> picked;
  for (const auto& q : card.questions) picked.emplace_back(q.options.size(), uint8_t(0));
  size_t qi = 0;
  js::scan_array(chosen, [&](const js::Value& row) {
    if (qi < picked.size() && row.is_array())
      js::scan_array(row.raw, [&](const js::Value& n) {
        const int at = n.type == js::Type::Number ? std::atoi(std::string(n.raw).c_str()) : -1;
        if (at >= 0 && size_t(at) < picked[qi].size()) picked[qi][size_t(at)] = 1;
        return true;
      });
    qi++;
    return true;
  });
  std::vector<bool> multi;
  std::vector<int> recommended, options;
  for (size_t i = 0; i < card.questions.size(); i++) {
    const auto& q = card.questions[i];
    const size_t n = std::count(picked[i].begin(), picked[i].end(), uint8_t(1));
    if (n == 0 || (!q.multi && n > 1)) return result(rid, false, "choose an option for each question");
    multi.push_back(q.multi);
    recommended.push_back(q.recommended);
    options.push_back(int(q.options.size()));
  }
  const auto steps = question_answer_steps(s->driver(), multi, recommended, options, picked);
  if (s->driver().menu_keys().paced) {
    if (!s->send_answer(steps)) return result(rid, false, "could not send the answer");
  } else {
    for (const auto& step : steps) s->pty().write(step);
  }
  result(rid, true, {}, key);
}

void Client::interrupt(std::string_view rid, uint64_t key) {
  LiveSession* s = session(key);
  if (!s || s->exited()) return result(rid, false, "no running agent " + std::to_string(key));
  s->pty().write("\x1b");
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
