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

#include "base/process.h"

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
  proc::Options opt;
  opt.cap = 256u << 20;
  // Enough: stop reading and let git go.
  opt.enough = [cap](const std::string& got) { return size_t(std::count(got.begin(), got.end(), '\0')) >= cap; };
  const proc::Result r = proc::capture({"git", "-c", "core.fsmonitor=false", "-C", cwd, "ls-files", "-z", "--cached",
                                        "--others", "--exclude-standard"},
                                       opt);
  if (!r.cut && !r.ok()) return false;
  const std::string& buf = r.out;
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
