#pragma once
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/conversation.h"
#include "core/workspace.h"

// mico's state protocol: one client's view of a Workspace, as messages. It
// carries state, not frames, so a client draws it however it likes — a web
// page, another program. Transport-agnostic: whatever carries the messages
// feeds receive() and sends what poll() returns, one JSON object each.
//
// Server to client:
//   hello      {protocol, adapters: [{id, name}]}              first, once
//   folders    {folders: [{path, name, subs: [{name, path}],   whenever the
//               chats: [{agent, id, path, title, cwd, mtime,    listing changed
//               bytes, sub, archived}]}]}
//   agents     {agents: [{key, agent, title, cwd, session_id,  whenever a running
//               transcript, status, queued: [text]}]}          agent changed
//               status: "working" | "idle" | "waiting" | "exited"
//               permission: {title: [text], question, options: [text],
//               details: [text], disabled: [bool], cursor, amend, plan},
//               only while the agent shows a permission dialog or a plan
//               to approve: what a client answers with answer_permission
//   chat       {path, events: [event], where: "tail"|"older"|"newer",
//               start: bool}                                   start: nothing older
//   chat_state {path, state: {key: value}, waiting: [id],      when it changed
//               optional: [id], running: [id], held: [text]}
//   result     {rid, ok, error?, key?}                         answers a command
//   error      {message}
//   event: {k, at, text?, name?, summary?, detail?, tool?, ok?}
//     k: "user" | "assistant" | "thinking" | "tool_call" | "tool_result" |
//        "task" | "question" | "turn_end" | "notice" | "queue_add" |
//        "queue_take" | "chart" | "image" | "meta"
//     at: byte offset of the transcript line it came from; stable for the life
//         of the file, unlike line numbers
//     tool: a call's id as 16 hex digits, which links a result to its call
//
// Client to server (rid, when given, comes back on the result):
//   open   {path}                      a stored or running chat: its tail, then
//                                      what is appended as it grows
//   older  {path}                      the next slab of history before the window
//   close  {path}                      stop following it
//   start  {rid, agent, cwd}           an agent mico has an adapter for, in a
//                                      tracked folder
//   resume {rid, agent, id, fork}      a stored chat
//   stop   {rid, key}
//   send   {rid, key, text}            a message to a running agent
//   answer_permission {rid, key, index, note?}
//                                      choice `index` of the dialog the agent
//                                      shows; `note` goes where it has room
//   answer {rid, key, tool, chosen}    the question card whose call is `tool`
//                                      (a waiting id from chat_state): `chosen`
//                                      lists, per question, the option
//                                      indexes picked, [[1], [0, 2]]
//   interrupt {rid, key}               Esc to the agent: stop what it is doing
namespace mico::api {

inline constexpr int kProtocol = 1;

class Client {
 public:
  explicit Client(Workspace& ws);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Handles one message from the client. Its answer, if any, goes out with
  // the next poll().
  void receive(std::string_view message);
  // Appends a message for each thing that changed since the last call, and
  // the answers to what was received, in order.
  void poll(std::vector<std::string>& out);

 private:
  struct Chat {
    std::unique_ptr<Conversation> conv;
    std::string facts;  // the chat_state last sent
  };

  void open_chat(const std::string& path);
  void older(const std::string& path);
  void start(std::string_view rid, const std::string& agent, const std::string& cwd);
  void resume(std::string_view rid, const std::string& agent, const std::string& id, bool fork);
  void stop(std::string_view rid, uint64_t key);
  void send(std::string_view rid, uint64_t key, const std::string& text);
  void answer_permission(std::string_view rid, uint64_t key, int index, const std::string& note);
  void answer_question(std::string_view rid, uint64_t key, const std::string& tool, std::string_view chosen);
  void interrupt(std::string_view rid, uint64_t key);
  void result(std::string_view rid, bool ok, std::string_view error = {}, uint64_t key = 0);
  void error(std::string_view message);

  // The adapter a chat at `path` is read with, or null when the path is no
  // chat the workspace knows: a client never names an arbitrary file.
  const Adapter* chat_adapter(const std::string& path) const;
  LiveSession* session(uint64_t key) const;
  bool tracked_dir(const std::string& dir) const;

  void send_folders(std::vector<std::string>& out);
  void send_agents(std::vector<std::string>& out);
  // Events [from, to) of a chat's window.
  void send_events(const std::string& path, Chat& chat, size_t from, size_t to, std::string_view where,
                   std::vector<std::string>& out);
  void send_facts(const std::string& path, Chat& chat, std::vector<std::string>& out);

  Workspace& ws_;
  std::vector<std::string> queued_;  // answers and chats opened, for the next poll
  bool hello_ = false;
  uint64_t store_seen_ = 0;
  uint64_t agents_seen_ = 0;
  std::string agents_sent_;
  std::map<std::string, Chat> chats_;
};

}  // namespace mico::api
