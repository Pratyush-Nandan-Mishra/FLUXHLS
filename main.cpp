#include "stage1/Frontend.h"
#include "stage1/Dumper.h"
#include "stage2/AffineAnalysis.h"
#include <iostream>

// Usage: fluxhls <source.cpp> [-- extra-clang-args...]
int main(int argc, const char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: fluxhls <source.cpp> [-- -Ifoo ...]\n";
        return 1;
    }

    const std::string filepath = argv[1];

    std::vector<std::string> extraArgs;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a != "--") extraArgs.push_back(a);
    }

    // ── Stage 1: pragma extraction + AST walk ─────────────────────────────────
    auto pragmas = parsePragmas(filepath);
    HLSContext ctx;
    buildHLSContext(filepath, pragmas, ctx, extraArgs);

    PrettyDumper stage1Dump;
    ctx.dump(stage1Dump);

    std::cout << std::string(56, '=') << "\n\n";

    // ── Stage 2: affine analysis + dependence graph ───────────────────────────
    AffineContext affCtx;
    buildAffineContext(ctx, affCtx);
    dumpAffineContext(affCtx);

    return 0;
}
