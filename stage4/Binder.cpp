#include "Binder.h"    // public API: buildBindings(), dumpBindings()
#include <algorithm>  // std::min, std::all_of
#include <iomanip>    // std::setw, std::left, std::right for column alignment
#include <iostream>   // std::cout for the pretty-printer
#include <map>        // std::map for grouping accesses by array and tracking port counters
#include <string>     // std::string, std::to_string
#include <vector>     // std::vector for access lists and binding lists

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 1 — Utility helpers
// ═══════════════════════════════════════════════════════════════════════════════

// Recursively collect all AffineAccess pointers from a loop subtree.
// Stage 4 needs the same full-scope view as Stage 3's inferOps.
static void gatherAccesses(const AffineLoop &loop,                    // root of the subtree
                            std::vector<const AffineAccess *> &out) { // output flat list
    for (const auto &acc : loop.accesses) out.push_back(&acc);       // this loop's own accesses
    for (const auto &ch  : loop.children) gatherAccesses(*ch, out);  // recurse into children
}

// Parse fixed numeric array dimensions from a C type string.
// "float [3][3]" → 9,  "float [16]" → 16,  "float *" → -1 (pointer, unknown).
// Returns -1 whenever any dimension is non-numeric.
static int parseArraySize(const std::string &type) {
    int    total = 1;    // running product of all numeric dimensions
    bool   found = false; // whether at least one '[N]' bracket pair was seen
    size_t i     = 0;    // current scan position in the type string
    while ((i = type.find('[', i)) != std::string::npos) { // scan for every '[' in the type
        size_t j = type.find(']', i);                      // find the matching ']'
        if (j == std::string::npos) return -1;             // malformed type — bail out
        std::string dim = type.substr(i + 1, j - i - 1);  // text between '[' and ']'
        if (dim.empty() ||                                 // empty dimension is non-numeric
            !std::all_of(dim.begin(), dim.end(), ::isdigit)) return -1; // variable dim — bail
        total *= std::stoi(dim);  // multiply numeric dimension into the running product
        found = true;             // at least one numeric dimension seen
        i     = j + 1;           // advance past ']'
    }
    return found ? total : -1;  // return product, or -1 if no dimension brackets found
}

