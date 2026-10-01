#pragma once
#include <string_view>

#include "views/chart.h"

// Mermaid diagrams an agent writes in a ```mermaid block, drawn with box-drawing
// characters: flowcharts (graph / flowchart, TD or LR), state diagrams,
// sequence diagrams, and pies (as a bar chart). Text cells, so they work in
// every terminal and copy as text. Not all of Mermaid: what agents draw to
// explain a design.
namespace mico::diagram {

// The diagram at most `cols` wide, as a figure of text rows. False when it is
// a kind this does not draw, does not parse, or cannot fit: the block then
// shows as code.
bool draw(std::string_view src, int cols, chart::Figure& out);

}  // namespace mico::diagram
