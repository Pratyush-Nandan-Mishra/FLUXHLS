#pragma once
#include <string>   // std::string for file path and SV text
#include <vector>   // std::vector for module list

// ── One generated RTL module ──────────────────────────────────────────────────
// Holds the complete SystemVerilog text for one HLS kernel function.
// String-based (no SV AST): the emitter concatenates sections directly into svText.

struct RTLModule {
    std::string funcName;   // kernel function name (e.g. "vadd")
    std::string fileName;   // output file path    (e.g. "output/vadd.sv")
    std::string svText;     // complete .sv file content ready to write to disk
    int         lineCount;  // number of lines in svText (computed after build)
};

// ── Top-level Stage 6 IR ──────────────────────────────────────────────────────

class RTLContext {
public:
    std::vector<RTLModule> modules; // one entry per HLS kernel function
};