// For a SLIDING_WINDOW access, find the inner child loop whose induction variable
// appears in the index expression — that loop's upper bound is the shift-register depth.
// Returns "?" if no matching child is found.
static std::string findSlidingDepth(const std::string &indexExpr,    // e.g. "i - t"
                                    const AffineLoop  &pipelineLoop) { // the pipelined loop
    for (const auto &child : pipelineLoop.children) {                 // scan immediate children
        if (indexExpr.find(child->var) != std::string::npos)          // inner var in index?
            return child->hi;                                          // return that loop's bound
    }
    return "?";  // no matching child — use placeholder
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 2 — Array binding (5-rule table)
// ═══════════════════════════════════════════════════════════════════════════════

// Short label for AccessPattern — used in the array-binding table display.
static const char *patternLabel(AccessPattern p) {
    switch (p) {
    case AccessPattern::Sequential:    return "SEQUENTIAL";
    case AccessPattern::Strided:       return "STRIDED";
    case AccessPattern::Reduction:     return "REDUCTION";
    case AccessPattern::SlidingWindow: return "SLIDING_WINDOW";
    default:                           return "UNKNOWN";
    }
}

// Apply the 5-rule binding table to one array and produce its ArrayBinding.
//
// Rule priority (highest wins):
//   Rule 1: param type has fixed dims ≤ 64 elements  → Register (complete partition)
//   Rule 2: SLIDING_WINDOW access                    → ShiftRegister (depth = inner loop bound)
//   Rule 3: STRIDED access (stride = N)              → BRAM  (N-bank cyclic partition)
//   Rule 4: REDUCTION write                          → BRAM  (accumulator lives in a register)
//   Rule 5: SEQUENTIAL (or UNKNOWN)                  → BRAM  (single-bank streaming)
static ArrayBinding makeArrayBinding(
    const std::string &array,                              // name of the array
    const std::vector<const AffineAccess *> &arrayAccs,   // all accesses to this array
    const std::vector<HLSParam>  &params,                 // function params (for size check)
    const AffineLoop &pipelineLoop)                       // needed for sliding-window depth
{
    ArrayBinding b;             // result to fill
    b.array = array;            // store the array name

    // Count simultaneous read and write ports needed in one pipeline iteration.
    b.readPorts  = 0;           // accumulte read port count
    b.writePorts = 0;           // accumulate write port count
    for (auto *acc : arrayAccs) {
        if (acc->isWrite) b.writePorts++;  // each write access consumes one write port
        else              b.readPorts++;   // each read access consumes one read port
    }

    // Find the dominant access pattern (SlidingWindow > Strided > Reduction > Sequential).
    b.pattern = AccessPattern::Unknown;  // start with no pattern
    std::string stride = "1";            // stride value string (used for Strided label)
    for (auto *acc : arrayAccs) {
        if (acc->pattern == AccessPattern::SlidingWindow) { // SlidingWindow always wins
            b.pattern = AccessPattern::SlidingWindow;       // set pattern and stop scanning
            break;
        }
        if (acc->pattern == AccessPattern::Strided &&
            b.pattern    != AccessPattern::SlidingWindow) { // Strided beats lower patterns
            b.pattern = AccessPattern::Strided;             // update dominant pattern
            stride    = acc->stride;                        // save stride value for display
        }
        if (b.pattern == AccessPattern::Unknown ||
            b.pattern == AccessPattern::Sequential) {
            b.pattern = acc->pattern;  // Sequential / Reduction fill in when nothing stronger
        }
    }

    // ── Rule 1: small fixed-size function parameter → Register ───────────────
    for (const auto &p : params) {
        if (p.name != array) continue;          // skip params that don't match this array
        int sz = parseArraySize(p.type);        // extract product of all numeric dimensions
        if (sz > 0 && sz <= 64) {              //** threshold: ≤ 64 floats fits in LUT registers
            b.resource  = ResourceKind::Register;
            b.configStr = std::to_string(sz) + " regs";
            b.reason    = "small fixed param (" + std::to_string(sz) +
                          " elem ≤ 64) → complete register partition";
            return b;  // Rule 1 wins — return immediately
        }
        break;  // param found but size check failed — fall through to pattern rules
    }

    // ── Rule 2: SLIDING_WINDOW → ShiftRegister ───────────────────────────────
    if (b.pattern == AccessPattern::SlidingWindow) {
        std::string depth = "?";                       // default placeholder
        for (auto *acc : arrayAccs) {
            if (acc->pattern == AccessPattern::SlidingWindow) {
                depth = findSlidingDepth(acc->indexExpr, pipelineLoop); // look up inner loop hi
                break;
            }
        }
        b.resource  = ResourceKind::ShiftRegister;
        b.configStr = "depth=" + depth;                // e.g. "depth=TAPS"
        b.reason    = "sliding window (depth=" + depth + ") → SRL16 shift-register line buffer";
        return b;  // Rule 2 wins
    }

    // ── Rule 3: STRIDED → BRAM N-bank cyclic partition ───────────────────────
    if (b.pattern == AccessPattern::Strided) {
        b.resource  = ResourceKind::BRAM;
        b.configStr = stride + " banks cyclic";        // e.g. "N banks cyclic"
        b.reason    = "strided (stride=" + stride + ") → " + stride + "-bank cyclic BRAM";
        return b;  // Rule 3 wins
    }

    // ── Rule 4: REDUCTION → BRAM (accumulator register is separate) ──────────
    if (b.pattern == AccessPattern::Reduction) {
        b.resource  = ResourceKind::BRAM;
        b.configStr = "1 bank";
        b.reason    = "reduction write → BRAM (accumulator lives in a separate register)";
        return b;  // Rule 4 wins
    }

    // ── Rule 5: SEQUENTIAL (default) → single-bank BRAM ─────────────────────
    b.resource  = ResourceKind::BRAM;
    b.configStr = "1 bank";
    b.reason    = "sequential access → single-bank BRAM";
    return b;  // Rule 5 applies for Sequential and Unknown patterns
}

// Build the full ArrayBinding list for a pipelined loop.
// Each unique array in the gathered access set gets exactly one ArrayBinding.
static std::vector<ArrayBinding> bindArrays(
    const std::vector<const AffineAccess *> &accesses,  // all accesses in pipeline scope
    const std::vector<HLSParam>  &params,               // function params for size detection
    const AffineLoop &pipelineLoop)                     // root loop (for sliding-window depth)
{
    // Collect unique array names preserving first-appearance order.
    std::vector<std::string> order;                                   // insertion-order names
    std::map<std::string, std::vector<const AffineAccess *>> byArr;  // accesses per array
    for (auto *acc : accesses) {
        if (byArr.find(acc->array) == byArr.end())   // first time seeing this array?
            order.push_back(acc->array);             // record name in order of appearance
        byArr[acc->array].push_back(acc);            // append access to its group
    }

    std::vector<ArrayBinding> result;          // output list
    for (const auto &arr : order) {           // process each array in appearance order
        result.push_back(
            makeArrayBinding(arr, byArr[arr], params, pipelineLoop)); // apply 5-rule table
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 3 — Op binding (assign each ScheduledOp to a hardware unit)
// ═══════════════════════════════════════════════════════════════════════════════

// Produce a human-readable description for one op (mirrors Stage 3's descOf).
static std::string opDesc(const ScheduledOp &op) {
    switch (op.kind) {
    case OpKind::Init:  return "acc = 0.0";                          // accumulator reset
    case OpKind::Load:
    case OpKind::Store:
        if (op.indexExpr.empty()) return op.array;                   // no index available
        return op.array + "[" + op.indexExpr + "]";                  // array[index]
    case OpKind::FAdd:  return "A + B";                              // float add
    case OpKind::FMul:  return "A * B";                              // float multiply
    case OpKind::MAC:   return "acc += A * B";                       // multiply-accumulate
    }
    return "\xe2\x80\x94";  // em dash fallback — should not reach here
}

// Map each ScheduledOp to a specific hardware unit.
//
// Assignment rules:
//   Init  → Register_<reduction_array>_acc  (or "acc_reg" if no REDUCTION write visible)
//   Load  → <resource_prefix>_<array>.port<N>   (N = next free port for this array)
//   Store → <resource_prefix>_<array>.port0     (stores always use port0 = write port)
//   FAdd / FMul / MAC → DSP48_<N>               (N = sequential counter within this loop)
static std::vector<OpBinding> bindOps(
    const std::vector<ScheduledOp>          &ops,          // Stage 3 ops in topological order
    const std::vector<const AffineAccess *> &accesses,     // for finding REDUCTION array name
    const std::vector<ArrayBinding>         &arrayBindings) // array → resource map
{
    // Build a quick-lookup map: array name → hardware unit prefix.
    // e.g. "A" → "BRAM_A",  "in_signal" → "ShiftReg_in_signal"
    std::map<std::string, std::string> unitPrefix; // array → unit prefix string
    for (const auto &ab : arrayBindings) {
        switch (ab.resource) {
        case ResourceKind::BRAM:          unitPrefix[ab.array] = "BRAM_"     + ab.array; break;
        case ResourceKind::ShiftRegister: unitPrefix[ab.array] = "ShiftReg_" + ab.array; break;
        case ResourceKind::Register:      unitPrefix[ab.array] = "Reg_"      + ab.array; break;
        default:                          unitPrefix[ab.array] = ab.array;               break;
        }
    }

    // Find the REDUCTION write array — used to name the accumulator register for Init ops.
    std::string reductionArray;                // name of the REDUCTION target, if any
    for (auto *acc : accesses) {
        if (acc->isWrite && acc->pattern == AccessPattern::Reduction) {
            reductionArray = acc->array;  // take the first REDUCTION write found
            break;
        }
    }

    std::vector<OpBinding> result;             // output list
    int dspCounter = 0;                        //** sequential DSP48 index for this loop
    std::map<std::string, int> portCounter;    // array → next unassigned port index

    for (const auto &op : ops) {  // iterate in ASAP (topological) order
        OpBinding b;               // fill one binding entry per op
        b.opId  = op.id;           // copy the op identifier
        b.kind  = op.kind;         // copy the op kind
        b.cycle = op.asap;         // bind to the Stage 3 ASAP start cycle
        b.array = op.array;        // array touched (empty for compute ops)
        b.desc  = opDesc(op);      // human-readable description

        switch (op.kind) {

        case OpKind::Init:
            // The accumulator register is named after the REDUCTION target array.
            b.unit = reductionArray.empty()
                     ? "acc_reg"                               // unnamed scalar accumulator
                     : "Register_" + reductionArray + "_acc"; // named after its array
            break;

        case OpKind::Load: {
            int &port = portCounter[op.array];  // get and increment this array's port counter
            auto it   = unitPrefix.find(op.array);
            std::string pfx = (it != unitPrefix.end()) ? it->second : ("BRAM_" + op.array);
            b.unit = pfx + ".port" + std::to_string(port++); //** assign port and advance counter
            break;
        }

        case OpKind::Store: {
            auto it = unitPrefix.find(op.array);
            std::string pfx = (it != unitPrefix.end()) ? it->second : ("BRAM_" + op.array);
            b.unit = pfx + ".port0";  // stores always write to port0 (BRAM write port)
            break;
        }

        case OpKind::FAdd:
        case OpKind::FMul:
        case OpKind::MAC:
            b.unit = "DSP48_" + std::to_string(dspCounter++); //** assign next DSP48 block
            break;
        }

        result.push_back(b);  // append completed binding
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 4 — Resource counting
// ═══════════════════════════════════════════════════════════════════════════════

// Tally hardware resource usage for one pipelined BoundLoop after binding.
// Counts from both op bindings (DSP48, accumulator regs) and array bindings (BRAM, etc.).
static void countResources(BoundLoop &loop) {
    loop.numDSP48s    = 0;  // reset before counting
    loop.numBRAMs     = 0;
    loop.numShiftRegs = 0;
    loop.numRegisters = 0;

    // Count compute units and implicit accumulator registers from op bindings.
    for (const auto &ob : loop.opBindings) {
        if (ob.kind == OpKind::FAdd ||
            ob.kind == OpKind::FMul ||
            ob.kind == OpKind::MAC)   loop.numDSP48s++;    //** each compute op uses one DSP48
        if (ob.kind == OpKind::Init)  loop.numRegisters++; //** each Init op needs one acc register
    }

    // Count memory resources from array bindings (one entry per distinct array).
    for (const auto &ab : loop.arrayBindings) {
        switch (ab.resource) {
        case ResourceKind::BRAM:          loop.numBRAMs++;     break; // logical BRAM (may be N-bank)
        case ResourceKind::ShiftRegister: loop.numShiftRegs++; break; // one SRL16 chain
        case ResourceKind::Register:      loop.numRegisters++; break; // small-array reg partition
        default: break;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 5 — Build BindingContext from AffineContext + ScheduleContext
// ═══════════════════════════════════════════════════════════════════════════════

// Recursively build one BoundLoop by walking the AffineLoop and ScheduledLoop trees in parallel.
// Both trees mirror the same HLSLoop structure so child index matching is always valid.
static std::unique_ptr<BoundLoop> bindLoop(
    const AffineLoop    &aff,    // Stage 2 loop node — provides access patterns
    const ScheduledLoop &sched,  // Stage 3 loop node — provides ASAP cycles and ops
    const std::vector<HLSParam> &params)  // function params (for small-array size check)
{
    auto bl = std::make_unique<BoundLoop>(); // allocate the Stage 4 loop node

    // Copy identity and II fields from the Stage 3 node verbatim.
    bl->id           = sched.id;
    bl->line         = sched.line;
    bl->var          = sched.var;
    bl->lo           = sched.lo;
    bl->hi           = sched.hi;
    bl->pragmas      = sched.pragmas;
    bl->requestedII  = sched.requestedII;
    bl->resMII       = sched.resMII;
    bl->recMII       = sched.recMII;
    bl->IIFeasible   = sched.IIFeasible;
    bl->pipelineDepth= sched.pipelineDepth;

    // Only pipelined loops receive array and op bindings.
    if (sched.requestedII >= 0) {                                       // -1 means not pipelined
        std::vector<const AffineAccess *> accesses;                     // flat access list
        gatherAccesses(aff, accesses);                                  // recurse AffineLoop subtree

        bl->arrayBindings = bindArrays(accesses, params, aff);          //** apply 5-rule table
        bl->opBindings    = bindOps(sched.ops, accesses, bl->arrayBindings); //** assign units
        countResources(*bl);                                            //** tally resource counts
    }

    // Recurse into child loops — walk both trees in lockstep by index.
    size_t n = std::min(aff.children.size(), sched.children.size()); // guard against mismatch
    for (size_t i = 0; i < n; ++i)
        bl->children.push_back(bindLoop(*aff.children[i], *sched.children[i], params));

    return bl;
}

// Top-level entry: build a BindingContext from AffineContext (Stage 2) and ScheduleContext (Stage 3).
void buildBindings(const AffineContext   &affCtx,   //** Stage 2 IR input
                   const ScheduleContext &schedCtx,  //** Stage 3 IR input
                   BindingContext        &bindCtx)   //** Stage 4 IR output
{
    // Walk AffineContext and ScheduleContext function lists in lockstep.
    size_t nFns = std::min(affCtx.functions.size(), schedCtx.functions.size());
    for (size_t fi = 0; fi < nFns; ++fi) {                    // one pass per function
        const auto &affFn   = affCtx.functions[fi];           // Stage 2 function
        const auto &schedFn = schedCtx.functions[fi];         // Stage 3 function

        BoundFunction bf;                   // Stage 4 function container
        bf.name   = schedFn.name;           // copy function name
        bf.line   = schedFn.line;           // copy source line
        bf.params = schedFn.params;         // copy params (carry interface pragma info)

        size_t nLoops = std::min(affFn.loops.size(), schedFn.loops.size());
        for (size_t li = 0; li < nLoops; ++li)
            bf.loops.push_back(bindLoop(*affFn.loops[li], *schedFn.loops[li], bf.params));

        bindCtx.functions.push_back(std::move(bf));  // append completed function
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 6 — Pretty printer
// ═══════════════════════════════════════════════════════════════════════════════

static std::string pad(int d) { return std::string(d * 2, ' '); }  // 2*d spaces for indentation

// Fixed-width resource label for the Array Bindings table Resource column.
static const char *resourceLabel(ResourceKind r) {
    switch (r) {
    case ResourceKind::BRAM:          return "BRAM         ";
    case ResourceKind::ShiftRegister: return "ShiftRegister";
    case ResourceKind::Register:      return "Register     ";
    case ResourceKind::DSP48:         return "DSP48        ";
    }
    return "?            ";
}

// Fixed-width 5-char op label — matches Stage 3's opLabel for consistency.
static const char *opLabel(OpKind k) {
    switch (k) {
    case OpKind::Init:  return "INIT ";
    case OpKind::Load:  return "LOAD ";
    case OpKind::Store: return "STORE";
    case OpKind::FAdd:  return "FADD ";
    case OpKind::FMul:  return "FMUL ";
    case OpKind::MAC:   return "MAC  ";
    }
    return "?    ";
}

// Short inline pragma annotation — same logic as Stage 3's pragmaTag.
static std::string pragmaTag(const HLSPragma &p) {
    if (p.kind == PragmaKind::Pipeline)
        return "[PIPELINE II=" +
               std::to_string(std::get<PipelinePragma>(p.data).II) + "]";
    if (p.kind == PragmaKind::Unroll) {
        int f = std::get<UnrollPragma>(p.data).factor;
        return f == 0 ? "[UNROLL full]"
                      : "[UNROLL factor=" + std::to_string(f) + "]";
    }
    return "";  // other pragma kinds have no display tag in Stage 4
}

// Unicode horizontal rule character (UTF-8 encoding of U+2500 BOX DRAWINGS LIGHT HORIZONTAL).
static const char *HR = "\xe2\x94\x80";  // single ─ character (3 bytes)

// Build a string of N horizontal-rule characters.
static std::string hrs(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += HR;  // concatenate N copies of ─
    return s;
}

static void dumpBoundLoop(const BoundLoop &loop, int depth) {  // print one loop node and its tables
    // ── Loop header ───────────────────────────────────────────────────────────
    std::cout << pad(depth)
              << loop.id
              << "  line " << loop.line
              << "  for " << loop.var
              << " in [" << loop.lo << ", " << loop.hi << ")";
    for (const auto &p : loop.pragmas) {   // inline pragma annotations
        auto s = pragmaTag(p);
        if (!s.empty()) std::cout << "  " << s;
    }
    std::cout << "\n";

    // ── Binding tables (only for pipelined loops that have bindings) ──────────
    bool hasTables = (loop.requestedII >= 0) &&
                     (!loop.arrayBindings.empty() || !loop.opBindings.empty());
    if (hasTables) {

        // ── Array Bindings table ───────────────────────────────────────────────
        if (!loop.arrayBindings.empty()) {
            std::cout << "\n" << pad(depth + 1)
                      << HR << HR << " Array Bindings " << hrs(44) << "\n";

            // Column header
            std::cout << pad(depth + 1)
                      << "  " << std::left << std::setw(18) << "Array"
                      << std::left << std::setw(15) << "Resource"
                      << std::left << std::setw(18) << "Config"
                      << std::left << std::setw(10) << "Ports"
                      << "Pattern\n";
            std::cout << pad(depth + 1) << "  " << std::string(72, '-') << "\n";

            for (const auto &ab : loop.arrayBindings) {
                std::string ports = std::to_string(ab.readPorts)  + "R " +
                                    std::to_string(ab.writePorts) + "W";
                std::cout << pad(depth + 1) << "  "
                          << std::left << std::setw(18) << ab.array          // array name
                          << std::left << std::setw(15) << resourceLabel(ab.resource) // resource
                          << std::left << std::setw(18) << ab.configStr      // config detail
                          << std::left << std::setw(10) << ports             // port count
                          << patternLabel(ab.pattern) << "\n";               // access pattern
            }
        }

        // ── Op Bindings table ──────────────────────────────────────────────────
        if (!loop.opBindings.empty()) {
            std::cout << "\n" << pad(depth + 1)
                      << HR << HR << " Op Bindings " << hrs(47) << "\n";

            // Column header
            std::cout << pad(depth + 1)
                      << "  " << std::left << std::setw(7)  << "Op"
                      << std::left << std::setw(30) << "Description"
                      << std::left << std::setw(28) << "Unit"
                      << "Cycle\n";
            std::cout << pad(depth + 1) << "  " << std::string(72, '-') << "\n";

            for (const auto &ob : loop.opBindings) {
                std::string desc = ob.desc;
                if (desc.size() > 28) desc = desc.substr(0, 25) + "...";  // truncate long exprs
                std::string unit = ob.unit;
                if (unit.size() > 26) unit = unit.substr(0, 23) + "...";  // truncate long unit names
                std::cout << pad(depth + 1) << "  "
                          << std::left << std::setw(7)  << opLabel(ob.kind)  // Op column
                          << std::left << std::setw(30) << desc              // Description
                          << std::left << std::setw(28) << unit              // Unit
                          << ob.cycle << "\n";                               // Cycle
            }
        }

        // ── Resource summary ───────────────────────────────────────────────────
        int aii = std::max(loop.resMII, loop.recMII);  // achievable II
        std::cout << "\n" << pad(depth + 1)
                  << "Resources: "
                  << loop.numDSP48s    << " DSP48  "
                  << loop.numBRAMs     << " BRAM  "
                  << loop.numShiftRegs << " ShiftReg  "
                  << loop.numRegisters << " Reg\n";
        std::cout << pad(depth + 1)
                  << "Pipeline depth = " << loop.pipelineDepth << " cycles"
                  << "   II = " << aii;
        if (loop.IIFeasible)
            std::cout << "  \xe2\x9c\x93 feasible\n\n";    // ✓
        else
            std::cout << "  \xe2\x9c\x97 NOT feasible (need II >= " << aii << ")\n\n";  // ✗
    }

    // ── Recurse into child loops ───────────────────────────────────────────────
    for (const auto &ch : loop.children)
        dumpBoundLoop(*ch, depth + 1);  // increase indentation depth for each nesting level
}

void dumpBindings(const BindingContext &bindCtx) {                    // top-level printer entry
    std::cout << "=== FluxHLS Stage 4 Output ===\n\n";               // stage banner
    for (const auto &fn : bindCtx.functions) {                        // iterate functions
        std::cout << "Function: " << fn.name
                  << "  (line " << fn.line << ")\n";                  // function name + line
        std::cout << "  Parameters:\n";                               // parameter section
        for (const auto &p : fn.params) {
            std::cout << "    " << p.type << " " << p.name;          // type and name
            for (const auto &pr : p.pragmas)
                if (pr.kind == PragmaKind::Interface)
                    std::cout << "  ["
                              << std::get<InterfacePragma>(pr.data).mode << "]";  // e.g. [m_axi]
            std::cout << "\n";
        }
        std::cout << "\n";                                            // blank line before loops
        for (const auto &loop : fn.loops)
            dumpBoundLoop(*loop, 1);                                  // print each top-level loop
    }
}
