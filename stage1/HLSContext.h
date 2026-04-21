#pragma once
#include "Pragma.h"
#include <memory>
#include <string>
#include <vector>

// Forward-declare the dumper interface so HLSContext::dump() can reference it
// without pulling in Dumper.h here.
struct IDumper;

// ─── IR nodes (no Clang dependency — Stage 2+ consumes these directly) ────────

struct MemAccess {
    std::string array;
    bool        isWrite;
    std::string index;   // source text of the subscript expression
};

struct HLSLoop {
    std::string id;      // "L0", "L0.L1", "L0.L1.L2"
    int         line;
    std::string var;     // induction variable name
    std::string lo, hi;  // bound expressions as source text
    std::vector<HLSPragma>                 pragmas;   // PIPELINE / UNROLL
    std::vector<MemAccess>                 accesses;
    std::vector<std::unique_ptr<HLSLoop>>  children;
};

struct HLSParam {
    std::string name, type;
    bool        isArray;
    std::vector<HLSPragma> pragmas;  // INTERFACE pragmas for this port
};

struct HLSFunction {
    std::string name;
    int         line;
    std::vector<HLSParam>                 params;
    std::vector<std::unique_ptr<HLSLoop>> loops;
};

// ─── Top-level container ──────────────────────────────────────────────────────

class HLSContext {
public:
    std::vector<HLSFunction> functions;
    void dump(IDumper &d) const;
};
