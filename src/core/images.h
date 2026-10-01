#pragma once
#include <string>
#include <string_view>

// Images inside a transcript line, whichever agent wrote it: Claude's
// {"type":"image","source":{"media_type":…,"data":…}} blocks (screenshots, an
// image file the agent read, one the user pasted), codex's input_image data
// URLs, pi's {"type":"image","data":…,"mimeType":…}. Matched on the JSON's own
// quotes, which text inside a string cannot produce (its quotes are escaped).
namespace mico {

// How many images the line holds.
int count_images(std::string_view line);
// The n-th: its media type and base64 data (a view into `line`).
bool transcript_image(std::string_view line, int n, std::string* media, std::string_view* base64);

}  // namespace mico
