#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mico::math {

// A zlib stream (RFC 1950) of `data`: LZ77 with deflate's fixed Huffman
// codes. Nowhere near zlib's ratio on text, but an equation image — one
// colour, alpha mostly zero — shrinks twenty-fold or more, and this is a
// hundred lines rather than a dependency.
std::string zlib_compress(const uint8_t* data, size_t n);

}  // namespace mico::math
