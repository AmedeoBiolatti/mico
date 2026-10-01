#pragma once
#include <string>
#include <string_view>

// The fallback for terminals that cannot show images (math/ draws equations
// for those that can): what a reader actually wants out of a chat, the
// symbols, without the layout. Greek
// letters, operators, arrows and the handful of structural commands agents
// actually write (\frac, \sqrt, ^, _) turn into the Unicode characters that
// already mean the same thing; anything this does not recognise is left as
// plain text rather than hidden or mangled.
namespace mico::latex {

// Converts the body of a math span (without the surrounding $ / \( \) / $$)
// into readable Unicode, appended to `out`.
void to_unicode(std::string_view in, std::string& out);

}  // namespace mico::latex
