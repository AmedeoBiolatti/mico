#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/color.h"

// Equations (and charts) drawn as pictures, for terminals that can show one
// inside the cell grid (kitty's graphics protocol, or sixel). Each
// image is sized to a whole number of cells, and the chat lays it out as that
// many rows of placeholder cells — so scrolling, clipping and diffing treat it
// exactly like text.
namespace mico::math {

struct Config {
  bool enabled = false;  // every attached terminal can show images
  int cell_w = 10, cell_h = 20;
  Color fg = 0x56B6C2;
  bool kitty = false;  // every attached terminal takes kitty's protocol
  bool operator==(const Config&) const = default;
};

// Changes what images are drawn for. Anything laid out under the old config
// is stale: generation() moves, and every cached image is dropped.
void configure(const Config& c);
const Config& config();
uint64_t generation();

struct Image {
  uint32_t id = 0;
  int cols = 0, rows = 0;
  int base_row = 0;        // inline: the row the surrounding text's baseline is on
  int w = 0, h = 0;        // pixels: exactly cols x rows cells
  std::vector<uint8_t> alpha;  // an equation: coverage, drawn in the configured colour
  // A chart or a picture: its own colours (straight alpha); alpha unused. A
  // picture's may be empty: see `encoded`.
  mutable std::vector<uint8_t> rgba;
  // A picture from outside keeps its encoded bytes (a PNG or JPEG, a small
  // fraction of its pixels) and the size they are fitted to. Its pixels are
  // decoded only when a terminal needs them, and dropped again when other
  // pictures need the room more (see `pixels()` in picture.h): its id stays
  // good all along, so no layout has to be redone.
  std::shared_ptr<const std::string> encoded;
  int fit_w = 0, fit_h = 0;
  int src_w = 0, src_h = 0;    // the encoded picture's own size
  std::string src;             // what it was drawn from
  std::string copy;            // what selecting it copies
  bool display = false;
  // Its kitty transmission payload (compressed, base64), made once and sent
  // to every terminal that needs it.
  mutable std::string wire;
  // `wire` is the encoded PNG as it is, for the terminal to decode (f=100).
  mutable bool wire_png = false;
};

// A display equation (`display`) or inline math, drawn to fit `max_cols`.
// Null when images are off, when inline math reads fine as Unicode, or when
// it cannot be drawn narrow enough.
const Image* image(std::string_view src, bool display, int max_cols);
const Image* find(uint32_t id);

// A picture drawn elsewhere (a chart), cached under `key`. Storing one with
// the same `lineage` as an earlier picture — the next frame of a live chart —
// drops the earlier one.
const Image* cached(const std::string& key);
const Image* store(const std::string& key, Image im, const std::string& lineage = {});

// Ids dropped since the last call, so terminals can free them.
void take_evicted(std::vector<uint32_t>& out);

// Draws without the cache or the config: for --math and the tests.
Image draw(std::string_view src, bool display, int cell_w, int cell_h, float scale = 1.f);

// The PNG of an image in `fg`, for --math.
std::string png(const Image& im, Color fg);

}  // namespace mico::math
