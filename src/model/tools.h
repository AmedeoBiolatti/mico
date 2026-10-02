#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace mico {

// Where an adapter reports the tool calls a transcript records. The activity
// index behind it times them, classifies them and keeps them; the adapter
// only says what each record means.
class ToolSink {
 public:
  virtual ~ToolSink() = default;
  // A call, timed by the result that answers it. `offset` is where its
  // transcript line starts; times are unix milliseconds.
  virtual void call(std::string id, int64_t at_ms, uint64_t offset, std::string tool, std::string command) = 0;
  // Whether what a call printed is wanted: the result answering call `id`,
  // or a run of `command`. Reading a result's text costs, and almost none is
  // ever wanted, so an adapter asks before it decodes one.
  virtual bool wants_output(const std::string& id, std::string_view command) const { return false; }
  // The result answering call `id`. `exact_ms` is the agent's own measure of
  // the call, when it gives one; -1 times it from the call. `output` is what
  // it printed, when wants_output() asked for it (and the agent kept it).
  virtual void result(const std::string& id, int64_t at_ms, bool failed, int64_t exact_ms = -1,
                      std::string_view output = {}) = 0;
  // A run recorded whole in one record. `reading` when the agent itself says
  // the command only read or searched.
  virtual void run(int64_t start_ms, int64_t dur_ms, uint64_t offset, std::string tool, std::string command,
                   bool failed, bool reading = false, std::string_view output = {}) = 0;
};

}  // namespace mico
