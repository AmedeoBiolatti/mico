#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mico::math {

// A zlib stream (RFC 1950) of `data`: LZ77 with deflate's fixed Huffman
// codes. Nowhere near zlib's ratio on text, but an equation image — one
// colour, alpha mostly zero — shrinks twenty-fold or more, and this is a
// hundred lines rather than a dependency.
std::string zlib_compress(const uint8_t* data, size_t n);

// The bytes a zlib stream holds, by the inflater PNG decoding already carries.
// False, with `out` empty, if it is not one.
bool zlib_inflate(const uint8_t* data, size_t n, std::vector<uint8_t>& out);

}  // namespace mico::math
