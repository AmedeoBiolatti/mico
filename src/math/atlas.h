#pragma once
#include <cstdint>

// The glyphs equations are composed from, pre-rendered from Latin Modern Math
// by tools/gen_math_atlas.py and linked into the binary. Nothing here touches
// a font file at run time.
namespace mico::math {

struct Glyph {
  char32_t cp;
  float adv;             // advance, em
  float x0, y0, x1, y1;  // ink box, em, y up from the baseline
  int bw, bh;            // coverage bitmap, master pixels
  int left, top;         // its first column right of the pen, first row below the baseline
  uint32_t index;        // position in the atlas, for the summed-area cache
};

// Null when the atlas has no such glyph.
const Glyph* glyph(char32_t cp);
// Master pixels per em the bitmaps were drawn at.
int master_px();
// Coverage summed over the rectangle [0,x) x [0,y), as a (bw+1) x (bh+1) table
// of 0..127 values. Built on first use: a box filter over it is exact at any
// scale and any subpixel offset, which is all resampling needs.
const uint32_t* sat(const Glyph& g);

}  // namespace mico::math
