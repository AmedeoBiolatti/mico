#include "core/commands.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "adapters/adapters.h"
#include "base/log.h"
#include "core/pty.h"

namespace mico {

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}




}  // namespace

std::vector<SlashCommand> builtin_commands(std::string_view agent) {
  const Adapter* a = adapter_for(agent);
  return a ? a->builtin_commands() : std::vector<SlashCommand>{};
}

std::vector<SlashCommand> file_commands(std::string_view agent, const std::string& cwd,
                                        const std::string& home) {
  std::vector<SlashCommand> out;
  if (const Adapter* a = adapter_for(agent)) a->file_commands(cwd, home, out);
  return out;
}

// ------------------------------------------------------------------ catalog

CommandCatalog::~CommandCatalog() {
  for (auto& [key, e] : entries_) end_probe(e);
}

bool CommandCatalog::probing() const {
  for (const auto& [key, e] : entries_)
    if (e.probe.pid > 0) return true;
  return false;
}

void CommandCatalog::rebuild(Entry& e, const std::string& agent, const std::string& cwd) {
  const char* home = getenv("HOME");
  std::vector<SlashCommand> list = e.probed;
  // Claude's own answer comes first, so its wording wins a name both have.
  for (auto& c : builtin_commands(agent)) list.push_back(std::move(c));
  for (auto& c : file_commands(agent, cwd, home ? home : "")) list.push_back(std::move(c));
  std::stable_sort(list.begin(), list.end(),
                   [](const SlashCommand& a, const SlashCommand& b) { return a.name < b.name; });
  list.erase(std::unique(list.begin(), list.end(),
                         [](const SlashCommand& a, const SlashCommand& b) { return a.name == b.name; }),
             list.end());
  e.list = std::move(list);
  e.built_ms = now_ms();
  version_++;
}

const std::vector<SlashCommand>& CommandCatalog::get(const std::string& agent, const std::string& cwd) {
  Entry& e = entries_[agent + "\n" + cwd];
  const int64_t now = now_ms();
  // Files are re-read now and then, so a new skill shows without a restart.
  if (e.built_ms == 0 || now - e.built_ms > 30'000) rebuild(e, agent, cwd);
  // An agent that can be asked is asked again every few minutes: an installed
  // plugin, a new command file.
  const Adapter* a = adapter_for(agent);
  if (a && !a->command_probe_argv().empty() && e.probe.pid < 0 &&
      (e.probed_ms < 0 || now - e.probed_ms > 300'000))
    start_probe(e, *a, cwd);
  return e.list;
}

void CommandCatalog::warm(const std::string& agent, const std::string& cwd) {
  const Adapter* a = adapter_for(agent);
  if (!a || a->command_probe_argv().empty() || cwd.empty()) return;
  Entry& e = entries_[agent + "\n" + cwd];
  if (e.probed_ms < 0 && e.probe.pid < 0) start_probe(e, *a, cwd);
}

void CommandCatalog::start_probe(Entry& e, const Adapter& agent, const std::string& cwd) {
  e.probed_ms = now_ms();  // an attempt, successful or not, waits its turn
  // Built before the fork: the child only execs.
  const std::vector<std::string> probe_argv = agent.command_probe_argv();
  const std::string ask = agent.command_probe_request();
  int in[2], out[2];
  if (pipe2(in, O_CLOEXEC) != 0) return;
  if (pipe2(out, O_CLOEXEC) != 0) {
    close(in[0]);
    close(in[1]);
    return;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    for (int fd : {in[0], in[1], out[0], out[1]}) close(fd);
    return;
  }
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(out[1], 1);
    const int null = open("/dev/null", O_WRONLY);
    if (null >= 0) dup2(null, 2);
    if (null > 2) close(null);
    if (chdir(cwd.c_str()) != 0) _exit(127);
    setsid();
    scrub_agent_env();
    std::vector<char*> argv;
    for (const std::string& a : probe_argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  if (write(in[1], ask.data(), ask.size()) < 0) {}
  // Closing stdin now would end the agent before it answers: it goes with the
  // process once the answer is in.
  e.probe.pid = pid;
  e.probe.in = in[1];
  e.probe.out = out[0];
  e.probe.started_ms = now_ms();
  e.probe.buf.clear();
  fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
  logs::line("commands: asking " + std::string(agent.id()) + " in " + cwd + " (pid " + std::to_string(pid) + ")");
}

void CommandCatalog::end_probe(Entry& e) {
  if (e.probe.pid <= 0) return;
  kill(-e.probe.pid, SIGTERM);
  kill(e.probe.pid, SIGTERM);
  waitpid(e.probe.pid, nullptr, 0);
  if (e.probe.in >= 0) close(e.probe.in);
  if (e.probe.out >= 0) close(e.probe.out);
  e.probe = Probe{};
}

bool CommandCatalog::pump() {
  bool finished = false;
  for (auto& [key, e] : entries_) {
    Probe& p = e.probe;
    if (p.pid <= 0) continue;
    char buf[65536];
    bool eof = false;
    for (;;) {
      const ssize_t n = read(p.out, buf, sizeof buf);
      if (n > 0) { p.buf.append(buf, size_t(n)); continue; }
      if (n == 0) eof = true;
      break;
    }
    const size_t nl = key.find('\n');
    const std::string agent = key.substr(0, nl), cwd = key.substr(nl + 1);
    const Adapter* adapter = adapter_for(agent);
    CommandProbeAnswer got;
    bool answered = false;
    if (adapter && adapter->read_command_probe(p.buf, got)) {
      logs::line("commands: " + agent + " listed " + std::to_string(got.commands.size()));
      // The same answer may name the models and their effort levels: the chip
      // pickers' list, with descriptions a screen scrape never has.
      if (!got.models.empty()) {
        logs::line("commands: " + agent + " named " + std::to_string(got.models.size()) + " models");
        set_known_models(agent, std::move(got.models));
        if (!got.efforts.empty()) set_known_efforts(agent, std::move(got.efforts));
      }
      e.probed = std::move(got.commands);
      end_probe(e);
      rebuild(e, agent, cwd);
      answered = true;
    }
    if (answered) {
      finished = true;
      continue;
    }
    if (eof || now_ms() - p.started_ms > 30'000) {
      logs::line("commands: " + agent + " gave no list (" + (eof ? "exited" : "timed out") + ")");
      end_probe(e);
      finished = true;
    }
  }
  return finished;
}

}  // namespace mico
