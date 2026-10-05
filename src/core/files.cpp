#include "core/files.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <set>
#include <string_view>

namespace mico {

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// `git ls-files` for `cwd`, NUL-separated; false when it is not a work tree
// or git is missing.
bool git_files(const std::string& cwd, size_t cap, std::vector<std::string>& out) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return false;
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    dup2(fds[1], 1);
    const int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 2);
      if (null > 2) close(null);
    }
    const char* argv[] = {"git", "-c", "core.fsmonitor=false", "-C", cwd.c_str(), "ls-files", "-z", "--cached", "--others",
                          "--exclude-standard", nullptr};
    execvp(argv[0], const_cast<char* const*>(argv));
    _exit(127);
  }
  close(fds[1]);
  std::string buf;
  char chunk[65536];
  bool full = false;
  for (;;) {
    const ssize_t n = read(fds[0], chunk, sizeof chunk);
    if (n <= 0) break;
    buf.append(chunk, size_t(n));
    // Enough: stop reading and let git go.
    if (size_t(std::count(buf.begin(), buf.end(), '\0')) >= cap) {
      full = true;
      break;
    }
  }
  close(fds[0]);
  if (full) kill(pid, SIGTERM);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!full && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) return false;
  for (size_t i = 0; i < buf.size() && out.size() < cap;) {
    const size_t e = buf.find('\0', i);
    if (e == std::string::npos) break;
    if (e > i) out.emplace_back(buf, i, e - i);
    i = e + 1;
  }
  return true;
}

bool skipped_dir(std::string_view name) {
  static constexpr std::string_view kSkip[] = {"node_modules", "__pycache__", "target", "build",
                                               "dist", "venv", "vendor"};
  if (name.starts_with('.')) return true;
  return std::find(std::begin(kSkip), std::end(kSkip), name) != std::end(kSkip);
}

// Breadth first, so the shallow files — the ones a person names — are in
// even when the cap cuts the walk short.
void walk_files(const std::string& cwd, size_t cap, std::vector<std::string>& out) {
  const int64_t deadline = now_ms() + 300;
  std::deque<std::string> dirs{""};
  while (!dirs.empty() && out.size() < cap && now_ms() < deadline) {
    const std::string rel = std::move(dirs.front());
    dirs.pop_front();
    DIR* d = opendir((rel.empty() ? cwd : cwd + "/" + rel).c_str());
    if (!d) continue;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
      std::string_view n(e->d_name);
      if (n == "." || n == "..") continue;
      bool dir = e->d_type == DT_DIR;
      if (e->d_type == DT_UNKNOWN) {
        struct stat st{};
        dir = stat((cwd + "/" + rel + std::string(n)).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      }
      if (dir) {
        if (!skipped_dir(n)) dirs.push_back(rel + std::string(n) + "/");
      } else if (!n.starts_with('.')) {
        names.emplace_back(rel + std::string(n));
      }
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
      if (out.size() >= cap) break;
      out.push_back(std::move(n));
    }
  }
}

}  // namespace

std::vector<std::string> list_files(const std::string& cwd, size_t cap) {
  std::vector<std::string> files;
  if (!git_files(cwd, cap, files)) {
    files.clear();
    walk_files(cwd, cap, files);
  }
  // Every folder on the way to a file can be named too.
  std::set<std::string> dirs;
  for (const auto& f : files)
    for (size_t s = f.find('/'); s != std::string::npos; s = f.find('/', s + 1)) dirs.insert(f.substr(0, s + 1));
  std::vector<std::string> out;
  out.reserve(files.size() + dirs.size());
  out.insert(out.end(), dirs.begin(), dirs.end());
  out.insert(out.end(), files.begin(), files.end());
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

const std::vector<std::string>& FileIndex::get(const std::string& cwd) {
  Entry& e = entries_[cwd];
  const int64_t now = now_ms();
  if (e.listed_ms == 0 || now - e.listed_ms > 5000) {
    e.paths = list_files(cwd);
    e.listed_ms = now;
    version_++;
  }
  return e.paths;
}

}  // namespace mico
