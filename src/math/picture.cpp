#include "math/picture.h"

#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "math/deflate.h"

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
// darken the edges they border. Separable: each source row is averaged across
// once, and added into the one or two output rows it overlaps.
std::vector<uint8_t> resize(const uint8_t* src, int sw, int sh, int tw, int th) {
  // Each output column's source columns and their weights (summing to one).
  struct Tap {
    int from, n;  // source columns from..from+n-1, their weights from wts[at]
    size_t at;
  };
  std::vector<Tap> taps(static_cast<size_t>(tw));
  std::vector<float> wts;
  const double fx = double(sw) / tw, fy = double(sh) / th;
  for (int x = 0; x < tw; x++) {
    const double x0 = x * fx, x1 = (x + 1) * fx;
    const int a = int(x0), b = std::min(sw, int(std::ceil(x1)));
    taps[size_t(x)] = Tap{a, b - a, wts.size()};
    for (int sx = a; sx < b; sx++)
      wts.push_back(float((std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) / fx));
  }

  std::vector<float> across(size_t(tw) * 4), acc(size_t(tw) * 4, 0.f);
  std::vector<uint8_t> out(size_t(tw) * size_t(th) * 4, 0);
  const auto emit = [&](int y) {
    uint8_t* o = &out[size_t(y) * size_t(tw) * 4];
    for (int x = 0; x < tw; x++) {
      const float* q = &acc[size_t(x) * 4];
      const float a = q[3];
      if (a > 0) {
        o[x * 4] = uint8_t(std::clamp(q[0] / a, 0.f, 255.f) + 0.5f);
        o[x * 4 + 1] = uint8_t(std::clamp(q[1] / a, 0.f, 255.f) + 0.5f);
        o[x * 4 + 2] = uint8_t(std::clamp(q[2] / a, 0.f, 255.f) + 0.5f);
      }
      o[x * 4 + 3] = uint8_t(std::clamp(a * 255.f, 0.f, 255.f) + 0.5f);
    }
    std::fill(acc.begin(), acc.end(), 0.f);
  };
  int y = 0;
  for (int sy = 0; sy < sh && y < th; sy++) {
    const uint8_t* row = &src[size_t(sy) * size_t(sw) * 4];
    for (int x = 0; x < tw; x++) {
      const Tap& t = taps[size_t(x)];
      float r = 0, g = 0, b = 0, al = 0;
      for (int k = 0; k < t.n; k++) {
        const uint8_t* px = row + size_t(t.from + k) * 4;
        const float pa = px[3] * (1.f / 255.f) * wts[t.at + size_t(k)];
        r += px[0] * pa, g += px[1] * pa, b += px[2] * pa, al += pa;
      }
      float* q = &across[size_t(x) * 4];
      q[0] = r, q[1] = g, q[2] = b, q[3] = al;
    }
    // This source row's share of each output row it overlaps.
    for (;;) {
      const double y0 = y * fy, y1 = (y + 1) * fy;
      const float wy = float((std::min(y1, double(sy + 1)) - std::max(y0, double(sy))) / fy);
      if (wy > 0)
        for (size_t i = 0; i < acc.size(); i++) acc[i] += across[i] * wy;
      if (y1 > double(sy + 1) + 1e-9) break;  // the output row goes on below
      emit(y++);
      if (y >= th || y * fy >= double(sy + 1)) break;
    }
  }
  while (y < th) emit(y++);
  return out;
}

}  // namespace

