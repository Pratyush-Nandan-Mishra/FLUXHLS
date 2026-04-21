#pragma once
#include "HLSContext.h"
#include <string>
#include <vector>

// Step 1 — Parse #pragma HLS lines from the source file as plain text.
//           No Clang needed; pragmas are not in the AST anyway.
std::vector<HLSPragma> parsePragmas(const std::string &filepath);

// Step 2 — Walk the Clang AST via libclang C API to fill an HLSContext.
//           compilerArgs: extra flags forwarded to Clang (e.g. {"-I/some/path"}).
void buildHLSContext(const std::string              &filepath,
                     const std::vector<HLSPragma>   &pragmas,
                     HLSContext                     &ctx,
                     const std::vector<std::string> &compilerArgs = {});
