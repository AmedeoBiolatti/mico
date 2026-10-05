#pragma once
#include <string>
#include <string_view>

namespace mico {

// `p` made absolute against `cwd`, with "./" and doubled slashes taken out.
std::string resolve_path(std::string_view cwd, std::string_view p);

// $XDG_CONFIG_HOME/mico (or ~/.config/mico): what the user chose.
std::string config_dir();
// $XDG_STATE_HOME/mico (or ~/.local/state/mico): what mico keeps for itself,
// its log among it.
std::string state_dir();

// Writes `data` to `path` whole or not at all: to a file beside it, renamed
// over it, for the user alone. False when it could not be written.
bool write_file_atomic(const std::string& path, std::string_view data);

}  // namespace mico
