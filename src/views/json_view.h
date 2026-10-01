#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// A tool result that is JSON, laid out to be read: indented, short arrays of
// plain values kept on one line. Folded (the result collapsed), it is a
// one-line outline of the top level — what each member holds counted as
// "{…12 keys}" or "[…40 items]", long strings shortened — and expanding the
// result shows it whole.
namespace mico::json_view {

// Expanded with folding: each line starts with a mark — ▾ on an open
// container's first line, ▸ on a closed one's — and the containers' ids by
// line come back, so a click can open or close one. A big document opens
// with what lies below its top two levels closed.
struct Folding {
  const std::unordered_set<int>* flipped = nullptr;  // containers flipped from that default
  std::vector<uint16_t> line_nodes;                  // out: per line, the id + 1 of the container it opens, or 0
};

// False when `text` is not one JSON object or array (a scalar is not worth
// laying out), or nests too deep.
bool pretty(std::string_view text, bool folded, std::string& out, Folding* folding = nullptr);

}  // namespace mico::json_view