bool base64_decode(std::string_view in, std::string& out) {
  // 0..63 a digit; 64 skipped (whitespace, the backslash of a JSON "\/");
  // 65 the end ('='); 66 anything else. URL-safe digits read too.
  static const auto table = [] {
    std::array<uint8_t, 256> t;
    t.fill(66);
    for (int i = 0; i < 26; i++) t[size_t('A' + i)] = uint8_t(i), t[size_t('a' + i)] = uint8_t(26 + i);
    for (int i = 0; i < 10; i++) t[size_t('0' + i)] = uint8_t(52 + i);
    t['+'] = t['-'] = 62;
    t['/'] = t['_'] = 63;
    t[' '] = t['\n'] = t['\r'] = t['\t'] = t['\\'] = 64;
    t['='] = 65;
    return t;
  }();
  out.clear();
  out.resize(in.size() / 4 * 3 + 3);
  char* o = out.data();
  const auto* p = reinterpret_cast<const uint8_t*>(in.data());
  const auto* end = p + in.size();
  // Four plain digits at a time, the whole of an unwrapped payload.
  while (end - p >= 4) {
    const uint32_t a = table[p[0]], b = table[p[1]], c = table[p[2]], d = table[p[3]];
    if ((a | b | c | d) >= 64) break;
    const uint32_t v = a << 18 | b << 12 | c << 6 | d;
    o[0] = char(v >> 16), o[1] = char(v >> 8), o[2] = char(v);
    o += 3, p += 4;
  }
  uint32_t acc = 0;
  int bits = 0;
  for (; p < end; p++) {
    const uint8_t v = table[*p];
    if (v == 64) continue;
    if (v == 65) break;
    if (v == 66) {
      out.clear();
      return false;
    }
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      *o++ = char((acc >> bits) & 0xFF);
    }
  }
  out.resize(size_t(o - out.data()));
  return !out.empty();
}

void base64_encode(const uint8_t* p, size_t n, std::string& out) {
  static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t start = out.size();
  out.resize(start + (n + 2) / 3 * 4);
  char* o = out.data() + start;
  size_t i = 0;
  for (; i + 3 <= n; i += 3, o += 4) {
    const uint32_t v = uint32_t(p[i]) << 16 | uint32_t(p[i + 1]) << 8 | p[i + 2];
    o[0] = kB64[v >> 18], o[1] = kB64[(v >> 12) & 63], o[2] = kB64[(v >> 6) & 63], o[3] = kB64[v & 63];
  }
  if (i < n) {
    const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0);
    o[0] = kB64[v >> 18], o[1] = kB64[(v >> 12) & 63];
    o[2] = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
    o[3] = '=';
  }
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

namespace {

// Decoded pixels in memory the decoding child wrote straight into: no pipe
// to carry them back, nor copies on either side of it.
struct Decoded {
  uint8_t* px = nullptr;
  size_t n = 0;
  int w = 0, h = 0;
  Decoded() = default;
  Decoded(const Decoded&) = delete;
  Decoded& operator=(const Decoded&) = delete;
  ~Decoded() {
    if (px) munmap(px, n);
  }
};

bool decode_shared(std::string_view bytes, Decoded& out) {
  if (bytes.empty() || bytes.size() > kMaxBytes) return false;
  // Cheap checks first: dimensions from the header, without decoding.
  int iw = 0, ih = 0, comp = 0;
  if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &iw, &ih, &comp))
    return false;
  if (iw <= 0 || ih <= 0 || uint64_t(iw) * uint64_t(ih) > kMaxPixels) return false;
  const size_t n = size_t(iw) * size_t(ih) * 4;
  void* mem = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return false;
  out.px = static_cast<uint8_t*>(mem);
  out.n = n;

  int fds[2];
  if (pipe(fds) != 0) return false;
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    // The child: decode into the shared pixels, say how big, and go. A bad
    // image can only kill this process.
    close(fds[0]);
    rlimit cpu{5, 5};
    setrlimit(RLIMIT_CPU, &cpu);
    int cw = 0, ch = 0, k = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()),
                                        &cw, &ch, &k, 4);
    if (!px || cw != iw || ch != ih) _exit(1);
    memcpy(mem, px, n);
    const int32_t dims[2] = {cw, ch};
    _exit(write_all(fds[1], dims, sizeof dims) ? 0 : 1);
  }
  close(fds[1]);
  int32_t dims[2] = {0, 0};
  size_t got = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool timed_out = false;
  while (got < sizeof dims) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) { timed_out = true; break; }
    pollfd p{fds[0], POLLIN, 0};
    const int r = poll(&p, 1, int(left));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { timed_out = true; break; }
    const ssize_t k = read(fds[0], reinterpret_cast<char*>(dims) + got, sizeof dims - got);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) break;
    got += size_t(k);
  }
  close(fds[0]);
  if (timed_out) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  if (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || got != sizeof dims) return false;
  if (dims[0] != iw || dims[1] != ih) return false;
  out.w = iw;
  out.h = ih;
  return true;
}

}  // namespace

