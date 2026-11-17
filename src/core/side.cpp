#include "core/side.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "adapters/adapter.h"
#include "base/fs.h"
#include "base/json.h"
#include "base/json_write.h"
#include "base/log.h"
#include "base/path.h"
#include "core/pty.h"

extern char** environ;

namespace mico {
namespace {
constexpr int64_t kTimeoutMs = 5 * 60 * 1000;
constexpr size_t kEarlierTurns = 6;
constexpr size_t kEarlierAnswer = 3000;  // bytes of each earlier answer resent
constexpr size_t kKeptTurns = 200;

int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string key_of(const std::string& agent, const std::string& session) { return agent + "\n" + session; }

std::string cut(std::string_view s, size_t n) {
  if (s.size() <= n) return std::string(s);
  size_t at = n;
  while (at > 0 && (uint8_t(s[at]) & 0xC0) == 0x80) at--;  // not inside a character
  return std::string(s.substr(0, at)) + "…";
}
}  // namespace

SideThreads::~SideThreads() { stop_all(); }

std::string SideThreads::file_for(const std::string& agent, const std::string& session) {
  std::string name = agent + "-" + session;
  for (char& c : name) if (c == '/' || c == '\\' || c == '\n') c = '_';
  return state_dir() + "/btw/" + name + ".jsonl";
}

std::string SideThreads::prompt(const std::vector<SideTurn>& earlier, const std::string& question) {
  std::string p =
      "This is a side question about the conversation above, asked while the main task carries on "
      "elsewhere. Answer it directly and briefly from what the conversation already holds. You may "
      "read files to answer, but do not edit anything, run commands that change anything, or continue "
      "the main task.\n\n";
  std::vector<const SideTurn*> done;
  for (const auto& t : earlier) if (t.state == SideTurn::State::Done && !t.answer.empty()) done.push_back(&t);
  if (done.size() > kEarlierTurns) done.erase(done.begin(), done.end() - long(kEarlierTurns));
  if (!done.empty()) {
    p += "Earlier side questions in this thread, oldest first:\n\n";
    for (const SideTurn* t : done)
      p += "<side_question>\n" + t->question + "\n</side_question>\n<side_answer>\n" +
           cut(t->answer, kEarlierAnswer) + "\n</side_answer>\n\n";
    p += "The new side question follows up on them.\n\n";
  }
  p += "Side question: " + question + "\n";
  return p;
}

SideThreads::Thread& SideThreads::get(const std::string& agent, const std::string& session) const {
  Thread& t = threads_[key_of(agent, session)];
  if (t.loaded) return t;
  t.loaded = true;
  t.agent = agent;
  t.session = session;
  std::string buf;
  const std::string path = file_for(agent, session);
  const std::string_view blob = fs::read_prefix(path, 8u << 20, buf);
  fs::for_each_line(blob, [&](std::string_view line) {
    SideTurn turn;
    bool any = false;
    js::scan_object(line, [&](std::string_view k, const js::Value& v) {
      if (!v.is_string()) return true;
      std::string* to = k == "q" ? &turn.question : k == "a" ? &turn.answer : k == "e" ? &turn.error : nullptr;
      if (to) { js::unescape_append(v.body(), *to); any = true; }
      return true;
    });
    if (any && !turn.question.empty()) {
      turn.state = turn.error.empty() ? SideTurn::State::Done : SideTurn::State::Failed;
      t.turns.push_back(std::move(turn));
    }
    return true;
  });
  return t;
}

const std::vector<SideTurn>& SideThreads::thread(const std::string& agent, const std::string& session) const {
  return get(agent, session).turns;
}

std::string SideThreads::ask(const Adapter& a, const std::string& session, const std::string& cwd,
                             const std::string& question) {
  if (session.empty()) return "send the first chat message before asking a side question";
  if (a.side_question_argv(session).empty()) return "this agent cannot answer side questions";
  Thread& t = get(std::string(a.id()), session);
  t.adapter = &a;
  t.cwd = cwd;
  SideTurn turn;
  turn.question = question;
  t.turns.push_back(std::move(turn));
  pump();
  return {};
}

void SideThreads::clear(const std::string& agent, const std::string& session) {
  Thread& t = get(agent, session);
  if (t.pid > 0) {
    kill(-t.pid, SIGTERM);
    kill(t.pid, SIGTERM);
    waitpid(t.pid, nullptr, 0);
    close(t.out);
    t.pid = -1;
    t.out = -1;
  }
  t.turns.clear();
  t.buf.clear();
  unlink(file_for(agent, session).c_str());
}

bool SideThreads::start(Thread& t, SideTurn& turn) {
  std::vector<SideTurn> earlier(t.turns.begin(), t.turns.begin() + (&turn - t.turns.data()));
  const std::string ask = prompt(earlier, turn.question);
  const std::vector<std::string> argv = t.adapter->side_question_argv(t.session);
  // Built before the fork: the daemon has threads, so the child only execs.
  std::vector<std::string> env = agent_env();
  std::vector<char*> envp = env_pointers(env);
  std::vector<char*> args;
  for (const std::string& s : argv) args.push_back(const_cast<char*>(s.c_str()));
  args.push_back(nullptr);
  int in[2], out[2];
  if (pipe2(in, O_CLOEXEC) != 0) return false;
  if (pipe2(out, O_CLOEXEC) != 0) {
    close(in[0]);
    close(in[1]);
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    for (int fd : {in[0], in[1], out[0], out[1]}) close(fd);
    return false;
  }
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(out[1], 1);
    dup2(out[1], 2);  // its complaints, for when it gives up
    if (chdir(t.cwd.c_str()) != 0) _exit(127);
    setsid();
    environ = envp.data();
    execvp(args[0], args.data());
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  // Well under a pipe's buffer: prompt() bounds what it resends.
  size_t at = 0;
  while (at < ask.size()) {
    const ssize_t n = write(in[1], ask.data() + at, ask.size() - at);
    if (n <= 0) break;
    at += size_t(n);
  }
  close(in[1]);
  fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
  t.pid = pid;
  t.out = out[0];
  t.buf.clear();
  t.last_note.clear();
  t.started_ms = now_ms();
  turn.state = SideTurn::State::Answering;
  logs::line("btw: asking " + t.agent + " " + t.session + " (pid " + std::to_string(pid) + ")");
  return true;
}

void SideThreads::finish(Thread& t, SideTurn& turn, bool ended) {
  if (t.pid > 0) {
    if (!ended) {
      kill(-t.pid, SIGTERM);
      kill(t.pid, SIGTERM);
    }
    int status = 0;
    waitpid(t.pid, &status, 0);
    if (turn.error.empty() && turn.answer.empty()) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 127) turn.error = "could not start " + t.agent;
      else if (!t.last_note.empty()) turn.error = t.last_note;
      else turn.error = t.agent + " stopped without answering";
    }
  }
  if (t.out >= 0) close(t.out);
  t.pid = -1;
  t.out = -1;
  t.buf.clear();
  turn.state = turn.error.empty() && !turn.answer.empty() ? SideTurn::State::Done : SideTurn::State::Failed;
  if (turn.state == SideTurn::State::Failed && turn.error.empty()) turn.error = "no answer";
  if (t.turns.size() > kKeptTurns) t.turns.erase(t.turns.begin(), t.turns.end() - long(kKeptTurns));
  save(t);
}

