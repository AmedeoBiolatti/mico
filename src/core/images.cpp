#include "core/images.h"

namespace mico {
namespace {

// Visits each image in order; stops when `fn` returns false.
template <class F>
void each_image(std::string_view s, F fn) {
  size_t i = 0;
  while (i < s.size()) {
    const size_t a = s.find("\"type\":\"image\"", i);
    const size_t b = s.find("\"image_url\":\"data:image/", i);
    if (a == std::string_view::npos && b == std::string_view::npos) return;
    if (b < a) {
      // A data URL: data:image/png;base64,…
      const size_t media_at = b + 18;
      const size_t semi = s.find(';', media_at), comma = s.find(',', media_at), end = s.find('"', media_at);
      if (semi == std::string_view::npos || comma == std::string_view::npos || end == std::string_view::npos ||
          comma > end) {
        i = b + 1;
        continue;
      }
      if (!fn(std::string(s.substr(media_at, semi - media_at)), s.substr(comma + 1, end - comma - 1))) return;
      i = end;
      continue;
    }
    // An image block: its data and media type are its own fields, or its
    // source's, before the next image starts.
    size_t stop = s.find("\"type\":\"image\"", a + 14);
    const size_t next_url = s.find("\"image_url\":\"data:image/", a + 14);
    stop = std::min(stop == std::string_view::npos ? s.size() : stop, next_url == std::string_view::npos ? s.size() : next_url);
    // The block may also have listed its fields before "type".
    const size_t back = s.rfind('{', a);
    const size_t from = back == std::string_view::npos ? a : back;
    const std::string_view block = s.substr(from, stop - from);
    const size_t d = block.find("\"data\":\"");
    if (d == std::string_view::npos) {
      i = a + 14;
      continue;
    }
    const size_t dend = block.find('"', d + 8);
    std::string media = "image/png";
    for (std::string_view key : {std::string_view("\"media_type\":\""), std::string_view("\"mimeType\":\"")}) {
      const size_t m = block.find(key);
      if (m != std::string_view::npos) {
        const size_t mend = block.find('"', m + key.size());
        if (mend != std::string_view::npos) media = std::string(block.substr(m + key.size(), mend - m - key.size()));
      }
    }
    if (dend == std::string_view::npos) return;
    if (!fn(media, block.substr(d + 8, dend - d - 8))) return;
    i = from + dend;
  }
}

}  // namespace

int count_images(std::string_view line) {
  if (line.find("image") == std::string_view::npos) return 0;
  int n = 0;
  each_image(line, [&](const std::string&, std::string_view) { n++; return n < 64; });
  return n;
}

bool transcript_image(std::string_view line, int n, std::string* media, std::string_view* base64) {
  int k = 0;
  bool found = false;
  each_image(line, [&](const std::string& m, std::string_view data) {
    if (k++ != n) return true;
    *media = m;
    *base64 = data;
    found = true;
    return false;
  });
  return found;
}

}  // namespace mico
