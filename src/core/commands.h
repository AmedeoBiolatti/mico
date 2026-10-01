#pragma once
#include <sys/types.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "adapters/adapter.h"
#include "core/models.h"
#include "model/commands.h"

namespace mico {

// The commands built into `agent`, as its own menu lists them; see
// Adapter::builtin_commands().
std::vector<SlashCommand> builtin_commands(std::string_view agent);

// Custom commands kept as files (Adapter::file_commands()). `home` is the
// user's home directory, passed so the selftest can point it elsewhere.
std::vector<SlashCommand> file_commands(std::string_view agent, const std::string& cwd,
                                        const std::string& home);

// What each agent offers after "/", per working directory, since project
// commands and skills live in the project.
//
// Claude's list is read from claude itself rather than kept here: its
// commands, skills and plugins change with every release and every install.
// Claude answers the SDK's "initialize" control request with exactly the list
// its own menu draws from, without starting a conversation, writing a
// transcript or calling the model. mico asks once per folder, in a throwaway
// `claude -p` process, and shows the built-in list until the answer lands.
class CommandCatalog {
 public:
  ~CommandCatalog();
  // The merged list for `agent` in `cwd`, sorted by name. Starts claude's
  // probe for `cwd` when it has none, or an old one.
  const std::vector<SlashCommand>& get(const std::string& agent, const std::string& cwd);
  // Asks claude once for `cwd` if it never has, so the list is ready by the
  // time someone types "/". Cheap to call every frame.
  void warm(const std::string& agent, const std::string& cwd);
  // Drives running probes. True when one finished, and a menu showing its
  // folder should be rebuilt.
  bool pump();
  bool probing() const;
  // Changes whenever any list does; a menu rebuilds its items when it moves.
  uint64_t version() const { return version_; }

 private:
  struct Probe {
    pid_t pid = -1;
    int in = -1;   // the child's stdin, held open until it answers
    int out = -1;  // the child's stdout
    int64_t started_ms = 0;
    std::string buf;
  };
  struct Entry {
    std::vector<SlashCommand> list;
    std::vector<SlashCommand> probed;  // claude's answer, kept across rebuilds
    int64_t built_ms = 0;
    int64_t probed_ms = -1;  // -1: never asked
    Probe probe;
  };
  void start_probe(Entry& e, const Adapter& agent, const std::string& cwd);
  void end_probe(Entry& e);
  void rebuild(Entry& e, const std::string& agent, const std::string& cwd);

  std::map<std::string, Entry> entries_;  // agent + '\n' + cwd
  uint64_t version_ = 1;
};

}  // namespace mico
