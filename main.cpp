#include "stage1/Frontend.h"          // parsePragmas() and buildHLSContext()
#include "stage1/Dumper.h"            // IDumper and PrettyDumper
#include "stage2/AffineAnalysis.h"    //** buildAffineContext() and dumpAffineContext()
#include <iostream>                    // std::cerr for error output

// Usage: fluxhls <source.cpp> [-- extra-clang-args...]
int main(int argc, const char **argv) {
    if (argc < 2) {                                                    // need at least one argument (the source file)
        std::cerr << "Usage: fluxhls <source.cpp> [-- -Ifoo ...]\n";  // print usage hint to stderr
        return 1;                                                       // exit with non-zero code on bad usage
    }

    const std::string filepath = argv[1];         // source file path is the first CLI argument

    std::vector<std::string> extraArgs;           // extra compiler flags forwarded to Clang
    for (int i = 2; i < argc; ++i) {             // walk remaining arguments
        std::string a = argv[i];                  // current argument string
        if (a != "--") extraArgs.push_back(a);    // skip the "--" separator, collect everything else
    }

    // ── Stage 1: pragma extraction + AST walk ─────────────────────────────────
    auto pragmas = parsePragmas(filepath);         //** text-scan the source file for all #pragma HLS lines
    HLSContext ctx;                                // IR container: will hold functions, loops, and array accesses
    buildHLSContext(filepath, pragmas, ctx, extraArgs); //** walk the Clang AST and fill ctx with the extracted IR

    PrettyDumper stage1Dump;                       // stdout pretty-printer for Stage 1
    ctx.dump(stage1Dump);                          // print every function, loop, and access to stdout

    std::cout << std::string(56, '=') << "\n\n";  // visual separator between Stage 1 and Stage 2 output

    // ── Stage 2: affine analysis + dependence graph ───────────────────────────
    AffineContext affCtx;                          // Stage 2 IR: enriched loops with access patterns and DDG
    buildAffineContext(ctx, affCtx);               //** classify accesses, compute RecMII, build dependence graph
    dumpAffineContext(affCtx);                     // print Stage 2 analysis results to stdout

    return 0;                                      // successful exit
}
