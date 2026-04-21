#include "AffineAnalysis.h"
#include <algorithm>
#include <iostream>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 1 — Index expression tokeniser and pattern classifier
// ═══════════════════════════════════════════════════════════════════════════════

// Split index string into identifier / operator tokens.
// "i * N + k"  →  ["i", "*", "N", "+", "k"]
static std::vector<std::string> tokenize(const std::string &s) {
    std::vector<std::string> toks;
    std::string cur;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '_') {
            cur += c;
        } else {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            if (!std::isspace(c)) toks.push_back(std::string(1, c));
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

static bool hasVar(const std::vector<std::string> &toks, const std::string &var) {
    return std::find(toks.begin(), toks.end(), var) != toks.end();
}

// True when <var> appears next to a "*" token  (e.g. "k * N" or "N * k").
static bool isMultiplied(const std::vector<std::string> &toks,
                          const std::string &var) {
    for (int i = 0; i < (int)toks.size(); ++i) {
        if (toks[i] != var) continue;
        if (i + 1 < (int)toks.size() && toks[i + 1] == "*") return true;
        if (i - 1 >= 0              && toks[i - 1] == "*") return true;
    }
    return false;
}

// Return the token that multiplies <var>, e.g. "N" for "k * N".
static std::string strideOf(const std::vector<std::string> &toks,
                              const std::string &var) {
    for (int i = 0; i < (int)toks.size(); ++i) {
        if (toks[i] != var) continue;
        if (i + 1 < (int)toks.size() && toks[i + 1] == "*"
            && i + 2 < (int)toks.size())
            return toks[i + 2];                  // var * X
        if (i - 1 >= 0 && toks[i - 1] == "*" && i - 2 >= 0)
            return toks[i - 2];                  // X * var
    }
    return "1";
}

// Classify one array access given the loop context.
//   loopVar   — this loop's induction variable
//   outerVars — enclosing loops' variables (outermost first)
//   childVars — direct child loops' variables
static AccessPattern classify(const std::string &expr,
                               const std::string &loopVar,
                               const std::vector<std::string> &outerVars,
                               const std::vector<std::string> &childVars,
                               bool isWrite) {
    auto toks = tokenize(expr);

    // Reduction: a write whose index doesn't reference any child loop variable.
    // This means the value is loop-invariant w.r.t. the innermost child loop
    // → it is written once per (parent) iteration after the child loop drains.
    if (isWrite && !childVars.empty()) {
        bool usesChild = false;
        for (const auto &cv : childVars)
            if (hasVar(toks, cv)) { usesChild = true; break; }
        if (!usesChild) return AccessPattern::Reduction;
    }

    bool usesLoop = hasVar(toks, loopVar);
    if (!usesLoop) return AccessPattern::Unknown;

    // Sliding window: subtraction AND an outer loop variable present.
    // Classic case: in_signal[i - t]  where i is outer, t is inner.
    bool hasMinus  = expr.find('-') != std::string::npos;
    bool usesOuter = false;
    for (const auto &ov : outerVars)
        if (hasVar(toks, ov)) { usesOuter = true; break; }
    if (hasMinus && usesOuter) return AccessPattern::SlidingWindow;

    // Strided: loop var appears with a multiplicative factor.
    if (isMultiplied(toks, loopVar)) return AccessPattern::Strided;

    return AccessPattern::Sequential;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 2 — Build AffineContext from HLSContext
// ═══════════════════════════════════════════════════════════════════════════════

// Collect every AffineAccess reachable from a loop (self + all descendants).
static void collectAllAccesses(const AffineLoop &loop,
                                std::vector<const AffineAccess *> &out) {
    for (const auto &acc : loop.accesses) out.push_back(&acc);
    for (const auto &ch  : loop.children)  collectAllAccesses(*ch, out);
}

// True when the subtree rooted at <loop> contains at least one write access.
static bool subtreeHasWrite(const AffineLoop &loop) {
    for (const auto &acc : loop.accesses)
        if (acc.isWrite) return true;
    for (const auto &ch : loop.children)
        if (subtreeHasWrite(*ch)) return true;
    return false;
}

// Recursively build one AffineLoop from its HLSLoop counterpart.
static std::unique_ptr<AffineLoop> buildAffineLoop(
        const HLSLoop                  &src,
        const std::vector<std::string> &outerVars) {

    auto dst = std::make_unique<AffineLoop>();
    dst->id      = src.id;
    dst->line    = src.line;
    dst->var     = src.var;
    dst->lo      = src.lo;
    dst->hi      = src.hi;
    dst->pragmas = src.pragmas;

    // Direct child loop variables — needed for Reduction detection.
    std::vector<std::string> childVars;
    for (const auto &ch : src.children)
        childVars.push_back(ch->var);

    // ── Classify all accesses that Stage 1 placed on this loop ────────────────
    for (const auto &acc : src.accesses) {
        AffineAccess aa;
        aa.array     = acc.array;
        aa.isWrite   = acc.isWrite;
        aa.indexExpr = acc.index;
        aa.pattern   = classify(acc.index, src.var, outerVars, childVars,
                                acc.isWrite);
        switch (aa.pattern) {
        case AccessPattern::Strided:
            aa.stride = strideOf(tokenize(acc.index), src.var);
            break;
        case AccessPattern::Sequential:
        case AccessPattern::SlidingWindow:
            aa.stride = "1";
            break;
        default:
            aa.stride = "\xe2\x80\x94";   // em dash — not applicable
            break;
        }
        dst->accesses.push_back(std::move(aa));
    }

    // ── Recurse into children (must happen before II analysis) ────────────────
    std::vector<std::string> newOuter = outerVars;
    newOuter.push_back(src.var);
    for (const auto &ch : src.children)
        dst->children.push_back(buildAffineLoop(*ch, newOuter));

    // ── II analysis — only for loops with a PIPELINE pragma ───────────────────
    for (const auto &p : src.pragmas) {
        if (p.kind != PragmaKind::Pipeline) continue;
        dst->requestedII = std::get<PipelinePragma>(p.data).II;
        dst->resMII      = 1;   // assume 1 DSP48, 1 MAC/cycle

        // RecMII source 1: explicit Reduction access in this loop or a child.
        bool hasReduction = false;
        for (const auto &acc : dst->accesses)
            if (acc.pattern == AccessPattern::Reduction)
                { hasReduction = true; break; }
        if (!hasReduction)
            for (const auto &ch : dst->children)
                for (const auto &acc : ch->accesses)
                    if (acc.pattern == AccessPattern::Reduction)
                        { hasReduction = true; break; }

        // RecMII source 2: pipeline loop that only reads (e.g. conv2d kw loop).
        // The missing write is a scalar accumulator (e.g. sum += …) that Stage 1
        // doesn't track.  Conservative assumption: RecMII = 1.
        bool readsOnly = !subtreeHasWrite(*dst);

        if (hasReduction) {
            dst->recMII       = 1;
            dst->recMIIReason = "accumulator dep: latency=1, distance=1";
        } else if (readsOnly) {
            dst->recMII       = 1;
            dst->recMIIReason = "reads-only loop \xe2\x86\x92 scalar accumulator (e.g. sum +=)";
        } else {
            dst->recMII       = 0;
            dst->recMIIReason = "no loop-carried dependences";
        }

        int achievedII  = std::max(dst->resMII, dst->recMII);
        dst->IIFeasible = (dst->requestedII >= achievedII);

        // ── Build dependence edges ─────────────────────────────────────────────

        // Loop-carried dep from accumulator pattern.
        if (hasReduction || readsOnly) {
            std::string acc_name = hasReduction ? "accumulator" : "scalar acc";
            DepEdge e;
            e.src         = acc_name;
            e.dst         = acc_name;
            e.type        = DepType::RAW;
            e.loopCarried = true;
            e.distance    = 1;
            dst->deps.push_back(e);
        }

        // Intra-iteration RAW: same array, one write, one or more reads.
        std::vector<const AffineAccess *> allAcc;
        collectAllAccesses(*dst, allAcc);
        for (const auto *wa : allAcc) {
            if (!wa->isWrite) continue;
            for (const auto *ra : allAcc) {
                if (ra->isWrite || ra->array != wa->array) continue;
                DepEdge e;
                e.src         = "READ "  + ra->array + "[" + ra->indexExpr + "]";
                e.dst         = "WRITE " + wa->array + "[" + wa->indexExpr + "]";
                e.type        = DepType::RAW;
                e.loopCarried = false;
                e.distance    = 0;
                dst->deps.push_back(e);
            }
        }
    }

    return dst;
}

void buildAffineContext(const HLSContext &ctx, AffineContext &affCtx) {
    for (const auto &fn : ctx.functions) {
        AffineFunction af;
        af.name   = fn.name;
        af.line   = fn.line;
        af.params = fn.params;
        for (const auto &loop : fn.loops)
            af.loops.push_back(buildAffineLoop(*loop, {}));
        affCtx.functions.push_back(std::move(af));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 3 — Pretty printer
// ═══════════════════════════════════════════════════════════════════════════════

static std::string pad(int d) { return std::string(d * 2, ' '); }

static const char *patternLabel(AccessPattern p) {
    switch (p) {
    case AccessPattern::Sequential:    return "SEQUENTIAL";
    case AccessPattern::Strided:       return "STRIDED";
    case AccessPattern::Reduction:     return "REDUCTION";
    case AccessPattern::SlidingWindow: return "SLIDING_WINDOW";
    default:                           return "UNKNOWN";
    }
}

static const char *depLabel(DepType t) {
    switch (t) {
    case DepType::RAW: return "RAW";
    case DepType::WAR: return "WAR";
    case DepType::WAW: return "WAW";
    }
    return "?";
}

static std::string pragmaTag(const HLSPragma &p) {
    if (p.kind == PragmaKind::Pipeline)
        return "[PIPELINE II=" +
               std::to_string(std::get<PipelinePragma>(p.data).II) + "]";
    if (p.kind == PragmaKind::Unroll) {
        int f = std::get<UnrollPragma>(p.data).factor;
        return f == 0 ? "[UNROLL full]"
                      : "[UNROLL factor=" + std::to_string(f) + "]";
    }
    return "";
}

static void dumpAffineLoop(const AffineLoop &loop, int depth) {
    // ── Loop header ───────────────────────────────────────────────────────────
    std::cout << pad(depth)
              << loop.id
              << "  line " << loop.line
              << "  for " << loop.var
              << " in [" << loop.lo << ", " << loop.hi << ")";
    for (const auto &p : loop.pragmas) {
        auto s = pragmaTag(p);
        if (!s.empty()) std::cout << "  " << s;
    }
    std::cout << "\n";

    // ── Classified accesses ───────────────────────────────────────────────────
    for (const auto &acc : loop.accesses) {
        std::cout << pad(depth + 1)
                  << (acc.isWrite ? "WRITE" : "READ ")
                  << "  " << acc.array << "[" << acc.indexExpr << "]"
                  << "   \xe2\x86\x92  " << patternLabel(acc.pattern);
        if (acc.pattern == AccessPattern::Strided)
            std::cout << "  stride=" << acc.stride;
        std::cout << "\n";
    }

    // ── II Analysis (pipelined loops only) ───────────────────────────────────
    if (loop.requestedII >= 0) {
        int aii = std::max(loop.resMII, loop.recMII);
        std::cout << "\n"
                  << pad(depth + 1) << "\xe2\x94\x80\xe2\x94\x80 II Analysis "
                  << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n";
        std::cout << pad(depth + 1)
                  << "ResMII = " << loop.resMII
                  << "  (1 MAC/cycle, 1 DSP48)\n";
        std::cout << pad(depth + 1)
                  << "RecMII = " << loop.recMII
                  << "  (" << loop.recMIIReason << ")\n";
        std::cout << pad(depth + 1)
                  << "Achievable II = max(" << loop.resMII << ", "
                  << loop.recMII << ") = " << aii << "   ";
        if (loop.IIFeasible)
            std::cout << "\xe2\x86\x92  requested II=" << loop.requestedII
                      << " \xe2\x9c\x93 feasible\n";
        else
            std::cout << "\xe2\x86\x92  requested II=" << loop.requestedII
                      << " \xe2\x9c\x97 NOT feasible (need II >= " << aii << ")\n";

        if (!loop.deps.empty()) {
            std::cout << "\n"
                      << pad(depth + 1)
                      << "\xe2\x94\x80\xe2\x94\x80 Dependences "
                      << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n";
            for (const auto &dep : loop.deps) {
                std::cout << pad(depth + 2)
                          << dep.src
                          << "  \xe2\x94\x80\xe2\x94\x80" << depLabel(dep.type);
                if (dep.loopCarried)
                    std::cout << " (loop-carried, dist=" << dep.distance << ")";
                std::cout << "\xe2\x94\x80\xe2\x94\x80\xe2\x96\xba  " << dep.dst << "\n";
            }
        }
        std::cout << "\n";
    }

    // ── Recurse ───────────────────────────────────────────────────────────────
    for (const auto &ch : loop.children)
        dumpAffineLoop(*ch, depth + 1);
}

void dumpAffineContext(const AffineContext &affCtx) {
    std::cout << "=== FluxHLS Stage 2 Output ===\n\n";
    for (const auto &fn : affCtx.functions) {
        std::cout << "Function: " << fn.name
                  << "  (line " << fn.line << ")\n";
        std::cout << "  Parameters:\n";
        for (const auto &p : fn.params) {
            std::cout << "    " << p.type << " " << p.name;
            for (const auto &pr : p.pragmas)
                if (pr.kind == PragmaKind::Interface)
                    std::cout << "  ["
                              << std::get<InterfacePragma>(pr.data).mode << "]";
            std::cout << "\n";
        }
        std::cout << "\n";
        for (const auto &loop : fn.loops)
            dumpAffineLoop(*loop, 1);
    }
}
