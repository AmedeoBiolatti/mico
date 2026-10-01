#pragma once
#include <string>
#include <utility>
#include <vector>

namespace mico {

// One entry of an agent's "/" menu.
struct SlashCommand {
  std::string name;         // without the slash: "compact", "frontend:review"
  std::string description;  // one line
  std::string hint;         // the arguments it takes: "<model>", "[name]"
};

// One entry of an agent's model picker.
struct ModelOption {
  std::string value;    // exactly what "/model <value>" takes
  std::string label;    // what the chip menu shows
  std::string detail;   // the agent's one-line description, often empty
  std::string resolved; // the concrete model id behind an alias, often empty
  bool current = false;
};

// What an agent says when asked what it offers (Adapter::command_probe_argv).
struct CommandProbeAnswer {
  std::vector<SlashCommand> commands;
  std::vector<ModelOption> models;   // empty when it does not say
  std::vector<std::string> efforts;  // the effort levels its models take, in its order
};

// How mico sets one of the agent's state fields, shown as a chip: model,
// effort, permission mode.
struct ChipControl {
  enum class Source {
    Fixed,    // `values` as given
    Models,   // what the agent said its models are (known_models)
    Efforts,  // what it said its effort levels are (known_efforts), else `values`
  };
  // Values mico offers itself, each selected by sending `set_prefix` + value.
  std::vector<std::string> values;
  Source source = Source::Fixed;
  std::string set_prefix;
  // A fixed ring the agent steps through with one key, rather than a command
  // taking a value: each entry's value and how it reads, in the ring's order.
  std::vector<std::pair<std::string, std::string>> ring;
  std::string ring_key;
  // The command that opens the agent's own picker, when mico offers no values;
  // empty when there is no way to set it from mico.
  std::string picker;
};

}  // namespace mico
