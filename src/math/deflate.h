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

// Raw deflate blocks (RFC 1951) of `data`, the same codes. `final` ends the
// stream; otherwise it ends as a sync flush does with its last four bytes
// (00 00 FF FF) left off, which is a permessage-deflate message (RFC 7692).
std::string deflate_raw(const uint8_t* data, size_t n, bool final);

// The bytes raw deflate blocks hold, as a permessage-deflate message carries
// them: its sync flush's last four bytes are put back first. False, with
// `out` empty, if they are not deflate or would come to more than `limit`.
bool inflate_message(const uint8_t* data, size_t n, size_t limit, std::string& out);

// The bytes a zlib stream holds, by the inflater PNG decoding already carries.
// False, with `out` empty, if it is not one.
bool zlib_inflate(const uint8_t* data, size_t n, std::vector<uint8_t>& out);

}  // namespace mico::math
