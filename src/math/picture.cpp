#include "math/picture.h"

#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_set>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop

namespace mico::math {
namespace {

constexpr size_t kMaxBytes = 48u << 20;     // an encoded image larger than this is not shown
constexpr uint64_t kMaxPixels = 40'000'000;  // nor one that decodes larger

bool write_all(int fd, const void* p, size_t n) {
  const char* c = static_cast<const char*>(p);
  while (n) {
    const ssize_t k = write(fd, c, n);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return false;
    c += k;
    n -= size_t(k);
  }
  return true;
}

// Area-averaged resize, in premultiplied alpha so transparent pixels do not
// darken the edges they border.
std::vector<uint8_t> resize(const std::vector<uint8_t>& src, int sw, int sh, int tw, int th) {
  std::vector<uint8_t> out(size_t(tw) * size_t(th) * 4, 0);
  const double fx = double(sw) / tw, fy = double(sh) / th;
  for (int y = 0; y < th; y++) {
    const double y0 = y * fy, y1 = (y + 1) * fy;
    for (int x = 0; x < tw; x++) {
      const double x0 = x * fx, x1 = (x + 1) * fx;
      double r = 0, g = 0, b = 0, a = 0, area = 0;
      for (int sy = int(y0); sy < std::min(sh, int(std::ceil(y1))); sy++) {
        const double wy = std::min(y1, double(sy + 1)) - std::max(y0, double(sy));
        for (int sx = int(x0); sx < std::min(sw, int(std::ceil(x1))); sx++) {
          const double wgt = wy * (std::min(x1, double(sx + 1)) - std::max(x0, double(sx)));
          const uint8_t* p = &src[(size_t(sy) * size_t(sw) + size_t(sx)) * 4];
          const double pa = p[3] / 255.0 * wgt;
          r += p[0] * pa, g += p[1] * pa, b += p[2] * pa, a += pa, area += wgt;
        }
      }
      uint8_t* o = &out[(size_t(y) * size_t(tw) + size_t(x)) * 4];
      if (a > 0) {
        o[0] = uint8_t(std::clamp(r / a, 0.0, 255.0) + 0.5);
        o[1] = uint8_t(std::clamp(g / a, 0.0, 255.0) + 0.5);
        o[2] = uint8_t(std::clamp(b / a, 0.0, 255.0) + 0.5);
      }
      o[3] = area > 0 ? uint8_t(std::clamp(a / area * 255.0, 0.0, 255.0) + 0.5) : 0;
    }
  }
  return out;
}

}  // namespace

bool base64_decode(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size() / 4 * 3);
  uint32_t acc = 0;
  int bits = 0;
  for (const char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') break;
    else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    else if (c == '\\') continue;  // a JSON-escaped slash, "\/"
    else return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(char((acc >> bits) & 0xFF));
    }
  }
  return !out.empty();
}

bool image_size(std::string_view bytes, int* w, int* h) {
  if (bytes.empty() || bytes.size() > kMaxBytes) return false;
  int iw = 0, ih = 0, comp = 0;
  if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &iw, &ih, &comp))
    return false;
  if (iw <= 0 || ih <= 0 || uint64_t(iw) * uint64_t(ih) > kMaxPixels) return false;
  *w = iw;
  *h = ih;
  return true;
}

bool decode_image(std::string_view bytes, std::vector<uint8_t>& rgba, int* w, int* h) {
  if (bytes.empty() || bytes.size() > kMaxBytes) return false;
  // Cheap checks first: dimensions from the header, without decoding.
  int iw = 0, ih = 0, comp = 0;
  if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &iw, &ih, &comp))
    return false;
  if (iw <= 0 || ih <= 0 || uint64_t(iw) * uint64_t(ih) > kMaxPixels) return false;

  int fds[2];
  if (pipe(fds) != 0) return false;
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    // The child: decode, write the pixels back, and go. A bad image can only
    // kill this process.
    close(fds[0]);
    rlimit cpu{5, 5};
    setrlimit(RLIMIT_CPU, &cpu);
    int cw = 0, ch = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()),
                                        &cw, &ch, &n, 4);
    if (!px) _exit(1);
    const int32_t dims[2] = {cw, ch};
    const bool ok = write_all(fds[1], dims, sizeof dims) && write_all(fds[1], px, size_t(cw) * size_t(ch) * 4);
    _exit(ok ? 0 : 1);
  }
  close(fds[1]);
  std::string got;
  const size_t want = sizeof(int32_t) * 2 + size_t(iw) * size_t(ih) * 4;
  got.reserve(want);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  char buf[1 << 16];
  bool timed_out = false;
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) { timed_out = true; break; }
    pollfd p{fds[0], POLLIN, 0};
    const int r = poll(&p, 1, int(left));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { timed_out = true; break; }
    const ssize_t k = read(fds[0], buf, sizeof buf);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) break;
    got.append(buf, size_t(k));
    if (got.size() > want) break;  // more than it said: not to be trusted
  }
  close(fds[0]);
  if (timed_out) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  if (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || got.size() != want) return false;
  int32_t dims[2];
  memcpy(dims, got.data(), sizeof dims);
  if (dims[0] != iw || dims[1] != ih) return false;
  *w = iw;
  *h = ih;
  rgba.assign(got.begin() + sizeof dims, got.end());
  return true;
}

