#pragma once
#include <bit>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mico {

// Private, versioned state passed through an anonymous fd at exec. Encode
// fields, never C++ object layouts or pointers: the receiving binary may be
// built with a different compiler. Change the version if the schema changes.
inline constexpr int kHandoffVersion = 1;
class Handoff {
 public:
  Handoff() = default;
  explicit Handoff(std::string_view data) : reading_(true), input_(data) {}
  bool reading() const { return reading_; }
  bool ok() const { return ok_; }
  bool finished() const { return ok_ && at_ == input_.size(); }
  const std::string& data() const { return output_; }
  void fail() { ok_ = false; }

  template<class... T> void fields(T&... v) { (field(v), ...); }
  template<class C, class F> void items(C& c, F f) {
    uint64_t n = c.size();
    field(n);
    if (!ok_) return;
    if (n > (1u << 20)) { fail(); return; }
    if (reading_) c.resize(size_t(n));
    for (auto& v : c) { if (!ok_) break; f(v); }
  }

 private:
  template<class T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
  void field(T& v) {
    uint64_t n = uint64_t(v);
    if (reading_) {
      if (!ok_ || input_.size() - at_ < 8) { fail(); return; }
      n = 0;
      for (int i = 0; i < 8; i++) n |= uint64_t(uint8_t(input_[at_++])) << (i * 8);
      v = T(n);
    } else {
      for (int i = 0; i < 8; i++) output_ += char(n >> (i * 8));
    }
  }
  void field(double& v) {
    uint64_t n = std::bit_cast<uint64_t>(v);
    field(n);
    if (reading_) v = std::bit_cast<double>(n);
  }
  void field(std::string& v) {
    uint64_t n = v.size();
    field(n);
    if (!ok_) return;
    if (reading_) {
      if (n > input_.size() - at_) { fail(); return; }
      v.assign(input_.substr(at_, size_t(n)));
      at_ += size_t(n);
    } else output_ += v;
  }
  template<class T> void field(std::vector<T>& v) { items(v, [&](T& x) { field(x); }); }
  template<class T> void field(std::deque<T>& v) { items(v, [&](T& x) { field(x); }); }
  template<class T> requires requires(T& v, Handoff& h) { v.handoff(h); }
  void field(T& v) { v.handoff(*this); }

  bool reading_ = false, ok_ = true;
  size_t at_ = 0;
  std::string_view input_;
  std::string output_;
};

}  // namespace mico
