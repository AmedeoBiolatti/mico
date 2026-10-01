#include "core/edits.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "adapters/adapters.h"

namespace mico {

bool may_have_changes(std::string_view agent, std::string_view raw) {
  const Adapter* a = adapter_for(agent);
  return a && a->may_have_changes(raw);
}

std::vector<LineChanges> read_changes(std::string_view agent, std::string_view raw, std::string_view cwd, bool text) {
  std::vector<LineChanges> out;
  const Adapter* a = adapter_for(agent);
  if (a && a->may_have_changes(raw)) a->read_changes(raw, cwd, text, out);
  return out;
}

bool load_change(const std::string& transcript, std::string_view agent, std::string_view cwd, uint64_t offset,
                 const std::string& file, FileChange& out) {
  const int fd = open(transcript.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::string line;
  char buf[65536];
  uint64_t at = offset;
  for (;;) {
    const ssize_t n = pread(fd, buf, sizeof buf, off_t(at));
    if (n <= 0) break;
    const void* nl = memchr(buf, '\n', size_t(n));
    if (nl) {
      line.append(buf, size_t(static_cast<const char*>(nl) - buf));
      break;
    }
    line.append(buf, size_t(n));
    at += uint64_t(n);
    if (line.size() > (256u << 20)) break;
  }
  close(fd);
  for (LineChanges& lc : read_changes(agent, line, cwd, true))
    for (FileChange& c : lc.changes)
      if (c.file == file || c.file.empty()) {
        out = std::move(c);
        if (out.file.empty()) out.file = file;
        return true;
      }
  return false;
}

}  // namespace mico
