#pragma once
#include <string>
#include <string_view>
#include <vector>

// The desktop clipboard, for what a terminal cannot paste: an image. A terminal
// only ever pastes text, so an image copied to the clipboard reaches mico as
// nothing at all. mico reads the clipboard itself on Ctrl+V, the way claude and
// codex do, through the desktop's own tool: wl-paste on Wayland, xclip on X11.
// Neither is linked in; when neither is installed the read says so and the
// caller tells the user what to install.
namespace mico::clip {

struct Content {
  enum class Kind { Empty, Text, Image, NoTool };
  Kind kind = Kind::Empty;
  std::string text;        // Kind::Text
  std::string image_path;  // Kind::Image: the saved file
  std::string error;       // what went wrong, for the status line
};

// Reads the clipboard, preferring an image over text. An image is saved to a
// private directory under the runtime dir (RAM-backed, gone at logout), where
// the agent can open it. Bounded by `timeout_ms`: a clipboard owner that never
// answers must not freeze the UI.
Content read(int timeout_ms = 2000);

// The image files `pasted` names, when it names nothing else: one path or
// file:// URI per line, optionally quoted, the way file managers and
// terminals paste a copied file. Empty when any line is not an existing image.
std::vector<std::string> image_paths(std::string_view pasted);

}  // namespace mico::clip
