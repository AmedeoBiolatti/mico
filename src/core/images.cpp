#include "core/images.h"

#include <algorithm>
#include <charconv>

#include "base/text.h"

namespace mico {
namespace {

constexpr std::string_view kBlock = "\"type\":\"image\"";
constexpr std::string_view kUrl = "\"image_url\":\"data:image/";
constexpr size_t npos = std::string_view::npos;

// The value of the string field `key` in `s`, if it has one.
std::string_view field(std::string_view s, std::string_view key) {
  const size_t k = text::find(s, key);
  if (k == npos) return {};
  const size_t end = s.find('"', k + key.size());
  return end == npos ? std::string_view() : s.substr(k + key.size(), end - k - key.size());
}

// Visits each image in order; stops when `fn` returns false. Each pattern's
// next match is looked for again only once it is passed, and the base64 is
// crossed once: a line of screenshots is read once, not once per image.
template <class F>
void each_image(std::string_view s, F fn) {
  size_t a = text::find(s, kBlock), b = text::find(s, kUrl);
  const auto past = [&](size_t i) {
    if (a < i) a = text::find(s, kBlock, i);
    if (b < i) b = text::find(s, kUrl, i);
  };
  while (a != npos || b != npos) {
    if (b < a) {
      // A data URL: data:image/png;base64,…
      const size_t media_at = b + kUrl.size() - 6;
      const size_t end = s.find('"', media_at);
      const std::string_view url = s.substr(media_at, end == npos ? 0 : end - media_at);
      const size_t semi = url.find(';'), comma = url.find(',');
      if (semi == npos || comma == npos) {
        past(b + 1);
        continue;
      }
      if (!fn(std::string(url.substr(0, semi)), url.substr(comma + 1))) return;
      past(end);
      continue;
    }
    // An image block: its data and media type are its own fields, or its
    // source's, before the next image starts. It may also have listed its
    // fields before "type".
    const size_t back = s.rfind('{', a);
    const size_t from = back == npos ? a : back;
    past(a + kBlock.size());
    const std::string_view block = s.substr(from, std::min(a, b) - from);
    const size_t d = text::find(block, "\"data\":\"");
    if (d == npos) continue;
    const size_t dend = block.find('"', d + 8);
    if (dend == npos) return;
    // The media type is looked for on either side of the data, not in it.
    std::string media = "image/png";
    for (std::string_view key : {std::string_view("\"media_type\":\""), std::string_view("\"mimeType\":\"")}) {
      std::string_view m = field(block.substr(0, d), key);
      if (m.empty()) m = field(block.substr(dend), key);
      if (!m.empty()) media = std::string(m);
    }
    if (!fn(media, block.substr(d + 8, dend - d - 8))) return;
  }
}

}  // namespace

std::vector<LineImage> line_images(std::string_view line) {
  std::vector<LineImage> out;
  if (!text::contains(line, "\"image")) return out;  // both patterns hold it
  each_image(line, [&](const std::string& media, std::string_view data) {
    out.push_back(LineImage{media, size_t(data.data() - line.data()), data.size()});
    return out.size() < 64;
  });
  return out;
}

std::string to_string(const LineImage& im) {
  return im.media + ' ' + std::to_string(im.at) + ' ' + std::to_string(im.len);
}

bool from_string(std::string_view s, LineImage* im) {
  // From the right: the numbers have no spaces, the media type might.
  const size_t b = s.rfind(' ');
  const size_t a = b == npos || b == 0 ? npos : s.rfind(' ', b - 1);
  if (a == npos) return false;
  const auto number = [](std::string_view v, size_t* n) {
    return std::from_chars(v.data(), v.data() + v.size(), *n).ec == std::errc();
  };
  im->media = std::string(s.substr(0, a));
  return number(s.substr(a + 1, b - a - 1), &im->at) && number(s.substr(b + 1), &im->len);
}

int count_images(std::string_view line) { return int(line_images(line).size()); }

bool transcript_image(std::string_view line, int n, std::string* media, std::string_view* base64) {
  std::vector<LineImage> all = line_images(line);
  if (n < 0 || size_t(n) >= all.size()) return false;
  *media = std::move(all[size_t(n)].media);
  *base64 = line.substr(all[size_t(n)].at, all[size_t(n)].len);
  return true;
}

}  // namespace mico
