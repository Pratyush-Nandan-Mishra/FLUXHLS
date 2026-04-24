#pragma once
#include "stage1/HLSContext.h"     // HLSParam, HLSPragma — function params and pragma data
#include "stage2/AffineContext.h"  // AccessPattern — carried into binding decisions for display
#include "stage3/ScheduleContext.h"// OpKind — carried into OpBinding for unit-assignment logic
#include <memory>                  // std::unique_ptr for owning child loops
#include <string>                  // std::string for names, labels, reason text
#include <vector>                  // std::vector for binding lists and children

// ── Hardware resource kinds Stage 4 can bind arrays and ops to ───────────────
// Each kind maps to a specific Xilinx primitive (or family of primitives).

enum class ResourceKind {
    BRAM,           // RAMB36/18 block RAM — dual-port, 1-cycle read/write latency
    ShiftRegister,  // SRL16E shift register chain — used for sliding-window line buffers
    Register,       // FDRE flip-flop — scalar accumulator or small fully-partitioned array
    DSP48           // DSP48E2 block — executes FAdd, FMul, and MAC operations
};

// ── Binding decision for one array ───────────────────────────────────────────
// Produced by the 5-rule binding table in Binder.cpp.

struct ArrayBinding {
    std::string   array;       // array name (matches AffineAccess::array)
    ResourceKind  resource;    //** chosen hardware resource type
    std::string   configStr;   // human-readable config: "1 bank", "N banks cyclic", "depth=TAPS"
    int           readPorts;   // simultaneous read accesses needed per pipeline iteration
    int           writePorts;  // simultaneous write accesses needed per pipeline iteration
    AccessPattern pattern;     // Stage 2 access pattern that drove this decision
    std::string   reason;      // one-line rationale (e.g. "strided → N-bank cyclic BRAM")
};

// ── Binding decision for one scheduled operation ──────────────────────────────
// Pairs each ScheduledOp with the specific hardware unit it executes on.

struct OpBinding {
    std::string  opId;   // matches ScheduledOp::id (e.g. "load_A", "mac", "store_C")
    OpKind       kind;   // operation category — determines which unit class it maps to
    std::string  unit;   // assigned hardware unit (e.g. "DSP48_0", "BRAM_A.port0", "acc_reg")
    int          cycle;  // ASAP start cycle carried from Stage 3
    std::string  array;  // array touched (empty for Init, FAdd, FMul, MAC)
    std::string  desc;   // human-readable description ("A[i]", "acc += A * B", etc.)
};

// ── Stage 4 loop node ─────────────────────────────────────────────────────────
// Mirrors AffineLoop / ScheduledLoop structure; adds array bindings and op bindings.

struct BoundLoop {
    // Identity fields — copied from ScheduledLoop verbatim
    std::string            id, var, lo, hi;
    int                    line;
    std::vector<HLSPragma> pragmas;

    // II analysis — carried forward from Stage 2/3, not recomputed here
    int  requestedII  = -1;   // II from PIPELINE pragma (-1 = not pipelined)
    int  resMII       =  1;   // resource-constrained MII
    int  recMII       =  0;   // recurrence-constrained MII
    bool IIFeasible   = true; // whether requestedII >= max(resMII, recMII)
    int  pipelineDepth = 0;   // latency of one pipeline iteration (from Stage 3)

    //** Stage 4 additions — only populated for pipelined loops
    std::vector<ArrayBinding> arrayBindings; // one entry per distinct array in scope
    std::vector<OpBinding>    opBindings;    // one entry per ScheduledOp

    // Resource count summary — filled by countResources() after binding
    int numDSP48s    = 0; //** number of DSP48 blocks consumed (one per FAdd/FMul/MAC op)
    int numBRAMs     = 0; //** number of distinct BRAM arrays (each may have multiple banks)
    int numShiftRegs = 0; //** number of SRL16 shift-register chains
    int numRegisters = 0; //** accumulator registers + small-array register partitions

    std::vector<std::unique_ptr<BoundLoop>> children; // nested child loops (recursively bound)
};

// ── Per-function container ────────────────────────────────────────────────────

struct BoundFunction {
    std::string            name;    // function name
    int                    line;    // source line of the function declaration
    std::vector<HLSParam>  params;  // parameters with interface pragmas
    std::vector<std::unique_ptr<BoundLoop>> loops; // top-level bound loop trees
};

// ── Top-level Stage 4 IR ──────────────────────────────────────────────────────

class BindingContext {
public:
    std::vector<BoundFunction> functions; // one entry per HLS kernel function
};
