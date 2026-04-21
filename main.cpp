#include "stage1/Frontend.h"
#include "stage1/Dumper.h"
#include <iostream>

// Usage: fluxhls <source.cpp> [-- extra-clang-args...]
int main(int argc, const char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: fluxhls <source.cpp> [-- -Ifoo ...]\n";
        return 1;
    }

    const std::string filepath = argv[1];

    // Collect any extra compiler args passed after --
    std::vector<std::string> extraArgs;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a != "--") extraArgs.push_back(a);
    }

    // Stage 1a: extract #pragma HLS from source text
    auto pragmas = parsePragmas(filepath);

    // Stage 1b: walk AST and build HLS IR
    HLSContext ctx;
    buildHLSContext(filepath, pragmas, ctx, extraArgs);

    // Output
    PrettyDumper dumper;
    ctx.dump(dumper);

    return 0;
}
