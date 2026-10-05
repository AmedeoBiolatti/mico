#include "core/scope.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "base/fs.h"
#include "base/log.h"
#include "core/store.h"

namespace mico::scope {
namespace {

std::string find_on_path(const char* name) {
  const char* env = getenv("PATH");
  std::string_view dirs = env ? env : "/usr/bin:/bin";
  while (!dirs.empty()) {
    const size_t colon = dirs.find(':');
    const std::string dir(dirs.substr(0, colon));
    dirs = colon == std::string_view::npos ? std::string_view() : dirs.substr(colon + 1);
    const std::string cand = (dir.empty() ? "." : dir) + "/" + name;
    if (access(cand.c_str(), X_OK) == 0) return cand;
  }
  return {};
}

// systemd-run, once a trial scope has run: a user manager that is not there
// (a container, an ssh session without lingering) fails here, not under an
// agent.
const std::string& systemd_run() {
  static const std::string path = [] {
    std::string bin = find_on_path("systemd-run");
    if (bin.empty()) {
      MLOG("scopes: no systemd-run; agents share the daemon's cgroup");
      return std::string();
    }
    const std::string unit = "--unit=mico-trial-" + std::to_string(getpid());
    const pid_t pid = fork();
    if (pid < 0) return std::string();
    if (pid == 0) {
      const int null = open("/dev/null", O_RDWR);
      if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
      }
      execl(bin.c_str(), bin.c_str(), "--user", "--scope", "--quiet", "--collect", unit.c_str(), "--", "true",
            static_cast<char*>(nullptr));
      _exit(127);
    }
    int st = 0;
    for (int i = 0; i < 300; i++) {  // three seconds, at most
      if (waitpid(pid, &st, WNOHANG) == pid) {
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
          MLOG("scopes: each agent runs in a systemd scope of its own (%s)", bin.c_str());
          return bin;
        }
        MLOG("scopes: a trial systemd-run --user --scope failed (status %d); agents share the daemon's cgroup", st);
        return std::string();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    MLOG("scopes: systemd-run did not answer; agents share the daemon's cgroup");
    return std::string();
  }();
  return path;
}

}  // namespace

bool enabled() {
  std::string buf;
  return !fs::read_prefix(config_dir() + "/scopes", 64, buf).starts_with("off");
}

void set_enabled(bool on) {
  mkdir(config_dir().c_str(), 0700);
  if (FILE* f = fopen((config_dir() + "/scopes").c_str(), "w")) {
    fputs(on ? "on\n" : "off\n", f);
    fclose(f);
  }
}

bool available() {
  if (const char* e = getenv("MICO_SCOPES"); e && std::string_view(e) == "0") return false;
  return enabled() && !systemd_run().empty();
}

std::string unit_name(const std::string& text) {
  std::string out = "mico-";
  for (const char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' ||
                    c == '_' || c == '.' || c == '-';
    out.push_back(ok ? c : '-');
  }
  if (out.size() > 200) out.resize(200);
  return out;
}

std::vector<std::string> wrap(const std::vector<std::string>& argv, const std::string& unit,
                              const std::string& description) {
  if (argv.empty() || unit.empty() || !available()) return argv;
  std::vector<std::string> out = {systemd_run(), "--user",          "--scope",
                                  "--quiet",     "--collect",       "--unit=" + unit,
                                  "--description=" + description, "--"};
  out.insert(out.end(), argv.begin(), argv.end());
  return out;
}

std::string unit_of(pid_t pid) {
  std::string buf;
  const std::string_view s = fs::read_prefix("/proc/" + std::to_string(pid) + "/cgroup", 4096, buf);
  // cgroup v2: "0::/user.slice/…/app.slice/mico-claude-3-4242.scope".
  size_t e = s.find('\n');
  std::string_view line = s.substr(0, e);
  const size_t slash = line.rfind('/');
  return slash == std::string_view::npos ? std::string() : std::string(line.substr(slash + 1));
}

}  // namespace mico::scope
