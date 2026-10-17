#pragma once
#include "views/markdown.h"
#include "ui/theme.h"
#include "vt/surface.h"

namespace mico {

// A read-only, bounded local-file preview. Owns its text separately from the
// chat so folding, streaming and relayout cannot invalidate the popup.
class FilePreview {
 public:
  void open(const std::string& url);
  void close() { url_.clear(); rect_ = {}; }
  bool active() const { return !url_.empty(); }
  Rect rect() const { return rect_; }
  void scroll(int delta);
  void render(Surface& surface, Rect pane, Rect anchor, const Theme& th);

 private:
  void layout(int width);
  std::string url_, path_, content_, note_;
  bool markdown_ = false;
  int width_ = 0, scroll_ = 0, visible_ = 0, line_ = 0;
  Rect rect_{};
  Arena scratch_;
  md::ChartEnv env_;
  md::Work work_;
  std::vector<md::Seg> segs_, inline_;
  std::vector<md::Line> rows_;
  std::vector<text::Span> spans_;
};

}  // namespace mico
