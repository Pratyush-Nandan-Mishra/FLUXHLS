#pragma once
#include "stage1/HLSContext.h"
#include <memory>
#include <string>
#include <vector>

// ── Access pattern detected by affine analysis ────────────────────────────────

enum class AccessPattern {
    Sequential,    // stride-1 in innermost loop var  →  safe to stream
    Strided,       // stride-N in innermost loop var  →  needs BRAM partition
    Reduction,     // innermost-loop-var-invariant write  →  accumulator store
    SlidingWindow, // index like in[i-t] (outer ± inner)  →  line buffer candidate
    Unknown
};

// ── A single array access with its classified pattern ─────────────────────────

struct AffineAccess {
    std::string   array;
    bool          isWrite;
    std::string   indexExpr;     // original text from Stage 1
    AccessPattern pattern;
    std::string   stride;        // "1", "N", "4", …  ("—" for non-strided)
};

// ── Data dependence edge ───────────────────────────────────────────────────────

enum class DepType { RAW, WAR, WAW };

struct DepEdge {
    std::string src, dst;
    DepType     type;
    bool        loopCarried;
    int         distance;    // iteration distance (1 = adjacent iterations)
};

// ── Stage 2 loop node ─────────────────────────────────────────────────────────

struct AffineLoop {
    std::string id;
    int         line;
    std::string var, lo, hi;
    std::vector<HLSPragma>   pragmas;

    std::vector<AffineAccess> accesses;
    std::vector<DepEdge>      deps;

    // II analysis fields (set only when PIPELINE pragma is present)
    int  requestedII  = -1;   // -1 means not a pipelined loop
    int  resMII       =  1;   // resource-constrained MII  (1 MAC/DSP assumed)
    int  recMII       =  0;   // recurrence-constrained MII
    bool IIFeasible   = true;
    std::string recMIIReason; // human-readable explanation

    std::vector<std::unique_ptr<AffineLoop>> children;
};

// ── Stage 2 function and top-level container ──────────────────────────────────

struct AffineFunction {
    std::string name;
    int         line;
    std::vector<HLSParam>  params;
    std::vector<std::unique_ptr<AffineLoop>> loops;
};

class AffineContext {
public:
    std::vector<AffineFunction> functions;
};
