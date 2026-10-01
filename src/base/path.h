#pragma once
#include <string>
#include <string_view>

namespace mico {

// `p` made absolute against `cwd`, with "./" and doubled slashes taken out.
std::string resolve_path(std::string_view cwd, std::string_view p);

}  // namespace mico
