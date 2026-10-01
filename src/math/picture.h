#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "math/math.h"

// Pictures from outside: image files a message points at, screenshots and
// images inside a transcript. Decoded, fitted to the chat, and shown through
// the same image cells as equations and charts.
namespace mico::math {

// Decodes PNG, JPEG, GIF (first frame) or BMP bytes to straight-alpha RGBA.
// The decoder runs in a throwaway child process: the bytes come from files
// and transcripts mico does not control, and a malformed image must not take
// down the daemon every agent runs under. False when the bytes are not an
// image it reads, are too large, or the child failed.
bool decode_image(std::string_view bytes, std::vector<uint8_t>& rgba, int* w, int* h);

// An image's size from its header, without decoding it (the header is read
// here, not in a child: it is a few bytes, checked). False as decode_image.
bool image_size(std::string_view bytes, int* w, int* h);

// Standard base64 (whitespace ignored). False on anything else.
bool base64_decode(std::string_view in, std::string& out);
// Appends `n` bytes as standard base64.
void base64_encode(const uint8_t* p, size_t n, std::string& out);

// A picture no wider than `max_cols` cells and no taller than `max_rows`,
// never enlarged, cached under `key` (which names the source and its
// version). `load` supplies the encoded bytes when it has to be drawn. Null
// when images are off, or the bytes cannot be read as an image.
const Image* picture(const std::string& key, const std::function<bool(std::string&)>& load,
                     int max_cols, int max_rows, const std::string& copy);

// Makes sure a picture's pixels are in `im.rgba`, decoding them if they were
// dropped, and marks it used; may drop the pixels of pictures used longest
// ago to keep within a budget. False when they cannot be had (the bytes no
// longer decode), and the picture is not drawn. True at once for anything
// that is not a picture.
bool pixels(const Image& im);

// Makes sure `im.wire` holds what kitty is sent for a picture, without
// holding up the frame. A PNG kitty can take as it is (`wire_png`): its
// bytes, at once. Anything else is decoded, fitted and compressed on a worker
// thread: false until that is done (collect_prepared() says when), and for a
// picture that cannot be decoded. True at once for anything not a picture.
bool kitty_wire(const Image& im);
// Hands what the workers finished to their pictures. True when any was: the
// screen is to be drawn again, and sends them.
bool collect_prepared();
// Whether workers are busy on pictures, so the loop wakes soon to collect.
bool preparing();
// A picture about to scroll into view: where terminals take kitty's
// protocol, its payload is made ready now (see kitty_wire()).
void prefetch(uint32_t id);

}  // namespace mico::math
