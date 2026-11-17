#pragma once
#include <sys/types.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace mico {
class Adapter;

// Side questions ("btw"): one thread per chat, answered by the chat's agent
// from a throwaway copy of the conversation as it is now. Nothing is saved in
// the agent's history, no new chat appears, and the chat itself is never
// touched. Each question carries the thread's earlier questions and answers,
// so follow-ups read as one conversation. Threads are kept in state_dir() so
// they outlive the panel, the view and the daemon.
struct SideTurn {
  std::string question, answer, error;
  enum class State : uint8_t { Queued, Answering, Done, Failed } state = State::Queued;
  bool over() const { return state == State::Done || state == State::Failed; }
};

class SideThreads {
 public:
  ~SideThreads();
  SideThreads() = default;
  SideThreads(const SideThreads&) = delete;
  SideThreads& operator=(const SideThreads&) = delete;

  // The thread of chat `session` of `agent`, read from disk on first use.
  const std::vector<SideTurn>& thread(const std::string& agent, const std::string& session) const;
  // Queues `question`; it is asked in `cwd` once the one before it is
  // answered. An error to show, or empty.
  std::string ask(const Adapter& a, const std::string& session, const std::string& cwd,
                  const std::string& question);
  // Forgets the thread: the next question starts a new one.
  void clear(const std::string& agent, const std::string& session);
  // Reads answers and starts queued questions. True when a thread changed.
  bool pump();
  bool running() const;
  void collect_fds(std::vector<int>& out) const;
  void stop_all();

  // What a question is sent as: the thread so far, then `question`.
  static std::string prompt(const std::vector<SideTurn>& earlier, const std::string& question);
  // Where a thread is kept.
  static std::string file_for(const std::string& agent, const std::string& session);

 private:
  struct Thread {
    const Adapter* adapter = nullptr;
    std::string agent, session, cwd;
    std::vector<SideTurn> turns;
    bool loaded = false;
    pid_t pid = -1;
    int out = -1;
    std::string buf, last_note;  // unread output; the last line that was not an event
    int64_t started_ms = 0;
  };
  Thread& get(const std::string& agent, const std::string& session) const;
  bool start(Thread& t, SideTurn& turn);
  void finish(Thread& t, SideTurn& turn, bool ended);
  void save(const Thread& t) const;
  mutable std::map<std::string, Thread> threads_;  // loaded as asked for
};

}  // namespace mico