const Image* picture(const std::string& key, const std::function<bool(std::string&)>& load,
                     int max_cols, int max_rows, const std::string& copy) {
  const Config& cfg = config();
  if (!cfg.enabled || max_cols < 2 || max_rows < 1) return nullptr;
  const std::string full = "pic:" + std::to_string(max_cols) + "x" + std::to_string(max_rows) + ":" + key;
  if (const Image* im = cached(full)) return im;
  // What could not be read is not tried again on every relayout.
  static std::unordered_set<std::string> failed;
  if (failed.count(key)) return nullptr;
  std::string bytes;
  int w = 0, h = 0;
  if (!load(bytes) || !image_size(bytes, &w, &h)) {
    if (failed.size() > 4096) failed.clear();
    failed.insert(key);
    return nullptr;
  }
  const int cw = cfg.cell_w, ch = cfg.cell_h;
  const double scale = std::min({1.0, double(max_cols * cw) / w, double(max_rows * ch) / h});
  const int tw = std::max(1, int(std::lround(w * scale))), th = std::max(1, int(std::lround(h * scale)));

  // Laid out from its size alone: the pixels wait until a terminal needs
  // them, so opening a chat full of screenshots decodes none of them.
  Image im;
  im.cols = (tw + cw - 1) / cw;
  im.rows = (th + ch - 1) / ch;
  im.w = im.cols * cw;
  im.h = im.rows * ch;
  im.fit_w = tw;
  im.fit_h = th;
  im.encoded = std::make_shared<const std::string>(std::move(bytes));
  im.display = true;
  im.src = key;
  im.copy = copy;
  return store(full, std::move(im));
}

namespace {

// Decoded pictures, most recently used last, and what their pixels (and
// kitty payloads) hold in all. A chat full of screenshots holds a few of
// them decoded, not all.
constexpr size_t kPixelBudget = 64u << 20;
std::vector<uint32_t>& resident() {
  static std::vector<uint32_t> r;
  return r;
}

void fit(const Image& im, const std::vector<uint8_t>& src, int w, int h) {
  const int tw = im.fit_w, th = im.fit_h;
  std::vector<uint8_t> fitted = (tw == w && th == h) ? src : resize(src, w, h, tw, th);
  im.rgba.assign(size_t(im.w) * size_t(im.h) * 4, 0);
  for (int y = 0; y < th && y < im.h; y++)
    memcpy(&im.rgba[size_t(y) * size_t(im.w) * 4], &fitted[size_t(y) * size_t(tw) * 4],
           size_t(std::min(tw, im.w)) * 4);
}

}  // namespace

bool pixels(const Image& im) {
  if (!im.encoded) return true;
  auto& r = resident();
  if (const auto it = std::find(r.begin(), r.end(), im.id); it != r.end()) r.erase(it);
  if (im.rgba.empty()) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!decode_image(*im.encoded, rgba, &w, &h)) return false;
    fit(im, rgba, w, h);
  }
  r.push_back(im.id);
  // Over budget: the pictures used longest ago give their pixels back. Their
  // ids stay; the terminals that were sent them keep them.
  size_t total = 0;
  for (size_t i = 0; i < r.size();) {
    const Image* p = find(r[i]);
    if (!p || !p->encoded) {
      r.erase(r.begin() + ptrdiff_t(i));
      continue;
    }
    total += p->rgba.size() + p->wire.size();
    i++;
  }
  while (total > kPixelBudget && r.size() > 1) {
    const Image* p = find(r.front());
    if (p) {
      total -= std::min(total, p->rgba.size() + p->wire.size());
      std::vector<uint8_t>().swap(p->rgba);
      std::string().swap(p->wire);
    }
    r.erase(r.begin());
  }
  return true;
}

}  // namespace mico::math
