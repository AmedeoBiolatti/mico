#include "core/pty.h"
#include "core/scope.h"

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "base/log.h"
#include "base/process.h"
#include <string>
#include <string_view>

extern char** environ;

namespace mico {

// A spawned agent must not inherit the session identity of whatever launched
// mico. If mico is started from inside a coding agent — which is exactly how
// people use a tool like this — these variables make the child believe it is a
// nested sub-session of its parent, and Claude Code then stops writing a
// transcript at all, which silently breaks the chat view.
//
// Only those: the user's own configuration under the same prefixes stays
// (CLAUDE_CONFIG_DIR, CLAUDE_CODE_USE_BEDROCK, CODEX_HOME, ...), or agents
// started by mico would read another config, or reach another API.
bool session_identity(std::string_view key) {
  static constexpr std::string_view kNames[] = {
      "CLAUDECODE", "CLAUDE_PID", "CLAUDE_EFFORT", "CLAUDE_PROJECT_DIR", "CLAUDE_ENV_FILE",
      "CLAUDE_CODE_ENTRYPOINT", "CLAUDE_CODE_EXECPATH", "CLAUDE_CODE_CHILD_SESSION", "CLAUDE_CODE_SSE_PORT",
      "CLAUDE_AGENT_SDK_VERSION", "CODEX_SANDBOX", "CODEX_SANDBOX_NETWORK_DISABLED", "CODEX_THREAD_ID"};
  static constexpr std::string_view kPrefixes[] = {"CLAUDE_CODE_SESSION_", "CLAUDE_CODE_MESSAGING_",
                                                   "ANTHROPIC_CLI_", "MICO_"};
  for (const std::string_view n : kNames)
    if (key == n) return true;
  for (const std::string_view p : kPrefixes)
    if (key.starts_with(p)) return true;
  return false;
}

std::vector<std::string> agent_env(const std::vector<std::string>& set) {
  const auto name_of = [](std::string_view kv) { return kv.substr(0, kv.find('=')); };
  std::vector<std::string> env;
  for (char** e = environ; e && *e; e++) {
    const std::string_view kv(*e);
    if (kv.find('=') == std::string_view::npos) continue;
    if (!session_identity(name_of(kv))) env.emplace_back(kv);
  }
  for (const std::string& s : set) {
    const std::string_view key = name_of(s);
    std::erase_if(env, [&](const std::string& kv) { return name_of(kv) == key; });
    if (s.find('=') != std::string::npos) env.push_back(s);
  }
  return env;
}

std::vector<char*> env_pointers(std::vector<std::string>& env) {
  std::vector<char*> out;
  out.reserve(env.size() + 1);
  for (auto& kv : env) out.push_back(kv.data());
  out.push_back(nullptr);
  return out;
}

Pty::~Pty() {
  if (fd_ >= 0) ::close(fd_);
  if (pid_ > 0 && !exited_) {
    ::kill(pid_, SIGHUP);
    int st = 0;
    waitpid(pid_, &st, WNOHANG);
  }
}

bool Pty::spawn(const std::vector<std::string>& argv, const std::string& cwd, int w, int h, const std::string& unit,
                const std::string& description) {
  spawn_error_.clear();
  if (argv.empty()) return false;

  {
    std::string joined;
    for (const auto& a : argv) { joined += a; joined += ' '; }
    MLOG("spawn: argv=[ %s] cwd=%s", joined.c_str(), cwd.c_str());
  }
  if (!cwd.empty() && access(cwd.c_str(), X_OK) != 0) {
    spawn_error_ = "folder not accessible: " + cwd;
    exited_ = true;
    MLOG("spawn FAILED: %s", spawn_error_.c_str());
    return false;
  }
  if (const std::string bin = proc::find_program(argv[0]); bin.empty()) {
    spawn_error_ = "'" + argv[0] + "' is not on PATH";
    exited_ = true;
    MLOG("spawn FAILED: %s  (PATH=%s)", spawn_error_.c_str(), getenv("PATH") ? getenv("PATH") : "");
    return false;
  } else {
    MLOG("spawn: resolved '%s' -> %s", argv[0].c_str(), bin.c_str());
  }

  // In a scope of its own, where there are scopes; built before the fork.
  const std::vector<std::string> run = unit.empty() ? argv : scope::wrap(argv, unit, description);
  unit_ = run.size() != argv.size() ? unit + ".scope" : std::string();
  if (!unit_.empty()) MLOG("spawn: in scope %s", unit_.c_str());

  winsize ws{};
  ws.ws_col = (unsigned short)(w > 0 ? w : 80);
  ws.ws_row = (unsigned short)(h > 0 ? h : 24);

  // Agents key their rendering off TERM and COLORTERM; without them Claude Code
  // drops to a degraded mode and the raw view stops looking like the real thing.
  std::vector<std::string> env = agent_env({"TERM=xterm-256color", "COLORTERM=truecolor", "LINES", "COLUMNS"});
  std::vector<char*> envp = env_pointers(env);
  std::vector<char*> args;
  args.reserve(run.size() + 1);
  for (const auto& a : run) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);

