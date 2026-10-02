#include "core/procmem.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mico::proc {
namespace {

// A small /proc file, whole. /proc reports a size of zero, so it is read
// until the end rather than by its size.
bool slurp(const char* path, std::string& out) {
  out.clear();
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, size_t(n));
    if (out.size() > (1u << 20)) break;
  }
  close(fd);
  return !out.empty();
}

int64_t page_size() {
  static const int64_t n = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
  return n;
}

}  // namespace

bool Table::parse_stat(std::string_view s, pid_t* ppid, std::string* name, int64_t* rss_pages) {
  // "pid (comm) state ppid …": comm can hold spaces and parentheses, so it
  // ends at the last ')'.
  const size_t open_p = s.find('('), close_p = s.rfind(')');
  if (open_p == std::string_view::npos || close_p == std::string_view::npos || close_p < open_p) return false;
  *name = std::string(s.substr(open_p + 1, close_p - open_p - 1));
  // After it: state (3), ppid (4), … rss (24), counted from 1 over the line.
  int field = 2;
  size_t i = close_p + 1;
  int64_t vals[25] = {};
  while (i < s.size() && field < 24) {
    while (i < s.size() && s[i] == ' ') i++;
    const size_t start = i;
    while (i < s.size() && s[i] != ' ') i++;
    field++;
    if (field == 4 || field == 24) vals[field] = std::atoll(std::string(s.substr(start, i - start)).c_str());
  }
  if (field < 24) return false;
  *ppid = pid_t(vals[4]);
  *rss_pages = vals[24];
  return true;
}

void Table::read() {
  ps_.clear();
  children_.clear();
  at_.clear();
  DIR* d = opendir("/proc");
  if (!d) return;
  std::string buf, name;
  char path[64];
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const pid_t pid = pid_t(std::atoi(e->d_name));
    snprintf(path, sizeof path, "/proc/%d/stat", int(pid));
    if (!slurp(path, buf)) continue;  // gone since the listing
    pid_t ppid = 0;
    int64_t pages = 0;
    if (!parse_stat(buf, &ppid, &name, &pages)) continue;
    at_[pid] = ps_.size();
    children_.emplace(ppid, ps_.size());
    ps_.push_back(P{pid, ppid, pages * page_size(), name});
  }
  closedir(d);
}

Usage Table::tree(pid_t root) const {
  Usage u;
  if (at_.find(root) == at_.end()) return u;
  std::vector<pid_t> todo{root};
  while (!todo.empty()) {
    const pid_t pid = todo.back();
    todo.pop_back();
    const auto it = at_.find(pid);
    if (it == at_.end()) continue;
    const P& p = ps_[it->second];
    u.rss += p.rss;
    u.procs++;
    if (p.rss > u.top_rss) {
      u.top_rss = p.rss;
      u.top_pid = p.pid;
      u.top_name = p.name;
    }
    // Bounded by the table: a cycle cannot happen, but a bad read must not loop.
    if (u.procs > int(ps_.size())) break;
    const auto range = children_.equal_range(pid);
    for (auto c = range.first; c != range.second; ++c) todo.push_back(ps_[c->second].pid);
  }
  return u;
}

Memory system_memory() {
  Memory m;
  std::string buf;
  if (!slurp("/proc/meminfo", buf)) return m;
  const auto field = [&](const char* key) -> int64_t {
    const size_t at = buf.find(key);
    if (at == std::string::npos) return 0;
    return std::atoll(buf.c_str() + at + strlen(key)) * 1024;  // kB
  };
  m.total = field("MemTotal:");
  m.available = field("MemAvailable:");
  return m;
}

int64_t self_rss() {
  std::string buf, name;
  pid_t ppid = 0;
  int64_t pages = 0;
  if (!slurp("/proc/self/stat", buf) || !Table::parse_stat(buf, &ppid, &name, &pages)) return 0;
  return pages * page_size();
}

std::string bytes(int64_t n) {
  char b[32];
  if (n >= (int64_t(1) << 30)) snprintf(b, sizeof b, "%.1f GB", double(n) / double(int64_t(1) << 30));
  else snprintf(b, sizeof b, "%lld MB", (long long)(n >> 20));
  return b;
}

}  // namespace mico::proc
