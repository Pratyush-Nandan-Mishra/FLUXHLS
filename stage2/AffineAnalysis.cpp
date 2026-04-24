#include "AffineAnalysis.h"    // AffineContext, AffineLoop, AccessPattern, DepEdge definitions
#include <algorithm>            // std::find, std::max
#include <iostream>             // std::cout for the pretty-printer
#include <string>               // std::string, std::to_string

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 1 — Index expression tokeniser and pattern classifier
// ═══════════════════════════════════════════════════════════════════════════════

// Split index string into identifier / operator tokens.
// "i * N + k"  →  ["i", "*", "N", "+", "k"]
static std::vector<std::string> tokenize(const std::string &s) {   //** break a raw index expression into a flat token list
    std::vector<std::string> toks;    // output: growing list of tokens
    std::string cur;                  // accumulator for the current alphanumeric run
    for (unsigned char c : s) {       // iterate over every byte of the expression string
        if (std::isalnum(c) || c == '_') {   // letter, digit, or underscore: still inside an identifier/number
            cur += c;                         // extend the current token
        } else {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }  // flush completed identifier token
            if (!std::isspace(c)) toks.push_back(std::string(1, c)); // non-space char is its own operator token
        }
    }
    if (!cur.empty()) toks.push_back(cur);  // flush the final token if the string ended mid-identifier
    return toks;                             // return the complete flat token list
}

static bool hasVar(const std::vector<std::string> &toks, const std::string &var) {  // check if a variable name exists in the token list
    return std::find(toks.begin(), toks.end(), var) != toks.end();  // linear search; tokens are small so linear is fine
}

// True when <var> appears next to a "*" token  (e.g. "k * N" or "N * k").
static bool isMultiplied(const std::vector<std::string> &toks,  // token list of the index expression
                          const std::string &var) {              // variable to test
    for (int i = 0; i < (int)toks.size(); ++i) {   // scan every token position
        if (toks[i] != var) continue;               // skip tokens that are not the target variable
        if (i + 1 < (int)toks.size() && toks[i + 1] == "*") return true;  //** var immediately followed by * → strided
        if (i - 1 >= 0              && toks[i - 1] == "*") return true;   //** var immediately preceded by * → strided
    }
    return false;  // variable found but never adjacent to a multiplication operator
}

// Return the token that multiplies <var>, e.g. "N" for "k * N".
static std::string strideOf(const std::vector<std::string> &toks,  // token list of the index expression
                              const std::string &var) {              // the loop induction variable
    for (int i = 0; i < (int)toks.size(); ++i) {   // scan every token position
        if (toks[i] != var) continue;               // skip until we reach the target variable
        if (i + 1 < (int)toks.size() && toks[i + 1] == "*"
            && i + 2 < (int)toks.size())
            return toks[i + 2];                     //** "var * X" form — the stride is the token after *
        if (i - 1 >= 0 && toks[i - 1] == "*" && i - 2 >= 0)
            return toks[i - 2];                     //** "X * var" form — the stride is the token before *
    }
    return "1";  // no explicit multiplier found; the implied stride is 1
}

