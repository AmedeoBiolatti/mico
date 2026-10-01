#pragma once
#include <sys/types.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/models.h"

namespace mico {

// One entry of an agent's "/" menu.
struct SlashCommand {
  std::string name;         // without the slash: "compact", "frontend:review"
  std::string description;  // one line
  std::string hint;         // the arguments it takes: "<model>", "[name]"
};

// The commands built into `agent`, as its own menu lists them. For claude this
// is only the interactive set: everything else comes from asking claude.
std::vector<SlashCommand> builtin_commands(std::string_view agent);

// Parses claude's answer to an SDK "initialize" request — the line holding
// the control_response — into commands. Exposed for the selftest.
std::vector<SlashCommand> parse_claude_commands(std::string_view line);
// The same answer's model catalog: what "/model <value>" takes, the shown
// name, the description, and (in `efforts`) the union of the effort levels
// the models report, in claude's order.
std::vector<ModelOption> parse_claude_models(std::string_view line,
                                             std::vector<std::string>* efforts = nullptr);

// Custom commands kept as files: pi's prompt templates and skills. `home` is
// the user's home directory, passed so the selftest can point it elsewhere.
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
  void start_probe(Entry& e, const std::string& cwd);
  void end_probe(Entry& e);
  void rebuild(Entry& e, const std::string& agent, const std::string& cwd);

  std::map<std::string, Entry> entries_;  // agent + '\n' + cwd
  uint64_t version_ = 1;
};

}  // namespace mico