bool decode_image(std::string_view bytes, std::vector<uint8_t>& rgba, int* w, int* h) {
  Decoded d;
  if (!decode_shared(bytes, d)) return false;
  rgba.assign(d.px, d.px + d.n);
  *w = d.w;
  *h = d.h;
  return true;
}

const Image* picture(const std::string& key, const std::function<bool(std::string&)>& load,
                     int max_cols, int max_rows, const std::string& copy, bool enlarge) {
  const Config& cfg = config();
  if (!cfg.enabled || max_cols < 2 || max_rows < 1) return nullptr;
  const std::string full = std::string(enlarge ? "picz:" : "pic:") + std::to_string(max_cols) + "x" +
                           std::to_string(max_rows) + ":" + key;
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
  const double scale = std::min({enlarge ? 8.0 : 1.0, double(max_cols * cw) / w, double(max_rows * ch) / h});
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
  im.src_w = w;
  im.src_h = h;
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

// The decoded pixels fitted to the picture: scaled to its fitted size, in
// the top left corner of its whole cells.
std::vector<uint8_t> fit(const Image& im, const Decoded& d) {
  const int tw = im.fit_w, th = im.fit_h;
  if (tw == im.w && th == im.h && d.w == tw && d.h == th) return std::vector<uint8_t>(d.px, d.px + d.n);
  const std::vector<uint8_t> scaled = (tw == d.w && th == d.h) ? std::vector<uint8_t>() : resize(d.px, d.w, d.h, tw, th);
  const uint8_t* fitted = scaled.empty() ? d.px : scaled.data();
  std::vector<uint8_t> out(size_t(im.w) * size_t(im.h) * 4, 0);
  for (int y = 0; y < th && y < im.h; y++)
    memcpy(&out[size_t(y) * size_t(im.w) * 4], fitted + size_t(y) * size_t(tw) * 4, size_t(std::min(tw, im.w)) * 4);
  return out;
}

// Marks `im` used last, and has the pictures used longest ago give back
// their pixels and payloads while the whole is over budget. Their ids stay;
// the terminals that were sent them keep them.
void remember(const Image& im) {
  auto& r = resident();
  if (const auto it = std::find(r.begin(), r.end(), im.id); it != r.end()) r.erase(it);
  r.push_back(im.id);
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
      p->wire_png = false;
    }
    r.erase(r.begin());
  }
}

// Pictures prepared for kitty off the UI thread: decoded (in a child, as
// decode_image does), fitted, compressed and put in base64 by a few workers.
// Last asked, first done: while scrolling, what is on screen now.
struct Job {
  uint32_t id = 0;
  std::shared_ptr<const std::string> encoded;
  std::vector<uint8_t> rgba;  // given when the pixels are at hand: no decode
  int w = 0, h = 0, fit_w = 0, fit_h = 0;
  std::string wire;
  bool ok = false;
};

struct Workers {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Job> todo;
  std::vector<Job> done;
  size_t busy = 0;
  // The UI thread's own: ids queued or under way, and ids that failed.
  std::unordered_set<uint32_t> asked, failed;

