#pragma once                          // guard against multiple inclusion
#include "stage1/HLSContext.h"        // HLSParam, HLSPragma — needed for function params and loop pragmas
#include <memory>                     // std::unique_ptr for owning child loops
#include <string>                     // std::string for ids, labels, expressions
#include <vector>                     // std::vector for ops, children, params

// ── Operation kinds Stage 3 can infer and schedule ───────────────────────────
// Derived from Stage 2 access patterns; used to assign latencies and labels.

enum class OpKind {
    Init,   // initialise the scalar accumulator register to 0.0 before an inner loop
    Load,   // read one element from an array (BRAM or DDR) into a register
    Store,  // write one element from a register back to an array
    FAdd,   // floating-point addition: computes A + B (no accumulation)
    FMul,   // floating-point multiply: computes A * B
    MAC     // multiply-accumulate: computes acc += A * B (loop-carried dep on acc)
};

// ── One scheduled operation ───────────────────────────────────────────────────

struct ScheduledOp {
    std::string      id;          // human-readable label e.g. "load_A", "mac", "store_C"
    OpKind           kind;        // operation category — determines latency and hardware unit
    std::string      array;       // array this op reads/writes (empty for Init, FAdd, MAC)
    std::string      indexExpr;   // index expression string from Stage 1 (e.g. "i * N + k")
    int              latency = 1; // cycles: time from start to when the result is available
    int              asap    = 0; //** ASAP result: earliest cycle this op can start
    int              alap    = 0; //** ALAP result: latest cycle this op can start without delaying output
    int              slack   = 0; //** slack = ALAP - ASAP: scheduling freedom (0 = on critical path)
    std::vector<int> depIds;      // indices into ScheduledLoop::ops this op must wait for
};

// ── Scheduled version of one loop node ───────────────────────────────────────

struct ScheduledLoop {
    // Identity fields — copied verbatim from AffineLoop
    std::string            id;      // loop identifier e.g. "L0", "L0.L1"
    int                    line;    // source line of the for-statement
    std::string            var;     // induction variable name
    std::string            lo, hi;  // loop trip bounds
    std::vector<HLSPragma> pragmas; // attached pragmas (PIPELINE, UNROLL, etc.)

    // II analysis — carried forward from Stage 2, not recomputed
    int  requestedII = -1;  // II from PIPELINE pragma (-1 if not pipelined)
    int  resMII      =  1;  // resource-constrained MII (always 1 in our model)
    int  recMII      =  0;  // recurrence-constrained MII from Stage 2
    bool IIFeasible  = true; // whether requestedII >= max(resMII, recMII)

    // Stage 3 additions — produced by the ASAP/ALAP passes
    int                      pipelineDepth = 0; //** cycle at which the last op finishes (= latency of one iteration)
    std::vector<ScheduledOp> ops;               //** flat op list in topological order, annotated with ASAP/ALAP/slack

    std::vector<std::unique_ptr<ScheduledLoop>> children; // nested child loops (recursively scheduled)
};

// ── Per-function and top-level context ───────────────────────────────────────

struct ScheduledFunction {
    std::string            name;    // function name
    int                    line;    // source line of the function declaration
    std::vector<HLSParam>  params;  // function parameters with interface pragmas
    std::vector<std::unique_ptr<ScheduledLoop>> loops; // top-level loop trees
};

class ScheduleContext {
public:
    std::vector<ScheduledFunction> functions; // one entry per HLS kernel function
};
