#include "math/deflate.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

namespace mico::math {
namespace {

// Deflate's bit stream: values go in least significant bit first, Huffman
// codes most significant bit first.
struct Bits {
  std::string& out;
  uint32_t acc = 0;
  int n = 0;
  void put(uint32_t v, int bits) {
    acc |= v << n;
    n += bits;
    while (n >= 8) {
      out.push_back(char(acc & 0xFF));
      acc >>= 8;
      n -= 8;
    }
  }
  void code(uint32_t c, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((c >> i) & 1u) << (len - 1 - i);
    put(r, len);
  }
  void flush() {
    if (n > 0) out.push_back(char(acc & 0xFF));
    acc = 0;
    n = 0;
  }
};

// The fixed literal/length code (RFC 1951, 3.2.6).
void symbol(Bits& b, int v) {
  if (v < 144) b.code(uint32_t(0x30 + v), 8);
  else if (v < 256) b.code(uint32_t(0x190 + (v - 144)), 9);
  else if (v < 280) b.code(uint32_t(v - 256), 7);
  else b.code(uint32_t(0xC0 + (v - 280)), 8);
}

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void match(Bits& b, int len, int dist) {
  int li = 28;
  while (kLenBase[li] > len) li--;
  symbol(b, 257 + li);
  if (kLenExtra[li]) b.put(uint32_t(len - kLenBase[li]), kLenExtra[li]);
  int di = 29;
  while (kDistBase[di] > dist) di--;
  b.code(uint32_t(di), 5);
  if (kDistExtra[di]) b.put(uint32_t(dist - kDistBase[di]), kDistExtra[di]);
}

}  // namespace

std::string zlib_compress(const uint8_t* p, size_t n) {
  std::string out("\x78\x01", 2);
  out.reserve(n / 8 + 64);
  Bits b{out};
  b.put(1, 1);  // the final block
  b.put(1, 2);  // fixed Huffman codes

  constexpr int kHashBits = 15;
  constexpr size_t kWindow = 32768;
  std::vector<int32_t> head(size_t(1) << kHashBits, -1);
  const auto hash = [&](size_t i) {
    const uint32_t v = uint32_t(p[i]) | uint32_t(p[i + 1]) << 8 | uint32_t(p[i + 2]) << 16;
    return (v * 2654435761u) >> (32 - kHashBits);
  };

  size_t i = 0;
  while (i < n) {
    int best = 0, dist = 0;
    if (i + 3 <= n) {
      const size_t most = std::min<size_t>(258, n - i);
      const auto try_at = [&](size_t cand) {
        if (cand >= i || i - cand > kWindow) return;
        // Eight bytes at a time; an image's long runs are most of the work.
        size_t l = 0;
        while (l + 8 <= most) {
          uint64_t x, y;
          memcpy(&x, p + cand + l, 8);
          memcpy(&y, p + i + l, 8);
          if (x != y) {
            if constexpr (std::endian::native == std::endian::little)
              l += size_t(__builtin_ctzll(x ^ y) >> 3);
            break;  // elsewhere the byte loop below finds it
          }
          l += 8;
        }
        while (l < most && p[cand + l] == p[i + l]) l++;
        if (int(l) > best) {
          best = int(l);
          dist = int(i - cand);
        }
      };
      // One pixel back first: an image's runs of identical pixels. A run
      // that long needs no search.
      if (i >= 4) try_at(i - 4);
      const uint32_t h = hash(i);
      if (best < 258 && head[h] >= 0) try_at(size_t(head[h]));
      head[h] = int32_t(i);
    }
    if (best >= 3) {
      match(b, best, dist);
      // Positions inside a short match are worth finding again; inside a long
      // run they only cost time (zlib's fast levels skip them the same way).
      if (best < 32)
        for (size_t k = i + 1; k < i + size_t(best) && k + 3 <= n; k++) head[hash(k)] = int32_t(k);
      else if (i + size_t(best) + 3 <= n)
        head[hash(i + size_t(best) - 1)] = int32_t(i + size_t(best) - 1);
      i += size_t(best);
    } else {
      symbol(b, p[i]);
      i++;
    }
  }
  symbol(b, 256);
  b.flush();

  // Adler-32, reduced once per 5536 bytes (the most that cannot overflow,
  // rounded down to whole blocks). Thirty-two bytes at a time: the block's
  // sum and weighted sum are the same arithmetic as byte by byte, but they
  // vectorize.
  uint32_t s1 = 1, s2 = 0;
  for (size_t k = 0; k < n;) {
    const size_t end = std::min(n, k + 5536);
    for (; k + 32 <= end; k += 32) {
      uint32_t a = 0, w = 0;
      for (int j = 0; j < 32; j++) {
        a += p[k + size_t(j)];
        w += uint32_t(32 - j) * p[k + size_t(j)];
      }
      s2 += 32 * s1 + w;
      s1 += a;
    }
    for (; k < end; k++) {
      s1 += p[k];
      s2 += s1;
    }
    s1 %= 65521;
    s2 %= 65521;
  }
  const uint32_t adler = (s2 << 16) | s1;
  out.push_back(char(adler >> 24));
  out.push_back(char(adler >> 16));
  out.push_back(char(adler >> 8));
  out.push_back(char(adler));
  return out;
}

}  // namespace mico::math
