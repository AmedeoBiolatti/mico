#pragma once
#include <cstdint>

namespace mico {

// -1 means "terminal default"; otherwise 0xRRGGBB.
using Color = int32_t;
inline constexpr Color kDefaultColor = -1;

}  // namespace mico
