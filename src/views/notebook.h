#pragma once
#include <string>
#include <string_view>

// Jupyter notebooks an agent reads, shown as a notebook: markdown cells as
// prose, code cells as code in the kernel's language, outputs under them,
// plots as pictures where they came, DataFrames as tables when the notebook
// has their HTML. Both turn the notebook into markdown for the chat's own
// renderer.
namespace mico::notebook {

// True when a tool result's text is Claude's reading of a notebook, whose
// cells it sends as <cell id="…">…</cell id="…"> blocks.
bool is_claude_read(std::string_view text);

// From the raw transcript line holding that tool result: its blocks in
// order, text and images, so each plot lands under its own cell.
bool from_claude(std::string_view raw_line, std::string& md);

// From an .ipynb file's JSON (what `cat notebook.ipynb` prints). False when
// it is not a notebook.
bool from_ipynb(std::string_view json, std::string& md);

}  // namespace mico::notebook
