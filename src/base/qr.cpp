#include "base/qr.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>

namespace mico::qr {
namespace {

// Level M, by version 1..40.
constexpr int kEccPerBlock[41] = {0,  10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26,
                                  28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28};
constexpr int kBlocks[41] = {0, 1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,  9,  10, 10, 11, 13, 14, 16, 17,
                             17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49};

int raw_modules(int ver) {
  int r = (16 * ver + 128) * ver + 64;
  if (ver >= 2) {
    const int n = ver / 7 + 2;
    r -= (25 * n - 10) * n - 55;
    if (ver >= 7) r -= 36;
  }
  return r;
}

int data_codewords(int ver) { return raw_modules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

// GF(256) with the QR polynomial 0x11D.
uint8_t gf_mul(uint8_t x, uint8_t y) {
  int z = 0;
  for (int i = 7; i >= 0; i--) {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return uint8_t(z);
}

std::vector<uint8_t> ecc_of(const std::vector<uint8_t>& data, int degree) {
  std::vector<uint8_t> gen(size_t(degree), 0);
  gen.back() = 1;
  uint8_t root = 1;
  for (int i = 0; i < degree; i++) {
    for (size_t j = 0; j < gen.size(); j++) {
      gen[j] = gf_mul(gen[j], root);
      if (j + 1 < gen.size()) gen[j] ^= gen[j + 1];
    }
    root = gf_mul(root, 2);
  }
  std::vector<uint8_t> rem(size_t(degree), 0);
  for (uint8_t b : data) {
    const uint8_t factor = b ^ rem[0];
    rem.erase(rem.begin());
    rem.push_back(0);
    for (size_t i = 0; i < rem.size(); i++) rem[i] ^= gf_mul(gen[i], factor);
  }
  return rem;
}

struct Grid {
  int n;
  std::vector<uint8_t> m, fn;  // module, and whether it is a function pattern
  explicit Grid(int size) : n(size), m(size_t(size * size), 0), fn(size_t(size * size), 0) {}
  uint8_t& at(int x, int y) { return m[size_t(y * n + x)]; }
  uint8_t get(int x, int y) const { return m[size_t(y * n + x)]; }
  void set_fn(int x, int y, bool dark) {
    if (x < 0 || y < 0 || x >= n || y >= n) return;
    m[size_t(y * n + x)] = dark;
    fn[size_t(y * n + x)] = 1;
  }
};

void finder(Grid& g, int cx, int cy) {
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++) {
      const int d = std::max(std::abs(dx), std::abs(dy));
      g.set_fn(cx + dx, cy + dy, d != 2 && d != 4);
    }
}

void alignment(Grid& g, int cx, int cy) {
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) g.set_fn(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

std::vector<int> align_positions(int ver) {
  if (ver == 1) return {};
  const int n = ver / 7 + 2;
  const int size = ver * 4 + 17;
  const int step = ver == 32 ? 26 : (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
  std::vector<int> r;
  r.resize(size_t(n));
  r[0] = 6;
  for (int i = n - 1, pos = size - 7; i >= 1; i--, pos -= step) r[size_t(i)] = pos;
  return r;
}

void format_bits(Grid& g, int mask) {
  const int data = /* level M */ 0 << 3 | mask;
  int rem = data;
  for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
  const int bits = (data << 10 | rem) ^ 0x5412;
  const auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
  for (int i = 0; i <= 5; i++) g.set_fn(8, i, bit(i));
  g.set_fn(8, 7, bit(6));
  g.set_fn(8, 8, bit(7));
  g.set_fn(7, 8, bit(8));
  for (int i = 9; i < 15; i++) g.set_fn(14 - i, 8, bit(i));
  for (int i = 0; i < 8; i++) g.set_fn(g.n - 1 - i, 8, bit(i));
  for (int i = 8; i < 15; i++) g.set_fn(8, g.n - 15 + i, bit(i));
  g.set_fn(8, g.n - 8, true);
}

Grid patterns(int ver) {
  Grid g(ver * 4 + 17);
  for (int i = 0; i < g.n; i++) {
    g.set_fn(6, i, i % 2 == 0);
    g.set_fn(i, 6, i % 2 == 0);
  }
  finder(g, 3, 3);
  finder(g, g.n - 4, 3);
  finder(g, 3, g.n - 4);
  const std::vector<int> pos = align_positions(ver);
  const int k = int(pos.size());
  for (int i = 0; i < k; i++)
    for (int j = 0; j < k; j++)
      if (!((i == 0 && j == 0) || (i == 0 && j == k - 1) || (i == k - 1 && j == 0))) alignment(g, pos[size_t(i)], pos[size_t(j)]);
  format_bits(g, 0);  // placed again once the mask is known
  if (ver >= 7) {
    int rem = ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const int bits = ver << 12 | rem;
    for (int i = 0; i < 18; i++) {
      const bool b = ((bits >> i) & 1) != 0;
      const int a = g.n - 11 + i % 3, c = i / 3;
      g.set_fn(a, c, b);
      g.set_fn(c, a, b);
    }
  }
  return g;
}

void place(Grid& g, const std::vector<uint8_t>& words) {
  size_t i = 0;
  for (int right = g.n - 1; right >= 1; right -= 2) {
    if (right == 6) right = 5;
    for (int vert = 0; vert < g.n; vert++)
      for (int j = 0; j < 2; j++) {
        const int x = right - j;
        const bool up = ((right + 1) & 2) == 0;
        const int y = up ? g.n - 1 - vert : vert;
        if (g.fn[size_t(y * g.n + x)] || i >= words.size() * 8) continue;
        g.at(x, y) = (words[i >> 3] >> (7 - (i & 7))) & 1;
        i++;
      }
  }
}

void apply_mask(Grid& g, int mask) {
  for (int y = 0; y < g.n; y++)
    for (int x = 0; x < g.n; x++) {
      bool inv = false;
      switch (mask) {
        case 0: inv = (x + y) % 2 == 0; break;
        case 1: inv = y % 2 == 0; break;
        case 2: inv = x % 3 == 0; break;
        case 3: inv = (x + y) % 3 == 0; break;
        case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
        case 5: inv = x * y % 2 + x * y % 3 == 0; break;
        case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
        default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
      }
      if (inv && !g.fn[size_t(y * g.n + x)]) g.at(x, y) ^= 1;
    }
}

int penalty(const Grid& g) {
  const int n = g.n;
  int total = 0;
  for (int pass = 0; pass < 2; pass++) {
    const auto at = [&](int a, int b) { return pass == 0 ? g.get(b, a) : g.get(a, b); };
    for (int a = 0; a < n; a++) {
      int run = 1;
      for (int b = 1; b <= n; b++) {
        if (b < n && at(a, b) == at(a, b - 1)) run++;
        else {
          if (run >= 5) total += 3 + (run - 5);
          run = 1;
        }
      }
      // 1:1:3:1:1 with four light modules to one side, the edges counting as light.
      for (int b = -4; b + 6 < n + 4; b++) {
        const auto v = [&](int k) { return k < 0 || k >= n ? 0 : int(at(a, k)); };
        if (v(b + 4) == 1 && v(b + 5) == 0 && v(b + 6) == 1 && v(b + 7) == 1 && v(b + 8) == 1 && v(b + 9) == 0 && v(b + 10) == 1) {
          bool left = true, right = true;
          for (int k = 0; k < 4; k++) {
            if (v(b + k) != 0) left = false;
            if (v(b + 11 + k) != 0) right = false;
          }
          total += 40 * (int(left) + int(right));
        }
      }
    }
  }
  for (int y = 0; y + 1 < n; y++)
    for (int x = 0; x + 1 < n; x++) {
      const uint8_t c = g.get(x, y);
      if (c == g.get(x + 1, y) && c == g.get(x, y + 1) && c == g.get(x + 1, y + 1)) total += 3;
    }
  int dark = 0;
  for (uint8_t v : g.m) dark += v;
  const int k = (std::abs(dark * 20 - n * n * 10) + n * n - 1) / (n * n) - 1;
  return total + k * 10;
}

}  // namespace

Code encode(std::string_view text, int mask) {
  int ver = 1;
  for (; ver <= 40; ver++) {
    const int bits = 4 + (ver <= 9 ? 8 : 16) + 8 * int(text.size());
    if (bits <= data_codewords(ver) * 8) break;
  }
  if (ver > 40) return {};

  std::vector<bool> bits;
  const auto put = [&](unsigned v, int len) {
    for (int i = len - 1; i >= 0; i--) bits.push_back(((v >> i) & 1) != 0);
  };
  put(4, 4);
  put(unsigned(text.size()), ver <= 9 ? 8 : 16);
  for (unsigned char c : text) put(c, 8);
  const size_t capacity = size_t(data_codewords(ver)) * 8;
  for (int i = 0; i < 4 && bits.size() < capacity; i++) bits.push_back(false);
  while (bits.size() % 8) bits.push_back(false);
  std::vector<uint8_t> data(capacity / 8);
  for (size_t i = 0; i < bits.size(); i++) data[i >> 3] |= uint8_t(bits[i]) << (7 - (i & 7));
  for (size_t i = bits.size() / 8, pad = 0; i < data.size(); i++, pad++) data[i] = pad % 2 == 0 ? 0xEC : 0x11;

  // Split into blocks, each with its own error correction, then interleave.
  const int nblocks = kBlocks[ver], ecc_len = kEccPerBlock[ver];
  const int raw = raw_modules(ver) / 8;
  const int short_blocks = nblocks - raw % nblocks, short_len = raw / nblocks;
  std::vector<std::vector<uint8_t>> blocks;
  for (int i = 0, k = 0; i < nblocks; i++) {
    const int len = short_len - ecc_len + (i < short_blocks ? 0 : 1);
    std::vector<uint8_t> d(data.begin() + k, data.begin() + k + len);
    k += len;
    const std::vector<uint8_t> e = ecc_of(d, ecc_len);
    if (i < short_blocks) d.push_back(0);  // a hole, so every block is as long
    d.insert(d.end(), e.begin(), e.end());
    blocks.push_back(std::move(d));
  }
  std::vector<uint8_t> words;
  for (size_t i = 0; i < blocks[0].size(); i++)
    for (size_t j = 0; j < blocks.size(); j++)
      if (i != size_t(short_len - ecc_len) || int(j) >= short_blocks) words.push_back(blocks[j][i]);

  Grid base = patterns(ver);
  place(base, words);
  int best = mask, best_penalty = 1 << 30;
  if (mask < 0 || mask > 7)
    for (int m = 0; m < 8; m++) {
      Grid g = base;
      apply_mask(g, m);
      format_bits(g, m);
      if (const int p = penalty(g); p < best_penalty) {
        best_penalty = p;
        best = m;
      }
    }
  apply_mask(base, best);
  format_bits(base, best);

  Code out;
  out.size = base.n;
  out.dark.assign(base.m.size(), false);
  for (size_t i = 0; i < base.m.size(); i++) out.dark[i] = base.m[i] != 0;
  return out;
}

}  // namespace mico::qr
