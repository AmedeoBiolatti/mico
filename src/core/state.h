#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mico {

// What an agent reports about itself: model, effort, mode, whatever else it
// happens to write. Deliberately a list rather than fixed fields — claude
// reports a permission mode, codex reports an approval policy and a sandbox,
// and a future agent will report something neither of them has.
struct StateField {
  std::string key;    // stable id, used to find the control for it
  std::string label;  // what to show
  std::string value;
};

struct SessionState {
  std::vector<StateField> fields;
  // Line these values came from. A transcript is read from both ends, so an
  // older record must not overwrite a newer one.
  uint32_t from_line = 0;

  void set(std::string_view key, std::string_view label, std::string_view value) {
    if (value.empty()) return;
    for (auto& f : fields)
      if (f.key == key) {
        f.value = value;
        return;
      }
    fields.push_back(StateField{std::string(key), std::string(label), std::string(value)});
  }
  // Declares a field with no value yet, so the chip shows before the agent has
  // reported anything. A later set() fills it in.
  void declare(std::string_view key, std::string_view label) {
    for (const auto& f : fields)
      if (f.key == key) return;
    fields.push_back(StateField{std::string(key), std::string(label), {}});
  }
  const std::string* find(std::string_view key) const {
    for (const auto& f : fields)
      if (f.key == key) return &f.value;
    return nullptr;
  }
  bool empty() const { return fields.empty(); }
  void clear() { fields.clear(); from_line = 0; }
};

}  // namespace mico