  void run() {
    for (;;) {
      Job j;
      {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return !todo.empty(); });
        j = std::move(todo.back());
        todo.pop_back();
        busy++;
      }
      if (j.rgba.empty()) {
        Decoded d;
        if (decode_shared(*j.encoded, d)) {
          Image shape;
          shape.w = j.w, shape.h = j.h, shape.fit_w = j.fit_w, shape.fit_h = j.fit_h;
          j.rgba = fit(shape, d);
        }
      }
      if (!j.rgba.empty()) {
        const std::string z = zlib_compress(j.rgba.data(), j.rgba.size());
        base64_encode(reinterpret_cast<const uint8_t*>(z.data()), z.size(), j.wire);
        j.ok = true;
      }
      std::lock_guard<std::mutex> lock(mu);
      busy--;
      done.push_back(std::move(j));
    }
  }
};

Workers& workers() {
  // Never torn down: the threads sleep until the process ends.
  static Workers* w = [] {
    auto* p = new Workers;
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 3u);
    for (unsigned i = 0; i < n; i++) std::thread([p] { p->run(); }).detach();
    return p;
  }();
  return *w;
}

// Pictures waiting longer than this many newer ones have scrolled past: not
// worth decoding any more. Asked for again, they are queued again.
constexpr size_t kMaxQueued = 12;

// A PNG small enough to send as it is: kitty decodes it, and nothing here
// has to.
bool kitty_takes(const Image& im) {
  const std::string& e = *im.encoded;
  return e.size() <= (8u << 20) && e.size() > 8 && e.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 &&
         uint64_t(im.src_w) * uint64_t(im.src_h) <= 24'000'000;
}

}  // namespace

bool pixels(const Image& im) {
  if (!im.encoded) return true;
  if (im.rgba.empty()) {
    Decoded d;
    if (!decode_shared(*im.encoded, d)) return false;
    im.rgba = fit(im, d);
  }
  remember(im);
  return true;
}

bool kitty_wire(const Image& im) {
  if (!im.encoded) return true;
  if (!im.wire.empty()) {
    remember(im);
    return true;
  }
  if (kitty_takes(im)) {
    base64_encode(reinterpret_cast<const uint8_t*>(im.encoded->data()), im.encoded->size(), im.wire);
    im.wire_png = true;
    remember(im);
    return true;
  }
  Workers& wk = workers();
  if (wk.failed.count(im.id) || wk.asked.count(im.id)) return false;
  Job j;
  j.id = im.id;
  j.encoded = im.encoded;
  j.rgba = im.rgba;
  j.w = im.w, j.h = im.h, j.fit_w = im.fit_w, j.fit_h = im.fit_h;
  {
    std::lock_guard<std::mutex> lock(wk.mu);
    while (wk.todo.size() >= kMaxQueued) {
      wk.asked.erase(wk.todo.front().id);
      wk.todo.pop_front();
    }
    wk.todo.push_back(std::move(j));
  }
  wk.asked.insert(im.id);
  wk.cv.notify_one();
  return false;
}

bool collect_prepared() {
  Workers& wk = workers();
  std::vector<Job> got;
  {
    std::lock_guard<std::mutex> lock(wk.mu);
    if (wk.done.empty()) return false;
    got.swap(wk.done);
  }
  bool any = false;
  for (Job& j : got) {
    wk.asked.erase(j.id);
    const Image* im = find(j.id);
    if (!im) continue;
    if (!j.ok) {
      wk.failed.insert(j.id);
      continue;
    }
    if (im->wire.empty()) {
      im->wire = std::move(j.wire);
      im->wire_png = false;
    }
    if (im->rgba.empty()) im->rgba = std::move(j.rgba);
    remember(*im);
    any = true;
  }
  return any;
}

void prefetch(uint32_t id) {
  if (!config().kitty) return;
  if (const Image* im = find(id); im && im->encoded) kitty_wire(*im);
}

bool preparing() {
  Workers& wk = workers();
  if (wk.asked.empty()) return false;
  std::lock_guard<std::mutex> lock(wk.mu);
  return !wk.todo.empty() || wk.busy > 0 || !wk.done.empty();
}

bool zlib_inflate(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
  out.clear();
  int len = 0;
  char* p = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(data), int(n), &len);
  if (!p) return false;
  out.assign(p, p + len);
  STBI_FREE(p);
  return true;
}

}  // namespace mico::math
