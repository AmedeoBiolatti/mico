#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "vt/events.h"

namespace mico {

// Turns a byte stream from a terminal into events. Owns only its partial-
// sequence buffer, so the daemon can run one per attached client without a tty
// of its own.
class InputDecoder {
 public:
  void feed(std::string_view bytes) { pending_.append(bytes); }
  bool empty() const { return pending_.empty(); }
  void clear() { pending_.clear(); }

  // Returns the next complete event, or nullopt when more bytes are needed.
  std::optional<InputEvent> next();

  // A bare Escape is indistinguishable from the start of an escape sequence
  // until either more bytes arrive or enough time passes without them. Call
  // this when input has gone quiet to resolve it as a real Escape key.
  std::optional<InputEvent> flush();
  // True while a lone ESC is waiting to be resolved; poll on a short timeout so
  // pressing Escape does not feel laggy.
  bool pending_escape() const { return pending_.size() == 1 && pending_[0] == 0x1b; }

 private:
  std::optional<InputEvent> parse(size_t& consumed);
  std::string pending_;
};

}  // namespace mico
