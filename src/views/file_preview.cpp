#include "views/file_preview.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/path.h"
#include "views/markdown_style.h"

namespace mico {

void FilePreview::open(const std::string& url) {
  close();
  path_ = file_url_path(url);
  if (path_.empty()) return;
  url_ = url;
  content_.clear(); note_.clear();
  width_ = scroll_ = line_ = 0;
  if (const size_t h = url.find("#L"); h != std::string::npos) line_ = std::atoi(url.c_str() + h + 2);
  env_.base_dir = path_.substr(0, path_.rfind('/'));
  markdown_ = path_.ends_with(".md") || path_.ends_with(".markdown") || path_.ends_with(".MD");
  // Nonblocking open plus fstat prevents a link to a pipe/device from hanging
  // the UI. Large files are sampled, never read wholesale on mouse movement.
  const int fd = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  struct stat st{};
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (fd >= 0) ::close(fd);
    note_ = "Cannot preview this file.";
    return;
  }
  constexpr size_t limit = 256 * 1024;
  content_.resize(size_t(std::min<int64_t>(std::max<int64_t>(0, st.st_size), limit)));
  size_t at = 0;
  while (at < content_.size()) {
    const ssize_t n = read(fd, content_.data() + at, content_.size() - at);
    if (n > 0) at += size_t(n);
    else if (n < 0 && errno == EINTR) continue;
    else break;
  }
  ::close(fd);
  content_.resize(at);
  for (unsigned char c : content_) {
    if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F) {
      content_.clear(); note_ = "No text preview available.";
      return;
    }
  }
  if (st.st_size > int64_t(limit)) note_ = "Preview limited to the first 256 KiB.";
  if (content_.empty()) note_ = "Empty file.";
}

void FilePreview::layout(int width) {
  if (width == width_) return;
  width_ = width;
  segs_.clear(); rows_.clear(); scratch_.clear();
  md::Out out{10000, &scratch_, &segs_, &rows_, &spans_, &inline_, &work_, &env_};
  if (markdown_) md::render(content_, 0, false, width, out);
  else md::render_output(content_, 0, false, width, out, code::lang_of_path(path_), true);
  if (line_ > 1) {
    scroll_ = std::min(line_ - 1, std::max(0, int(rows_.size()) - 1));
    size_t offset = 0;
    for (int n = 1; n < line_ && offset < content_.size(); n++) {
      const size_t end = content_.find('\n', offset);
      offset = end == std::string::npos ? content_.size() : end + 1;
    }
    for (size_t i = 0; i < rows_.size(); i++) {
      const auto& row = rows_[i];
      bool found = false;
      for (uint16_t k = 0; k < row.seg_count; k++) {
        const auto& seg = segs_[row.seg_first + k];
        found |= !(seg.off & md::kScratchBit) && seg.off + seg.len > offset;
      }
      if (found) { scroll_ = int(i); break; }
    }
    line_ = 0;
  }
}

void FilePreview::scroll(int delta) {
  scroll_ = std::clamp(scroll_ + delta, 0, std::max(0, int(rows_.size()) - visible_));
}

void FilePreview::render(Surface& surface, Rect pane, Rect anchor, const Theme& th) {
  const int below = pane.bottom() - anchor.bottom(), above = anchor.y - pane.y;
  const int height = std::min({22, std::max(above, below), std::max(6, pane.h * 2 / 3)});
  const int width = std::min(84, pane.w - 2);
  if (height < 5 || width < 16) { rect_ = {}; return; }
  rect_ = {std::clamp(anchor.x, pane.x + 1, pane.right() - width - 1),
           below >= above ? anchor.bottom() : anchor.y - height, width, height};
  layout(width - 4);
  visible_ = height - 4;
  scroll_ = std::clamp(scroll_, 0, std::max(0, int(rows_.size()) - visible_));
  Painter p(surface, rect_);
  p.clear(Style{th.text, th.panel});
  p.box({0, 0, width, height}, Style{th.accent, th.panel, attr::kDecor});
  const std::string name = path_.substr(path_.rfind('/') + 1);
  p.text_clipped(2, 0, " " + name + " ", Style{th.heading, th.panel, attr::kBold}, width - 4);
  p.text_clipped(2, height - 1, " Wheel: scroll · Esc: close ", Style{th.dim, th.panel}, width - 4);
  for (int y = 0; y < visible_ && scroll_ + y < int(rows_.size()); y++) {
    const auto& row = rows_[size_t(scroll_ + y)];
    int x = 2 + row.indent;
    const Color bg = row.flags & md::kCodeRow ? th.code_bg : th.panel;
    p.fill({2, y + 2, width - 4, 1}, Style{th.text, bg});
    for (uint16_t k = 0; k < row.seg_count; k++) {
      const auto& seg = segs_[row.seg_first + k];
      uint32_t image;
      int image_row, image_cols;
      if (md::image_ref(scratch_, seg, &image, &image_row, &image_cols)) {
        const Style style{Color(image), bg, attr::kImage};
        for (int col = 0; col < image_cols && x + col < width - 2; col++)
          p.put(x + col, y + 2, char32_t(uint32_t(image_row) << 16 | uint32_t(col)), style);
        x += image_cols;
        continue;
      }
      const std::string_view text = seg.off & md::kScratchBit
          ? scratch_.view({seg.off & ~md::kScratchBit, seg.len})
          : std::string_view(content_).substr(seg.off, seg.len);
      Style style = ink_style(Style{th.text, bg}, seg.ink, th);
      if (seg.attr & md::kAttrBold) style.a |= attr::kBold;
      if (seg.attr & md::kAttrItalic) style.a |= attr::kItalic;
      if (seg.attr & md::kAttrStrike) style.a |= attr::kStrike;
      // The popup is read-only: its links never replace the chat's hit map.
      x += p.text_clipped(x, y + 2, text, style, std::max(0, width - 2 - x));
    }
  }
  if (!note_.empty()) p.text_clipped(2, 1, note_, Style{th.dim, th.panel}, width - 4);
  else if (rows_.size() > size_t(visible_))
    p.text_clipped(2, 1, std::to_string(scroll_ + 1) + "–" +
        std::to_string(std::min(int(rows_.size()), scroll_ + visible_)) + " / " + std::to_string(rows_.size()),
        Style{th.dim, th.panel}, width - 4);
}

}  // namespace mico
