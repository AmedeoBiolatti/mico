#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

// Syntax colouring for fenced code in the chat. Not a parser: a lexer per
// language family that finds comments, strings, numbers, keywords, types and
// calls, which is what makes code readable at a glance. Anything it does not
// recognise is plain text.
namespace mico::code {

// What a run of code is, coloured at draw time.
enum class Tok : uint8_t { Text, Keyword, String, Comment, Number, Type, Func, Added, Removed, Hunk };

struct Run {
  uint32_t off, len;  // within the line
  Tok tok;
};

struct Lang;  // opaque: what a fence's language name resolves to

// The language of a fence's info string ("python", "rs", "c++"…), or null
// for one this does not colour.
const Lang* lang_of(std::string_view fence);
// A file's language, by its extension ("src/app.cpp" is C++).
const Lang* lang_of_path(std::string_view path);
// Its display name, for the block's label.
std::string_view name_of(const Lang* l);

// Carried from one line of a block to the next: an open block comment or
// multi-line string.
struct State {
  uint8_t open = 0;
};

// Splits one line into coloured runs (adjacent runs never share a token).
void highlight(std::string_view line, const Lang* lang, State& st, std::vector<Run>& out);

}  // namespace mico::code