  int master = -1;
  pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) return false;

  if (pid == 0) {
    if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) _exit(127); }
    // Only its terminal goes with it. Inherited, the daemon's sockets and the
    // other agents' terminals would outlive mico in every agent and in what
    // they start (MCP servers, shells): closing a pane would not hang its
    // agent up while another agent still held that terminal open.
    if (close_range(3, ~0u, 0) != 0)
      for (int fd = 3; fd < 4096; fd++) close(fd);

    environ = envp.data();  // execvp searches PATH with it, and passes it on
    execvp(args[0], args.data());
    _exit(127);
  }

  fcntl(master, F_SETFD, FD_CLOEXEC);  // the next agent's must not hold this one's
  fd_ = master;
  pid_ = pid;
  exited_ = false;
  MLOG("spawn: forkpty ok, child pid %d", int(pid));
  int flags = fcntl(fd_, F_GETFL, 0);
  fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
  return true;
}

bool Pty::read_available(std::string& out) {
  if (fd_ < 0) return false;
  char buf[65536];
  bool any = false;
  for (;;) {
    ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n > 0) { out.append(buf, size_t(n)); any = true; continue; }
    if (n == 0) return false;  // EOF: child closed the terminal
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
    if (errno == EINTR) continue;
    return any;  // EIO on Linux means the child is gone
  }
}

void Pty::write(std::string_view bytes) {
  if (fd_ < 0 || exited_) return;
  pending_input_.append(bytes);
  flush_input();
}

void Pty::flush_input() {
  if (fd_ < 0 || exited_) { pending_input_.clear(); return; }
  size_t off = 0;
  while (off < pending_input_.size()) {
    ssize_t n = ::write(fd_, pending_input_.data() + off, pending_input_.size() - off);
    if (n > 0) { off += size_t(n); continue; }
    if (n < 0 && (errno == EINTR)) continue;
    break;  // retry on the next service tick, preserving paste/Enter ordering
  }
  pending_input_.erase(0, off);
}

void Pty::resize(int w, int h) {
  if (fd_ < 0) return;
  winsize ws{};
  ws.ws_col = (unsigned short)(w > 0 ? w : 80);
  ws.ws_row = (unsigned short)(h > 0 ? h : 24);
  ioctl(fd_, TIOCSWINSZ, &ws);
}

void Pty::poll_exit() {
  if (pid_ <= 0 || exited_) return;
  int st = 0;
  pid_t r = waitpid(pid_, &st, WNOHANG);
  if (r == pid_) {
    exited_ = true;
    status_ = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    signal_ = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    if (WIFSIGNALED(st))
      MLOG("child pid %d killed by signal %d", int(pid_), WTERMSIG(st));
    else
      MLOG("child pid %d exited, code %d", int(pid_), status_);
  }
}

void Pty::terminate() {
  if (pid_ > 0 && !exited_) ::kill(pid_, SIGTERM);
}

bool Pty::reset_exited() {
  poll_exit();
  if (!exited_) return false;
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  pid_ = -1;
  exited_ = false;
  status_ = 0;
  spawn_error_.clear();
  pending_input_.clear();
  return true;
}

}  // namespace mico
