#pragma once
#include <string>

#include "term/term.h"

namespace mico {

// Re-encodes a decoded event back into terminal bytes for a child pty. mico
// decodes input once and re-emits it, rather than passing raw bytes through, so
// the same keystroke can mean different things in chat view and raw view.
std::string encode_key(const KeyEvent& k, bool app_cursor);
std::string encode_mouse(const MouseEvent& m, bool sgr);
std::string encode_paste(const std::string& text, bool bracketed);

}  // namespace mico
