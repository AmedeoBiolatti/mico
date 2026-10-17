#pragma once
#include "views/markdown.h"
#include "ui/theme.h"
#include "vt/surface.h"

namespace mico {
// Apply a Markdown segment's ink to its surrounding text style.
Style ink_style(Style base, md::Ink ink, const Theme& th);
}  // namespace mico
