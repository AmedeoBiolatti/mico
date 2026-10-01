#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Writing JSON, for messages to clients. Everything written is valid UTF-8:
// transcript text can be cut inside a character, and a browser drops a
// WebSocket that sends it a broken one.
namespace mico::jw {

// `s` as a JSON string, quotes included. Bytes that are not UTF-8 become
// U+FFFD; control characters are escaped.
void string(std::string& out, std::string_view s);

// Builds one JSON value in `out`, placing commas itself. Keys and values are
// written in the order called; nothing is checked beyond that.
class Writer {
 public:
  explicit Writer(std::string& out) : out_(out) {}
  Writer& begin_object() { sep(); out_ += '{'; first_ = true; return *this; }
  Writer& end_object() { out_ += '}'; first_ = false; return *this; }
  Writer& begin_array() { sep(); out_ += '['; first_ = true; return *this; }
  Writer& end_array() { out_ += ']'; first_ = false; return *this; }
  // The key of the next value in an object.
  Writer& key(std::string_view k) {
    sep();
    string(out_, k);
    out_ += ':';
    first_ = true;  // the value that follows takes no comma
    return *this;
  }
  Writer& str(std::string_view v) { sep(); string(out_, v); return *this; }
  Writer& num(int64_t v) { sep(); out_ += std::to_string(v); return *this; }
  Writer& num(uint64_t v) { sep(); out_ += std::to_string(v); return *this; }
  Writer& num(int v) { return num(int64_t(v)); }
  Writer& boolean(bool v) { sep(); out_ += v ? "true" : "false"; return *this; }
  Writer& null() { sep(); out_ += "null"; return *this; }
  // A 64-bit id, as a hex string: JavaScript numbers hold only 53 bits.
  Writer& id(uint64_t v);

  Writer& field(std::string_view k, std::string_view v) { return key(k).str(v); }
  Writer& field(std::string_view k, const char* v) { return key(k).str(v); }
  Writer& field(std::string_view k, int64_t v) { return key(k).num(v); }
  Writer& field(std::string_view k, uint64_t v) { return key(k).num(v); }
  Writer& field(std::string_view k, int v) { return key(k).num(v); }
  Writer& field(std::string_view k, bool v) { return key(k).boolean(v); }

 private:
  void sep() {
    if (!first_) out_ += ',';
    first_ = false;
  }
  std::string& out_;
  bool first_ = true;
};

}  // namespace mico::jw
