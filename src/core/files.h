#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mico {

// The files and folders under `cwd` that an "@" can name, as paths relative
// to it, folders ending in '/'. Inside a git work tree this is what git
// tracks plus what it does not ignore, so a build directory stays out;
// elsewhere a walk that skips hidden and dependency folders. Stops at `cap`
// entries: a picker over a home directory has to stay quick.
std::vector<std::string> list_files(const std::string& cwd, size_t cap = 60000);

// list_files, kept per folder and re-listed when it is asked for again after
// a few seconds, so a file the agent just wrote can be named.
class FileIndex {
 public:
  const std::vector<std::string>& get(const std::string& cwd);
  // Changes whenever a list does.
  uint64_t version() const { return version_; }

 private:
  struct Entry {
    std::vector<std::string> paths;
    int64_t listed_ms = 0;
  };
  std::map<std::string, Entry> entries_;
  uint64_t version_ = 1;
};

}  // namespace mico