// Classify one array access given the loop context.
//   loopVar   — this loop's induction variable
//   outerVars — enclosing loops' variables (outermost first)
//   childVars — direct child loops' variables
static AccessPattern classify(const std::string &expr,                     // raw index expression string from Stage 1
                               const std::string &loopVar,                  // induction variable of the current loop
                               const std::vector<std::string> &outerVars,  // variables of all enclosing loops
                               const std::vector<std::string> &childVars,  // variables of direct child loops
                               bool isWrite) {                              // true = write access, false = read
    auto toks = tokenize(expr);     // tokenise the index expression before applying the classification rules

    // Reduction: a write whose index doesn't reference any child loop variable.
    // This means the value is loop-invariant w.r.t. the innermost child loop
    // → it is written once per (parent) iteration after the child loop drains.
    if (isWrite && !childVars.empty()) {   //** only applies to write accesses that have at least one child loop
        bool usesChild = false;            // assume no child variable appears in the index
        for (const auto &cv : childVars)  // check each child loop's induction variable
            if (hasVar(toks, cv)) { usesChild = true; break; }  // child var found → this is NOT a reduction
        if (!usesChild) return AccessPattern::Reduction;  //** none of the child vars appear → REDUCTION
    }

    bool usesLoop = hasVar(toks, loopVar);   // check whether this loop's own variable appears in the index
    if (!usesLoop) return AccessPattern::Unknown;  // loop variable absent — cannot classify the access

    // Sliding window: subtraction AND an outer loop variable present.
    // Classic case: in_signal[i - t]  where i is outer, t is inner.
    bool hasMinus  = expr.find('-') != std::string::npos;  // check for a subtraction in the raw expression string
    bool usesOuter = false;                // assume no outer loop variable appears in the index
    for (const auto &ov : outerVars)       // check each enclosing loop's variable
        if (hasVar(toks, ov)) { usesOuter = true; break; }  // outer variable found in the index
    if (hasMinus && usesOuter) return AccessPattern::SlidingWindow;  //** subtract + outer var → SLIDING_WINDOW

    // Strided: loop var appears with a multiplicative factor.
    if (isMultiplied(toks, loopVar)) return AccessPattern::Strided;  //** var has a multiplier → STRIDED (e.g. B[k*N+j])

    return AccessPattern::Sequential;  // none of the special patterns matched → default SEQUENTIAL stride-1
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 2 — Build AffineContext from HLSContext
// ═══════════════════════════════════════════════════════════════════════════════

// Collect every AffineAccess reachable from a loop (self + all descendants).
static void collectAllAccesses(const AffineLoop &loop,                   // root of the subtree to collect from
                                std::vector<const AffineAccess *> &out) { // output list of access pointers
    for (const auto &acc : loop.accesses) out.push_back(&acc);  // add this loop's own accesses
    for (const auto &ch  : loop.children)  collectAllAccesses(*ch, out);  // recurse into each child loop
}

// True when the subtree rooted at <loop> contains at least one write access.
static bool subtreeHasWrite(const AffineLoop &loop) {   // check this loop and all its descendants
    for (const auto &acc : loop.accesses)               // check accesses placed directly on this loop
        if (acc.isWrite) return true;                   // write found — stop searching immediately
    for (const auto &ch : loop.children)                // check child loops recursively
        if (subtreeHasWrite(*ch)) return true;          // a descendant has a write
    return false;                                       // no write found anywhere in the subtree
}

// Recursively build one AffineLoop from its HLSLoop counterpart.
static std::unique_ptr<AffineLoop> buildAffineLoop(   //** core Stage 2 transformation: HLSLoop → AffineLoop
        const HLSLoop                  &src,           // source Stage 1 loop node (read-only)
        const std::vector<std::string> &outerVars) {   // induction variables of all enclosing loops

    auto dst = std::make_unique<AffineLoop>();  // allocate the new Stage 2 loop node
    dst->id      = src.id;      // copy loop identifier string e.g. "L0.L1"
    dst->line    = src.line;    // copy source line number of the for-statement
    dst->var     = src.var;     // copy induction variable name
    dst->lo      = src.lo;      // copy loop lower bound expression
    dst->hi      = src.hi;      // copy loop upper bound expression
    dst->pragmas = src.pragmas; // copy attached pragmas (PIPELINE, UNROLL, etc.)

    // Direct child loop variables — needed for Reduction detection.
    std::vector<std::string> childVars;         // collect the induction variable of each direct child
    for (const auto &ch : src.children)
        childVars.push_back(ch->var);           // add child's induction variable to the list

    // ── Classify all accesses that Stage 1 placed on this loop ────────────────
    for (const auto &acc : src.accesses) {      // iterate over every array access recorded by Stage 1
        AffineAccess aa;                         // create a new enriched access descriptor
        aa.array     = acc.array;               // copy the array name
        aa.isWrite   = acc.isWrite;             // copy the read/write flag
        aa.indexExpr = acc.index;               // copy the raw index expression string
        aa.pattern   = classify(acc.index, src.var, outerVars, childVars,  //** run the 4-rule classifier
                                acc.isWrite);   // pass isWrite so Reduction rule is gated correctly
        switch (aa.pattern) {
        case AccessPattern::Strided:
            aa.stride = strideOf(tokenize(acc.index), src.var);  // extract the stride token (e.g. "N")
            break;
        case AccessPattern::Sequential:
        case AccessPattern::SlidingWindow:
            aa.stride = "1";               // stride-1: one element per iteration
            break;
        default:
            aa.stride = "\xe2\x80\x94";   // em dash — stride is not applicable for Reduction or Unknown
            break;
        }
        dst->accesses.push_back(std::move(aa));  // append the classified access to the Stage 2 loop node
    }

    // ── Recurse into children (must happen before II analysis) ────────────────
    std::vector<std::string> newOuter = outerVars;  // copy the current outer variable list for children
    newOuter.push_back(src.var);                    // this loop's variable becomes an outer var for its children
    for (const auto &ch : src.children)
        dst->children.push_back(buildAffineLoop(*ch, newOuter));  // recursively build each child AffineLoop

    // ── II analysis — only for loops with a PIPELINE pragma ───────────────────
    for (const auto &p : src.pragmas) {             // scan pragmas attached to this loop
        if (p.kind != PragmaKind::Pipeline) continue;  // skip non-pipeline pragmas
        dst->requestedII = std::get<PipelinePragma>(p.data).II;  //** extract the user-requested II value
        dst->resMII      = 1;   // resource-constrained MII: assume 1 DSP48 per cycle → 1 MAC/cycle

        // RecMII source 1: explicit Reduction access in this loop or a child.
        bool hasReduction = false;              // flag: set if we find a Reduction-classified write
        for (const auto &acc : dst->accesses)  // check accesses placed directly on this loop node
            if (acc.pattern == AccessPattern::Reduction)
                { hasReduction = true; break; }  //** direct reduction access found
        if (!hasReduction)                       // if not found at this level, check one level down
            for (const auto &ch : dst->children)
                for (const auto &acc : ch->accesses)
                    if (acc.pattern == AccessPattern::Reduction)
                        { hasReduction = true; break; }  // child loop has the reduction access

        // RecMII source 2: pipeline loop that only reads (e.g. conv2d kw loop).
        // The missing write is a scalar accumulator (e.g. sum += …) that Stage 1
        // doesn't track.  Conservative assumption: RecMII = 1.
        bool readsOnly = !subtreeHasWrite(*dst);  //** true when no array write exists anywhere in the subtree

        if (hasReduction) {                        // confirmed loop-carried dep via accumulator write
            dst->recMII       = 1;                 //** RecMII = 1: accumulator dep (latency=1, distance=1)
            dst->recMIIReason = "accumulator dep: latency=1, distance=1";  // reason stored for the pretty-printer
        } else if (readsOnly) {                    // reads-only pipeline: implied scalar accumulator
            dst->recMII       = 1;                 //** RecMII = 1: conservative heuristic for untracked scalar
            dst->recMIIReason = "reads-only loop \xe2\x86\x92 scalar accumulator (e.g. sum +=)";  // explain the heuristic
        } else {
            dst->recMII       = 0;                 // no loop-carried dep detected — purely data-parallel
            dst->recMIIReason = "no loop-carried dependences";  // pipeline is fully parallel
        }

        int achievedII  = std::max(dst->resMII, dst->recMII);   //** II lower bound = max(ResMII, RecMII)
        dst->IIFeasible = (dst->requestedII >= achievedII);      //** feasible if user's II ≥ achievable II

        // ── Build dependence edges ─────────────────────────────────────────────

        // Loop-carried dep from accumulator pattern.
        if (hasReduction || readsOnly) {             // accumulator dependency confirmed by either source
            std::string acc_name = hasReduction ? "accumulator" : "scalar acc";  // choose display label
            DepEdge e;                               // create the dependence edge record
            e.src         = acc_name;               // source of the edge: the accumulator variable
            e.dst         = acc_name;               // destination: same accumulator (self-loop in DDG)
            e.type        = DepType::RAW;            // read-after-write on the accumulator register
            e.loopCarried = true;                   // this dependence crosses iteration boundaries
            e.distance    = 1;                      // distance 1: each iteration reads the previous iteration's result
            dst->deps.push_back(e);                 // add loop-carried edge to the dependence graph
        }

        // Intra-iteration RAW: same array, one write, one or more reads.
        std::vector<const AffineAccess *> allAcc;   // flat list of all accesses in the full pipeline scope
        collectAllAccesses(*dst, allAcc);            // gather accesses from this loop and all its descendants
        for (const auto *wa : allAcc) {              // iterate over every access looking for writes
            if (!wa->isWrite) continue;              // skip read accesses in the outer scan
            for (const auto *ra : allAcc) {          // pair each write with every read
                if (ra->isWrite || ra->array != wa->array) continue;  // only same-array write→read pairs
                DepEdge e;                           // create the intra-iteration dependence edge
                e.src         = "READ "  + ra->array + "[" + ra->indexExpr + "]";  // read endpoint label
                e.dst         = "WRITE " + wa->array + "[" + wa->indexExpr + "]";  // write endpoint label
                e.type        = DepType::RAW;        // read-after-write dependence type
                e.loopCarried = false;               // within the same iteration — not loop-carried
                e.distance    = 0;                   // zero distance: both accesses happen in the same cycle scope
                dst->deps.push_back(e);              // add intra-iteration edge to the dependence graph
            }
        }
    }

    return dst;  //** return the fully enriched AffineLoop node with classified accesses and II analysis
}

void buildAffineContext(const HLSContext &ctx, AffineContext &affCtx) {  //** top-level Stage 2 entry point
    for (const auto &fn : ctx.functions) {          // process every function extracted by Stage 1
        AffineFunction af;                           // create the Stage 2 function container
        af.name   = fn.name;                        // copy function name
        af.line   = fn.line;                        // copy source line number
        af.params = fn.params;                      // copy parameter list (interface pragmas live here)
        for (const auto &loop : fn.loops)
            af.loops.push_back(buildAffineLoop(*loop, {}));  // recursively build the Stage 2 loop tree
        affCtx.functions.push_back(std::move(af));  // append the completed function to the Stage 2 IR
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 3 — Pretty printer
// ═══════════════════════════════════════════════════════════════════════════════

static std::string pad(int d) { return std::string(d * 2, ' '); }  // produce 2*d spaces for indentation

static const char *patternLabel(AccessPattern p) {   // convert AccessPattern enum to a display string
    switch (p) {
    case AccessPattern::Sequential:    return "SEQUENTIAL";      // stride-1 burst-friendly access
    case AccessPattern::Strided:       return "STRIDED";         // stride-N column-major access
    case AccessPattern::Reduction:     return "REDUCTION";       // loop-invariant write (accumulator store)
    case AccessPattern::SlidingWindow: return "SLIDING_WINDOW";  // index has subtraction plus outer variable
    default:                           return "UNKNOWN";          // unclassified access
    }
}

static const char *depLabel(DepType t) {   // convert dependence type enum to a display string
    switch (t) {
    case DepType::RAW: return "RAW";  // Read-After-Write
    case DepType::WAR: return "WAR";  // Write-After-Read
    case DepType::WAW: return "WAW";  // Write-After-Write
    }
    return "?";  // should not reach here
}

static std::string pragmaTag(const HLSPragma &p) {   // format a pragma as a short display label
    if (p.kind == PragmaKind::Pipeline)               // PIPELINE pragma
        return "[PIPELINE II=" +
               std::to_string(std::get<PipelinePragma>(p.data).II) + "]";  // include the II value
    if (p.kind == PragmaKind::Unroll) {               // UNROLL pragma
        int f = std::get<UnrollPragma>(p.data).factor;  // extract the unroll factor
        return f == 0 ? "[UNROLL full]"               // factor 0 means fully unroll
                      : "[UNROLL factor=" + std::to_string(f) + "]";  // partial unroll with factor
    }
    return "";  // other pragma types have no Stage 2 display label
}

static void dumpAffineLoop(const AffineLoop &loop, int depth) {  // recursively print one loop and its analysis
    // ── Loop header ───────────────────────────────────────────────────────────
    std::cout << pad(depth)                           // indent to the current nesting depth
              << loop.id                              // loop identifier e.g. "L0.L1"
              << "  line " << loop.line               // source line of the for-statement
              << "  for " << loop.var                 // induction variable name
              << " in [" << loop.lo << ", " << loop.hi << ")";  // loop trip bounds
    for (const auto &p : loop.pragmas) {              // print pragmas inline with the loop header
        auto s = pragmaTag(p);
        if (!s.empty()) std::cout << "  " << s;      // only print pragmas that have a Stage 2 display label
    }
    std::cout << "\n";                                // end the loop header line

    // ── Classified accesses ───────────────────────────────────────────────────
    for (const auto &acc : loop.accesses) {           // print each classified array access
        std::cout << pad(depth + 1)                   // indent one level deeper than the loop header
                  << (acc.isWrite ? "WRITE" : "READ ")  // write or read label
                  << "  " << acc.array << "[" << acc.indexExpr << "]"  // array name and index expression
                  << "   \xe2\x86\x92  " << patternLabel(acc.pattern);  // arrow then pattern label
        if (acc.pattern == AccessPattern::Strided)
            std::cout << "  stride=" << acc.stride;  // show the stride value only for strided accesses
        std::cout << "\n";
    }

    // ── II Analysis (pipelined loops only) ───────────────────────────────────
    if (loop.requestedII >= 0) {                      // only print when this loop has a PIPELINE pragma
        int aii = std::max(loop.resMII, loop.recMII); // compute achievable II = max(ResMII, RecMII)
        std::cout << "\n"
                  << pad(depth + 1) << "\xe2\x94\x80\xe2\x94\x80 II Analysis "
                  << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n";  // section header bar
        std::cout << pad(depth + 1)
                  << "ResMII = " << loop.resMII
                  << "  (1 MAC/cycle, 1 DSP48)\n";    // resource-constrained MII explanation
        std::cout << pad(depth + 1)
                  << "RecMII = " << loop.recMII
                  << "  (" << loop.recMIIReason << ")\n";  // recurrence-constrained MII with reason string
        std::cout << pad(depth + 1)
                  << "Achievable II = max(" << loop.resMII << ", "
                  << loop.recMII << ") = " << aii << "   ";  // show the max formula and its result
        if (loop.IIFeasible)
            std::cout << "\xe2\x86\x92  requested II=" << loop.requestedII
                      << " \xe2\x9c\x93 feasible\n";  // tick mark: user's II request can be met
        else
            std::cout << "\xe2\x86\x92  requested II=" << loop.requestedII
                      << " \xe2\x9c\x97 NOT feasible (need II >= " << aii << ")\n";  // cross: infeasible

        if (!loop.deps.empty()) {                     // print dependence edges only if there are any
            std::cout << "\n"
                      << pad(depth + 1)
                      << "\xe2\x94\x80\xe2\x94\x80 Dependences "
                      << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n";  // section header bar
            for (const auto &dep : loop.deps) {       // print each edge in the data dependence graph
                std::cout << pad(depth + 2)
                          << dep.src
                          << "  \xe2\x94\x80\xe2\x94\x80" << depLabel(dep.type);  // source label then dep type
                if (dep.loopCarried)
                    std::cout << " (loop-carried, dist=" << dep.distance << ")";  // annotate loop-carried distance
                std::cout << "\xe2\x94\x80\xe2\x94\x80\xe2\x96\xba  " << dep.dst << "\n";  // arrowhead and destination label
            }
        }
        std::cout << "\n";  // blank line after the II Analysis block
    }

    // ── Recurse ───────────────────────────────────────────────────────────────
    for (const auto &ch : loop.children)
        dumpAffineLoop(*ch, depth + 1);  // print each child loop indented one level deeper
}

void dumpAffineContext(const AffineContext &affCtx) {   // print the full Stage 2 IR to stdout
    std::cout << "=== FluxHLS Stage 2 Output ===\n\n";  // stage banner
    for (const auto &fn : affCtx.functions) {            // iterate over every analysed function
        std::cout << "Function: " << fn.name
                  << "  (line " << fn.line << ")\n";     // function name and source line number
        std::cout << "  Parameters:\n";                  // parameter section header
        for (const auto &p : fn.params) {                // iterate over each parameter
            std::cout << "    " << p.type << " " << p.name;  // print parameter type and name
            for (const auto &pr : p.pragmas)             // print interface pragma tag if present
                if (pr.kind == PragmaKind::Interface)
                    std::cout << "  ["
                              << std::get<InterfacePragma>(pr.data).mode << "]";  // e.g. [m_axi]
            std::cout << "\n";                           // newline after each parameter
        }
        std::cout << "\n";                               // blank line before the loop listing
        for (const auto &loop : fn.loops)
            dumpAffineLoop(*loop, 1);                    // print each top-level loop tree at depth 1
    }
}
