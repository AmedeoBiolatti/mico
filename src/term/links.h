#pragma once
#include <cstdint>
#include <string_view>

// Hyperlinks, as cells carry them: a small id standing for a URL. The chat
// interns a URL when it lays text out, the painter stamps the id on the cells
// it draws, and the encoder writes OSC 8 around them, so the terminal itself
// knows the text is a link.
namespace mico::links {

// The id for `url`, 0 when it is not something a link may point at (no
// scheme mico opens, or control characters that would break the escape).
uint16_t intern(std::string_view url);
std::string_view url(uint16_t id);
// Moves when the table had to be emptied: ids laid out before are stale.
uint64_t generation();

}  // namespace mico::links