void SideThreads::save(const Thread& t) const {
  std::string data;
  for (const auto& turn : t.turns) {
    if (!turn.over()) continue;
    jw::Writer w(data);
    w.begin_object().field("q", turn.question).field("a", turn.answer);
    if (!turn.error.empty()) w.field("e", turn.error);
    w.end_object();
    data += '\n';
  }
  fs::make_dirs(state_dir() + "/btw");
  write_file_atomic(file_for(t.agent, t.session), data);
}

bool SideThreads::pump() {
  bool changed = false;
  for (auto& [key, t] : threads_) {
    auto running = std::find_if(t.turns.begin(), t.turns.end(),
                                [](const SideTurn& s) { return s.state == SideTurn::State::Answering; });
    if (running != t.turns.end() && t.pid > 0) {
      SideTurn& turn = *running;
      char chunk[65536];
      bool eof = false, over = false;
      for (;;) {
        const ssize_t n = read(t.out, chunk, sizeof chunk);
        if (n > 0) { t.buf.append(chunk, size_t(n)); continue; }
        if (n == 0) eof = true;
        break;
      }
      size_t from = 0;
      for (size_t nl; !over && (nl = t.buf.find('\n', from)) != std::string::npos; from = nl + 1) {
        const std::string_view line(t.buf.data() + from, nl - from);
        const std::string before = turn.answer;
        if (!line.empty() && line.front() != '{') {
          // Not an event: a warning or why it gave up.
          std::string_view note = line;
          while (!note.empty() && (note.back() == '\r' || note.back() == ' ')) note.remove_suffix(1);
          if (!note.empty()) t.last_note = std::string(note.substr(0, 300));
          continue;
        }
        over = t.adapter->read_side_answer(line, turn.answer, turn.error);
        changed |= turn.answer != before || over;
      }
      t.buf.erase(0, from);
      if (over || eof || now_ms() - t.started_ms > kTimeoutMs) {
        if (!over && !eof) turn.error = "no answer after five minutes";
        finish(t, turn, eof);
        changed = true;
      }
      continue;
    }
    if (running != t.turns.end()) {  // its process is gone (a daemon restart)
      running->state = SideTurn::State::Failed;
      running->error = "interrupted";
      changed = true;
    }
    auto next = std::find_if(t.turns.begin(), t.turns.end(),
                             [](const SideTurn& s) { return s.state == SideTurn::State::Queued; });
    if (next == t.turns.end() || !t.adapter) continue;
    if (!start(t, *next)) {
      next->state = SideTurn::State::Failed;
      next->error = "could not start " + t.agent;
      save(t);
    }
    changed = true;
  }
  return changed;
}

bool SideThreads::running() const {
  return std::any_of(threads_.begin(), threads_.end(), [](const auto& e) { return e.second.pid > 0; });
}

void SideThreads::collect_fds(std::vector<int>& out) const {
  for (const auto& [key, t] : threads_) if (t.out >= 0) out.push_back(t.out);
}

void SideThreads::stop_all() {
  for (auto& [key, t] : threads_) {
    if (t.pid <= 0) continue;
    kill(-t.pid, SIGTERM);
    kill(t.pid, SIGTERM);
    waitpid(t.pid, nullptr, 0);
    close(t.out);
    t.pid = -1;
    t.out = -1;
  }
}

}  // namespace mico
