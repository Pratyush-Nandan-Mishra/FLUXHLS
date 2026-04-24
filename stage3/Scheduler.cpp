#include "Scheduler.h"      // public API: buildSchedule(), dumpSchedule()
#include <algorithm>         // std::max, std::min, std::find
#include <iomanip>           // std::setw, std::left, std::right for column alignment
#include <iostream>          // std::cout for the pretty-printer
#include <string>            // std::string, std::to_string

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 1 — Latency table and operation inference
// ═══════════════════════════════════════════════════════════════════════════════

// Return the hardware latency (in cycles) for a given operation kind.
// All set to 1 here (simplified educational model).
// To use realistic Xilinx DSP48 float latencies, change these values:
//   FAdd → 4 cycles,  FMul → 3 cycles,  MAC → 6 cycles
static int latencyOf(OpKind k) {              // one central table — easy to change for realism
    switch (k) {
    case OpKind::Init:  return 1;             // register write: 1 cycle
    case OpKind::Load:  return 1;             // BRAM read: 1 cycle (DDR would be higher)
    case OpKind::Store: return 1;             // BRAM write: 1 cycle
    case OpKind::FAdd:  return 1;             // float add: simplified to 1 (realistic: 4)
    case OpKind::FMul:  return 1;             // float multiply: simplified to 1 (realistic: 3)
    case OpKind::MAC:   return 1;             // multiply-accumulate: simplified to 1 (realistic: 6)
    }
    return 1;                                 // fallback — should not reach here
}

// Collect every AffineAccess reachable from a loop subtree (self + descendants).
// Used by inferOps to see all reads and writes in the full pipeline scope.
static void gatherAccesses(const AffineLoop &loop,                    // root of the subtree
                            std::vector<const AffineAccess *> &out) { // output flat list
    for (const auto &acc : loop.accesses) out.push_back(&acc);        // add this loop's own accesses
    for (const auto &ch  : loop.children) gatherAccesses(*ch, out);   // recurse into child loops
}

