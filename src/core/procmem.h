#pragma once
#include <sys/types.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// How much memory processes hold, read from /proc: for telling which agent's
// work is filling the machine before something is killed for it.
namespace mico::proc {

// A process and everything it started, added up.
struct Usage {
  int64_t rss = 0;       // resident bytes, all of them
  int procs = 0;
  pid_t top_pid = 0;     // the one holding the most
  int64_t top_rss = 0;
  std::string top_name;  // its command name, as /proc has it ("xc121")
};

// Every process's parent and resident memory, read from /proc at once.
class Table {
 public:
  void read();
  // `root` and its descendants. Zero when `root` is gone.
  Usage tree(pid_t root) const;
  // From a `/proc/<pid>/stat` line: its parent, name and resident pages.
  // False when it is not one.
  static bool parse_stat(std::string_view stat, pid_t* ppid, std::string* name, int64_t* rss_pages);

 private:
  struct P {
    pid_t pid, ppid;
    int64_t rss;
    std::string name;
  };
  std::vector<P> ps_;
  std::unordered_multimap<pid_t, size_t> children_;
  std::unordered_map<pid_t, size_t> at_;
};

// The machine's memory, from /proc/meminfo, in bytes.
struct Memory {
  int64_t total = 0, available = 0;
};
Memory system_memory();
// This process's resident bytes.
int64_t self_rss();

// "6.8 GB", "412 MB".
std::string bytes(int64_t n);

}  // namespace mico::proc
