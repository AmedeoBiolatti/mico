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

#include "base/log.h"
#include "base/process.h"
#include "core/store.h"

namespace mico::scope {
namespace {

// systemd-run, once a trial scope has run: a user manager that is not there
// (a container, an ssh session without lingering) fails here, not under an
// agent.
const std::string& systemd_run() {
  static const std::string path = [] {
    std::string bin = proc::find_program("systemd-run");
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

bool enabled() { return setting_on("scopes", true); }

void set_enabled(bool on) { set_setting_on("scopes", on); }

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

}  // namespace mico::scope