// Infer a flat list of ScheduledOps for one pipelined loop.
//
// Classification rules (derived from Stage 2 access patterns):
//   REDUCTION write + N reads   → Init, Load×N, MAC, Store
//   reads-only (no array write) → Init, Load×N, MAC          (store in parent scope)
//   SEQUENTIAL/other reads + write → Load×N, FAdd, Store
//
// The returned list is already in topological order: Init < Loads < Compute < Store.
static std::vector<ScheduledOp> inferOps(const AffineLoop &loop) {   //** core inference step: patterns → ops
    std::vector<ScheduledOp> ops;   // output list, built in topological order

    // Gather every access in the pipelined scope (this loop + all child loops).
    std::vector<const AffineAccess *> all;   // flat view of all accesses in scope
    gatherAccesses(loop, all);               // fills 'all' recursively

    // Detect which accumulator pattern applies.
    bool hasReduction = false;   // true when a REDUCTION-classified write exists
    bool readsOnly    = true;    // true when no write access exists at all
    for (const auto *a : all) {                                             // scan every access
        if (a->isWrite && a->pattern == AccessPattern::Reduction)           // explicit accumulator write?
            hasReduction = true;                                            // mark: has a reduction
        if (a->isWrite)                                                     // any write at all?
            readsOnly = false;                                              // mark: not reads-only
    }

    // Both flags can't be true at once: Reduction requires a write, so readsOnly would be false.
    bool isAccumulator = hasReduction || readsOnly; // either explicit (REDUCTION) or implied (scalar acc)

    // Running index used to fill depIds — must match insertion order below.
    int idx        = 0;   // tracks the current op's index in the ops vector
    int initIdx    = -1;  // index of the Init op (-1 if no accumulator)
    int computeIdx = -1;  // index of the Compute (FAdd or MAC) op
    std::vector<int> loadIndices; // indices of all Load ops (Compute waits for all of them)

    // ── Init op (only for accumulator patterns) ───────────────────────────────
    if (isAccumulator) {                         // emit Init only when there is an accumulator
        ScheduledOp op;                          // create the operation record
        op.id      = "init_acc";                 // label used in the schedule table
        op.kind    = OpKind::Init;               // Init kind → latency 1 from table
        op.latency = latencyOf(OpKind::Init);    // look up latency from the central table
        op.depIds  = {};                         // nothing to wait for — starts at cycle 0
        initIdx    = idx++;                      // record this op's index before pushing
        ops.push_back(op);                       // append to the topological list
    }

    // ── Load ops — one per READ access in the pipeline scope ─────────────────
    for (const auto *a : all) {                      // scan all accesses gathered above
        if (a->isWrite) continue;                    // skip write accesses in this pass
        ScheduledOp op;                              // create a Load operation
        op.id        = "load_" + a->array;           // e.g. "load_A", "load_in_signal"
        op.kind      = OpKind::Load;                 // Load kind
        op.array     = a->array;                     // which array this load reads
        op.indexExpr = a->indexExpr;                 // index expression e.g. "i * N + k"
        op.latency   = latencyOf(OpKind::Load);      // look up latency
        op.depIds    = {};                           // loads have no op-level predecessor
        loadIndices.push_back(idx++);               // record this Load's index
        ops.push_back(op);                          // append to the topological list
    }

    // ── Compute op — MAC for accumulators, FAdd for simple additions ──────────
    {
        ScheduledOp op;                                              // create the compute operation
        op.kind    = isAccumulator ? OpKind::MAC : OpKind::FAdd;    //** choose MAC or FAdd based on pattern
        op.id      = (op.kind == OpKind::MAC) ? "mac" : "fadd";     // label for the schedule table
        op.latency = latencyOf(op.kind);                             // look up latency
        op.depIds  = loadIndices;                                    // must wait for ALL loads to finish
        if (initIdx >= 0) op.depIds.push_back(initIdx);             // also wait for accumulator init
        computeIdx = idx++;                                          // record this op's index
        ops.push_back(op);                                           // append to topological list
    }

    // ── Store op — only when there is an explicit array write ────────────────
    if (!readsOnly) {                                    // reads-only loops have no array write to store
        for (const auto *a : all) {                      // find the first write access
            if (!a->isWrite) continue;                   // skip read accesses
            ScheduledOp op;                              // create the Store operation
            op.id        = "store_" + a->array;          // e.g. "store_C", "store_out_signal"
            op.kind      = OpKind::Store;                // Store kind
            op.array     = a->array;                     // which array this store writes
            op.indexExpr = a->indexExpr;                 // index expression from Stage 1
            op.latency   = latencyOf(OpKind::Store);     // look up latency
            op.depIds    = {computeIdx};                 //** store must wait for compute to finish
            ops.push_back(op);                           // append to topological list
            break;                                       // one store per pipeline slot
        }
    }

    return ops;   // return flat op list in guaranteed topological order
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 2 — ASAP and ALAP scheduling passes
// ═══════════════════════════════════════════════════════════════════════════════

// ASAP pass: assign each op the earliest cycle all its inputs are ready.
// Precondition: ops are in topological order (inferOps guarantees this).
static void runASAP(std::vector<ScheduledOp> &ops) {  //** forward pass — O(N²) over the small op list
    for (int i = 0; i < (int)ops.size(); ++i) {       // process ops in order (topological = safe)
        int earliest = 0;                              // start as early as possible — cycle 0
        for (int dep : ops[i].depIds) {                // inspect every predecessor
            int depFinish = ops[dep].asap + ops[dep].latency; // predecessor finishes at this cycle
            earliest = std::max(earliest, depFinish);  //** push start right if a predecessor finishes later
        }
        ops[i].asap = earliest;                        // record the ASAP cycle for this op
    }
}

// Compute the pipeline depth: the cycle at which the last op finishes.
// This is the latency of one complete pipeline iteration.
static int computeDepth(const std::vector<ScheduledOp> &ops) {  // scan all ops to find the last finish
    int depth = 0;                                               // running maximum
    for (const auto &op : ops)                                   // check every op
        depth = std::max(depth, op.asap + op.latency);           // finish = start + latency
    return depth;                                                // depth = pipeline iteration latency
}

// ALAP pass: assign each op the latest cycle it can start without delaying
// any op that depends on it.  Backward pass from the pipeline depth.
static void runALAP(std::vector<ScheduledOp> &ops, int depth) {  //** backward pass — O(N²) over small list

    // Initialise every op's ALAP to the latest it could possibly finish.
    for (auto &op : ops)
        op.alap = depth - op.latency;  // latest start = depth minus own latency

    // Tighten ALAP by looking at every consumer (op j that depends on op i).
    for (int i = (int)ops.size() - 1; i >= 0; --i) {  // walk backward: consumers already tightened
        for (int j = i + 1; j < (int)ops.size(); ++j) {  // scan all later ops
            for (int dep : ops[j].depIds) {               // check each dependency of ops[j]
                if (dep != i) continue;                   // only care when ops[j] depends on ops[i]
                // ops[i] must finish before ops[j] can start:
                //   ops[i].alap + ops[i].latency  <=  ops[j].alap
                ops[i].alap = std::min(ops[i].alap,       //** tighten: can't start later than this
                                       ops[j].alap - ops[i].latency);
            }
        }
    }

    // Compute slack: how many cycles each op can slide without changing pipeline depth.
    for (auto &op : ops)
        op.slack = op.alap - op.asap;  //** slack = 0 means the op is on the critical path
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 3 — Build ScheduleContext from AffineContext
// ═══════════════════════════════════════════════════════════════════════════════

// Recursively build one ScheduledLoop from its AffineLoop counterpart.
static std::unique_ptr<ScheduledLoop> buildScheduledLoop(const AffineLoop &src) {
    auto dst = std::make_unique<ScheduledLoop>(); // allocate the Stage 3 loop node

    // Copy identity fields verbatim from Stage 2.
    dst->id      = src.id;       // loop ID e.g. "L0.L1"
    dst->line    = src.line;     // source line number
    dst->var     = src.var;      // induction variable name
    dst->lo      = src.lo;       // loop lower bound expression
    dst->hi      = src.hi;       // loop upper bound expression
    dst->pragmas = src.pragmas;  // attached pragmas (PIPELINE, UNROLL, etc.)

    // Carry II analysis results from Stage 2 — Stage 3 does not recompute them.
    dst->requestedII = src.requestedII; // user-requested II from PIPELINE pragma
    dst->resMII      = src.resMII;      // resource-constrained MII (always 1 here)
    dst->recMII      = src.recMII;      // recurrence-constrained MII from Stage 2
    dst->IIFeasible  = src.IIFeasible;  // whether the requested II is achievable

    // Only pipelined loops get a schedule — non-pipelined loops just propagate structure.
    bool isPipelined = (src.requestedII >= 0); // requestedII is -1 for non-pipelined loops
    if (isPipelined) {
        dst->ops           = inferOps(src);              //** Step 1: infer ops from access patterns
        runASAP(dst->ops);                               //** Step 2: ASAP forward pass
        dst->pipelineDepth = computeDepth(dst->ops);     //** Step 3: compute depth from ASAP results
        runALAP(dst->ops, dst->pipelineDepth);           //** Step 4: ALAP backward pass + slack
    }

    // Recurse into child loops regardless of pipeline status.
    for (const auto &ch : src.children)
        dst->children.push_back(buildScheduledLoop(*ch));  // recursively build child ScheduledLoop

    return dst;   // return fully scheduled loop node
}

void buildSchedule(const AffineContext &affCtx, ScheduleContext &schedCtx) {  //** top-level Stage 3 entry
    for (const auto &fn : affCtx.functions) {           // process every function from Stage 2
        ScheduledFunction sf;                            // create the Stage 3 function container
        sf.name   = fn.name;                            // copy function name
        sf.line   = fn.line;                            // copy source line
        sf.params = fn.params;                          // copy parameters (interface pragmas live here)
        for (const auto &loop : fn.loops)
            sf.loops.push_back(buildScheduledLoop(*loop));  // recursively build the Stage 3 loop tree
        schedCtx.functions.push_back(std::move(sf));    // append completed function to the context
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 4 — Pretty printer
// ═══════════════════════════════════════════════════════════════════════════════

static std::string pad(int d) { return std::string(d * 2, ' '); }  // 2*d spaces for indentation

static const char *opLabel(OpKind k) {   // fixed-width 5-char label for the Op column
    switch (k) {
    case OpKind::Init:  return "INIT ";   // initialise accumulator
    case OpKind::Load:  return "LOAD ";   // array read
    case OpKind::Store: return "STORE";   // array write
    case OpKind::FAdd:  return "FADD ";   // float add
    case OpKind::FMul:  return "FMUL ";   // float multiply
    case OpKind::MAC:   return "MAC  ";   // multiply-accumulate
    }
    return "?    ";                        // should not reach here
}

static std::string descOf(const ScheduledOp &op) {   // human-readable description for the table
    switch (op.kind) {
    case OpKind::Init:  return "acc = 0.0";           // accumulator initialisation
    case OpKind::Load:
    case OpKind::Store:                                // for Load/Store: show array[index]
        if (op.indexExpr.empty()) return op.array;     // no index available
        return op.array + "[" + op.indexExpr + "]";   // full subscript expression
    case OpKind::FAdd:  return "A + B";               // simple addition
    case OpKind::FMul:  return "A * B";               // simple multiply
    case OpKind::MAC:   return "acc += A * B";         // multiply-accumulate (loop-carried on acc)
    }
    return "—";   // fallback
}

static std::string pragmaTag(const HLSPragma &p) {   // short display label for inline pragma annotation
    if (p.kind == PragmaKind::Pipeline)               // PIPELINE pragma
        return "[PIPELINE II=" +
               std::to_string(std::get<PipelinePragma>(p.data).II) + "]";
    if (p.kind == PragmaKind::Unroll) {               // UNROLL pragma
        int f = std::get<UnrollPragma>(p.data).factor;   // extract factor
        return f == 0 ? "[UNROLL full]"               // full unroll
                      : "[UNROLL factor=" + std::to_string(f) + "]";  // partial unroll
    }
    return "";   // other pragma types don't need a display label in Stage 3
}

static void dumpScheduledLoop(const ScheduledLoop &loop, int depth) {  // print one loop and its schedule
    // ── Loop header ───────────────────────────────────────────────────────────
    std::cout << pad(depth)
              << loop.id
              << "  line " << loop.line
              << "  for " << loop.var
              << " in [" << loop.lo << ", " << loop.hi << ")";   // bounds
    for (const auto &p : loop.pragmas) {       // inline pragma annotations
        auto s = pragmaTag(p);
        if (!s.empty()) std::cout << "  " << s;
    }
    std::cout << "\n";

    // ── Schedule table (pipelined loops only) ─────────────────────────────────
    if (loop.requestedII >= 0 && !loop.ops.empty()) {
        int aii = std::max(loop.resMII, loop.recMII);   // achievable II = max(ResMII, RecMII)

        // Section header.
        std::cout << "\n" << pad(depth + 1)
                  << "\xe2\x94\x80\xe2\x94\x80 Schedule (ASAP / ALAP) "
                  << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                  << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                  << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n";

        // Column header row.
        std::cout << pad(depth + 1)
                  << "  " << std::left << std::setw(7)  << "Op"
                  << "  " << std::left << std::setw(26) << "Description"
                  << std::right
                  << std::setw(6) << "ASAP"
                  << std::setw(6) << "ALAP"
                  << std::setw(7) << "Slack"
                  << "  Cycles\n";

        // Separator line.
        std::cout << pad(depth + 1)
                  << "  " << std::string(58, '-') << "\n";

        // One row per scheduled operation.
        for (const auto &op : loop.ops) {                         // iterate over ASAP-ordered ops
            std::string desc = descOf(op);                        // get human-readable description
            if (desc.size() > 24) desc = desc.substr(0, 21) + "...";  // truncate very long index exprs

            std::cout << pad(depth + 1) << "  "
                      << std::left  << std::setw(7)  << opLabel(op.kind)   // Op column (5 chars + padding)
                      << std::left  << std::setw(26) << desc               // Description column
                      << std::right << std::setw(6)  << op.asap            // ASAP cycle
                      << std::right << std::setw(6)  << op.alap            // ALAP cycle
                      << std::right << std::setw(7)  << op.slack           // slack (0 = critical path)
                      << "  [" << op.asap << ", " << (op.asap + op.latency - 1) << "]\n";  // cycle range
        }

        // Summary line.
        std::cout << "\n" << pad(depth + 1)
                  << "Pipeline depth = " << loop.pipelineDepth << " cycles"
                  << "   II = " << aii;
        if (loop.IIFeasible)
            std::cout << "  \xe2\x9c\x93 feasible\n";   // tick — user's II request is achievable
        else
            std::cout << "  \xe2\x9c\x97 NOT feasible (need II >= " << aii << ")\n";  // cross
        std::cout << "\n";
    }

    // ── Recurse into child loops ───────────────────────────────────────────────
    for (const auto &ch : loop.children)
        dumpScheduledLoop(*ch, depth + 1);   // increase depth for each nesting level
}

void dumpSchedule(const ScheduleContext &schedCtx) {                  // print full Stage 3 IR to stdout
    std::cout << "=== FluxHLS Stage 3 Output ===\n\n";               // stage banner
    for (const auto &fn : schedCtx.functions) {                       // iterate over every function
        std::cout << "Function: " << fn.name
                  << "  (line " << fn.line << ")\n";                  // function name and source line
        std::cout << "  Parameters:\n";                               // parameter section header
        for (const auto &p : fn.params) {                             // print each parameter
            std::cout << "    " << p.type << " " << p.name;          // type and name
            for (const auto &pr : p.pragmas)                          // inline interface pragma
                if (pr.kind == PragmaKind::Interface)
                    std::cout << "  ["
                              << std::get<InterfacePragma>(pr.data).mode << "]";  // e.g. [m_axi]
            std::cout << "\n";
        }
        std::cout << "\n";                                            // blank line before loop listing
        for (const auto &loop : fn.loops)
            dumpScheduledLoop(*loop, 1);                              // print each top-level loop at depth 1
    }
}
