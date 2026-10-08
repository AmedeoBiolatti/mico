#pragma once
#include <string_view>
#include <vector>

// A QR code of some text: byte mode, error correction level M, the smallest
// version that holds it, the mask the standard's penalty rules like best.
// Enough to hand a phone an address; nothing else of the standard is here.
namespace mico::qr {

struct Code {
  int size = 0;                // modules a side, 0 when the text is too long
  std::vector<bool> dark;      // row by row, size * size
  bool at(int x, int y) const { return dark[size_t(y) * size_t(size) + size_t(x)]; }
};

// `mask` forces one of the eight masks (0-7); -1 chooses.
Code encode(std::string_view text, int mask = -1);

}  // namespace mico::qr
