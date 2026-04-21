#include "Frontend.h"
#include <clang-c/Index.h>
#include <algorithm>
#include <climits>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 1 — Pragma parsing (pure text, no Clang)
// ═══════════════════════════════════════════════════════════════════════════════

static std::string trim(const std::string &s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string parseKV(const std::string &tok, const std::string &key) {
    std::string prefix = key + "=";
    return tok.rfind(prefix, 0) == 0 ? tok.substr(prefix.size()) : "";
}

std::vector<HLSPragma> parsePragmas(const std::string &filepath) {
    std::vector<HLSPragma> out;
    std::ifstream file(filepath);
    std::string   line;
    unsigned      lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;
        auto pos = line.find("#pragma HLS");
        if (pos == std::string::npos) continue;

        std::istringstream ss(trim(line.substr(pos + 11)));
        std::string keyword;
        ss >> keyword;
        std::vector<std::string> toks;
        for (std::string t; ss >> t;) toks.push_back(t);

        HLSPragma p;
        p.line = lineNum;

        if (keyword == "PIPELINE") {
            PipelinePragma pp;
            for (auto &t : toks) { auto v = parseKV(t, "II"); if (!v.empty()) pp.II = std::stoi(v); }
            p.kind = PragmaKind::Pipeline; p.data = pp;

        } else if (keyword == "UNROLL") {
            UnrollPragma up;
            for (auto &t : toks) { auto v = parseKV(t, "factor"); if (!v.empty()) up.factor = std::stoi(v); }
            p.kind = PragmaKind::Unroll; p.data = up;

        } else if (keyword == "ARRAY_PARTITION") {
            ArrayPartitionPragma ap;
            for (auto &t : toks) {
                auto v = parseKV(t, "variable"); if (!v.empty()) ap.var    = v;
                    v = parseKV(t, "type");      if (!v.empty()) ap.type   = v;
                    v = parseKV(t, "factor");    if (!v.empty()) ap.factor = std::stoi(v);
            }
            p.kind = PragmaKind::ArrayPartition; p.data = ap;

        } else if (keyword == "INTERFACE") {
            InterfacePragma ip;
            for (auto &t : toks) {
                if (t.find('=') == std::string::npos && ip.mode.empty()) { ip.mode = t; continue; }
                auto v = parseKV(t, "port"); if (!v.empty()) ip.port = v;
                    v = parseKV(t, "mode"); if (!v.empty()) ip.mode = v;
            }
            p.kind = PragmaKind::Interface; p.data = ip;

        } else {
            continue;
        }
        out.push_back(p);
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 2 — libclang AST walking
// ═══════════════════════════════════════════════════════════════════════════════

static std::string cxStr(CXString s) {
    std::string r = clang_getCString(s);
    clang_disposeString(s);
    return r;
}

static unsigned cursorLine(CXCursor c) {
    unsigned line = 0;
    clang_getSpellingLocation(clang_getCursorLocation(c), nullptr, &line, nullptr, nullptr);
    return line;
}

// cursorText — returns the source text of a cursor.
// Uses clang_getExpansionLocation (where the macro is USED) rather than
// clang_getCursorExtent (which can follow a #define back to its definition
// and return the entire rest of the file as the token stream).
static std::string cursorText(CXCursor c, CXTranslationUnit tu) {
    CXSourceRange ext = clang_getCursorExtent(c);

    CXFile startFile, endFile;
    unsigned startLine, startCol, endLine, endCol;
    clang_getExpansionLocation(clang_getRangeStart(ext),
                               &startFile, &startLine, &startCol, nullptr);
    clang_getExpansionLocation(clang_getRangeEnd(ext),
                               &endFile,   &endLine,   &endCol,   nullptr);

    if (!startFile) return "?";

    // If the expansion start and end land in different files (e.g. a function-
    // like macro whose body is in a header), or the range looks degenerate,
    // just grab the single token at the usage site.
    bool sameFile = clang_File_isEqual(startFile, endFile);
    bool saneRange = sameFile &&
                     (startLine < endLine ||
                      (startLine == endLine && startCol < endCol));

    CXSourceLocation tokStart =
        clang_getLocation(tu, startFile, startLine, startCol);

    if (!saneRange) {
        // Grab up to 64 chars worth of tokens from the usage point
        CXSourceLocation tokEnd =
            clang_getLocation(tu, startFile, startLine, startCol + 64);
        CXToken *toks; unsigned n;
        clang_tokenize(tu, clang_getRange(tokStart, tokEnd), &toks, &n);
        std::string r = (n > 0) ? cxStr(clang_getTokenSpelling(tu, toks[0])) : "?";
        clang_disposeTokens(tu, toks, n);
        return r;
    }

    CXSourceLocation tokEnd =
        clang_getLocation(tu, endFile, endLine, endCol);
    CXToken *toks; unsigned n;
    clang_tokenize(tu, clang_getRange(tokStart, tokEnd), &toks, &n);
    std::string text;
    for (unsigned i = 0; i < n; ++i) {
        if (i) text += ' ';
        text += cxStr(clang_getTokenSpelling(tu, toks[i]));
    }
    clang_disposeTokens(tu, toks, n);
    return text.empty() ? "?" : text;
}

static CXChildVisitResult collectDirect(CXCursor c, CXCursor, CXClientData d) {
    reinterpret_cast<std::vector<CXCursor> *>(d)->push_back(c);
    return CXChildVisit_Continue;
}

// Return the operator symbol between the two direct children of a BinaryOperator.
static std::string binaryOpToken(CXCursor binOp, CXTranslationUnit tu) {
    std::vector<CXCursor> ch;
    clang_visitChildren(binOp, collectDirect, &ch);
    if (ch.size() < 2) return "";

    CXSourceLocation lhsEnd   = clang_getRangeEnd  (clang_getCursorExtent(ch[0]));
    CXSourceLocation rhsStart = clang_getRangeStart(clang_getCursorExtent(ch[1]));
    CXSourceRange    opRange  = clang_getRange(lhsEnd, rhsStart);

    CXToken *toks; unsigned n;
    clang_tokenize(tu, opRange, &toks, &n);
    std::string op;
    for (unsigned i = 0; i < n; ++i) {
        std::string s = cxStr(clang_getTokenSpelling(tu, toks[i]));
        if (!s.empty()) { op = s; break; }
    }
    clang_disposeTokens(tu, toks, n);
    return op;
}

// ─── For-loop info extraction ─────────────────────────────────────────────────

static void extractForInfo(CXCursor forCursor, CXTranslationUnit tu,
                           std::string &var, std::string &lo, std::string &hi) {
    var = "?"; lo = "0"; hi = "?";
    std::vector<CXCursor> ch;
    clang_visitChildren(forCursor, collectDirect, &ch);

    for (auto &c : ch) {
        CXCursorKind k = clang_getCursorKind(c);
        if (k == CXCursor_DeclStmt && var == "?") {
            std::vector<CXCursor> dc;
            clang_visitChildren(c, collectDirect, &dc);
            for (auto &d : dc)
                if (clang_getCursorKind(d) == CXCursor_VarDecl) {
                    var = cxStr(clang_getCursorSpelling(d));
                    break;
                }
        } else if (k == CXCursor_BinaryOperator && hi == "?") {
            std::string text = cursorText(c, tu);
            if (text.find('<') != std::string::npos) {
                std::vector<CXCursor> bc;
                clang_visitChildren(c, collectDirect, &bc);
                if (bc.size() >= 2)
                    hi = cursorText(bc[1], tu);
            }
        } else if (k == CXCursor_CompoundStmt) {
            break;
        }
    }
}

// ─── Pragma post-pass: attach PIPELINE/UNROLL to the right loop ───────────────
//
//  Vitis HLS allows pragmas either BEFORE the loop header or at the TOP of the
//  loop body (before any nested loop).  We run this pass after the full loop
//  tree is built so we can see child line numbers.
//
//  A pragma at line P belongs to loop L when:
//    (a) parentLine < P < L.line          — just before the loop header, OR
//    (b) L.line < P < firstChildLoop.line — at the top of the body
//
//  'claimed' prevents the same pragma from being attached to both a parent and
//  a child loop (e.g. a pragma inside for-j's body must not also appear on for-k).

static void matchPragmasToLoop(HLSLoop &loop, unsigned parentLine,
                                const std::vector<HLSPragma> &all,
                                std::unordered_set<unsigned>  &claimed) {
    unsigned firstChild = (unsigned)INT_MAX;
    for (const auto &c : loop.children)
        firstChild = std::min(firstChild, (unsigned)c->line);

    for (const auto &p : all) {
        if (p.kind != PragmaKind::Pipeline && p.kind != PragmaKind::Unroll)
            continue;
        if (claimed.count(p.line)) continue;          // already owned by an ancestor
        bool beforeHeader = (p.line > parentLine         && p.line < (unsigned)loop.line);
        bool topOfBody    = (p.line > (unsigned)loop.line && p.line < firstChild);
        if (beforeHeader || topOfBody) {
            loop.pragmas.push_back(p);
            claimed.insert(p.line);
        }
    }

    for (auto &child : loop.children)
        matchPragmasToLoop(*child, (unsigned)loop.line, all, claimed);
}

static void matchAllPragmas(HLSContext &ctx, const std::vector<HLSPragma> &all) {
    std::unordered_set<unsigned> claimed;
    for (auto &fn : ctx.functions)
        for (auto &loop : fn.loops)
            matchPragmasToLoop(*loop, (unsigned)fn.line, all, claimed);
}

// ─── Visitor state ────────────────────────────────────────────────────────────

struct VisitorCtx {
    const std::vector<HLSPragma> *pragmas;
    HLSContext                   *ctx;
    HLSFunction                  *func           = nullptr;
    std::vector<HLSLoop *>        loopStk;
    int                           loopSeq        = 0;
    bool                          nextWriteFlag  = false; // set while visiting LHS
    CXTranslationUnit             tu;
};

static std::string makeLoopId(const std::vector<HLSLoop *> &stk, int seq) {
    if (stk.empty()) return "L" + std::to_string(seq);
    return stk.back()->id + ".L" + std::to_string(seq);
}

// ─── Body visitor ─────────────────────────────────────────────────────────────

static CXChildVisitResult bodyVisitor(CXCursor c, CXCursor /*parent*/,
                                      CXClientData data);

// Call bodyVisitor on 'c' directly, then recurse into its children if it
// returned CXChildVisit_Recurse.  This lets us visit a specific cursor rather
// than only its children (which is all clang_visitChildren gives us).
static void visitSubtree(CXCursor c, CXClientData data) {
    CXChildVisitResult r = bodyVisitor(c, c, data);
    if (r == CXChildVisit_Recurse)
        clang_visitChildren(c, bodyVisitor, data);
}

static CXChildVisitResult topVisitor(CXCursor c, CXCursor,
                                     CXClientData data) {
    if (clang_getCursorKind(c) != CXCursor_FunctionDecl) return CXChildVisit_Continue;
    if (!clang_isCursorDefinition(c))                    return CXChildVisit_Continue;

    auto *ctx = reinterpret_cast<VisitorCtx *>(data);

    HLSFunction fn;
    fn.name = cxStr(clang_getCursorSpelling(c));
    fn.line = (int)cursorLine(c);

    int nArgs = clang_Cursor_getNumArguments(c);
    for (int i = 0; i < nArgs; ++i) {
        CXCursor param = clang_Cursor_getArgument(c, i);
        HLSParam p;
        p.name   = cxStr(clang_getCursorSpelling(param));
        CXType t = clang_getCursorType(param);
        p.type   = cxStr(clang_getTypeSpelling(t));
        p.isArray = (t.kind == CXType_Pointer      ||
                     t.kind == CXType_ConstantArray ||
                     t.kind == CXType_IncompleteArray);

        for (const auto &pr : *ctx->pragmas) {
            if (pr.kind != PragmaKind::Interface) continue;
            if (std::get<InterfacePragma>(pr.data).port == p.name)
                p.pragmas.push_back(pr);
        }
        fn.params.push_back(std::move(p));
    }

    ctx->ctx->functions.push_back(std::move(fn));
    ctx->func    = &ctx->ctx->functions.back();
    ctx->loopStk.clear();
    ctx->loopSeq = 0;

    clang_visitChildren(c, bodyVisitor, data);

    ctx->func = nullptr;
    return CXChildVisit_Continue;
}

static CXChildVisitResult bodyVisitor(CXCursor c, CXCursor /*parent*/,
                                      CXClientData data) {
    auto *ctx = reinterpret_cast<VisitorCtx *>(data);
    CXCursorKind kind = clang_getCursorKind(c);

    // ── For loops ──────────────────────────────────────────────────────────────
    if (kind == CXCursor_ForStmt) {
        auto loop  = std::make_unique<HLSLoop>();
        loop->line = (int)cursorLine(c);
        loop->id   = makeLoopId(ctx->loopStk, ctx->loopSeq++);
        extractForInfo(c, ctx->tu, loop->var, loop->lo, loop->hi);
        // Pragmas are attached in the post-pass (matchAllPragmas), not here.

        HLSLoop *raw = loop.get();
        if (ctx->loopStk.empty()) ctx->func->loops.push_back(std::move(loop));
        else                      ctx->loopStk.back()->children.push_back(std::move(loop));
        ctx->loopStk.push_back(raw);

        clang_visitChildren(c, bodyVisitor, data);

        ctx->loopStk.pop_back();
        return CXChildVisit_Continue;
    }

    // ── Assignment operators: track which side is the LHS (write) ─────────────
    //
    //  We handle BinaryOperator(=) and CompoundAssignOperator(+=,-=,...) by
    //  manually recursing, setting nextWriteFlag=true only for the LHS child.
    //  This avoids false positives from non-assignment binary ops (*, +, etc.).
    if ((kind == CXCursor_BinaryOperator ||
         kind == CXCursor_CompoundAssignOperator) && !ctx->loopStk.empty()) {

        bool isAssign = (kind == CXCursor_CompoundAssignOperator) ||
                        (binaryOpToken(c, ctx->tu) == "=");

        std::vector<CXCursor> ch;
        clang_visitChildren(c, collectDirect, &ch);

        if (isAssign && ch.size() >= 2) {
            ctx->nextWriteFlag = true;
            visitSubtree(ch[0], data);   // LHS — visit ch[0] itself, not just its children
            ctx->nextWriteFlag = false;
            visitSubtree(ch[1], data);   // RHS
            return CXChildVisit_Continue;
        }
        return CXChildVisit_Recurse;
    }

    // ── Array subscript expressions ────────────────────────────────────────────
    if (kind == CXCursor_ArraySubscriptExpr && !ctx->loopStk.empty()) {
        MemAccess acc;
        acc.isWrite = ctx->nextWriteFlag;

        std::vector<CXCursor> ch;
        clang_visitChildren(c, collectDirect, &ch);
        if (!ch.empty()) {
            auto findRef = [](CXCursor cur) -> std::string {
                if (clang_getCursorKind(cur) == CXCursor_DeclRefExpr)
                    return cxStr(clang_getCursorSpelling(cur));
                std::vector<CXCursor> inner;
                clang_visitChildren(cur, collectDirect, &inner);
                for (auto &ic : inner)
                    if (clang_getCursorKind(ic) == CXCursor_DeclRefExpr)
                        return cxStr(clang_getCursorSpelling(ic));
                return "";
            };
            acc.array = findRef(ch[0]);
            if (ch.size() >= 2)
                acc.index = cursorText(ch[1], ctx->tu);
        }

        if (!acc.array.empty())
            ctx->loopStk.back()->accesses.push_back(acc);
        return CXChildVisit_Continue;
    }

    return CXChildVisit_Recurse;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Public entry point
// ═══════════════════════════════════════════════════════════════════════════════

void buildHLSContext(const std::string              &filepath,
                     const std::vector<HLSPragma>   &pragmas,
                     HLSContext                     &ctx,
                     const std::vector<std::string> &compilerArgs) {
    CXIndex idx = clang_createIndex(0, 1);

    std::vector<const char *> args = {"-std=c++17", "-w"};
    for (const auto &a : compilerArgs) args.push_back(a.c_str());

    CXTranslationUnit tu = clang_parseTranslationUnit(
        idx, filepath.c_str(),
        args.data(), (int)args.size(),
        nullptr, 0, CXTranslationUnit_None);

    if (!tu) {
        std::cerr << "[FluxHLS] Failed to parse: " << filepath << "\n";
        clang_disposeIndex(idx);
        return;
    }

    VisitorCtx vctx;
    vctx.pragmas = &pragmas;
    vctx.ctx     = &ctx;
    vctx.tu      = tu;

    clang_visitChildren(clang_getTranslationUnitCursor(tu), topVisitor, &vctx);

    // Post-pass: attach PIPELINE/UNROLL pragmas to the correct loop.
    matchAllPragmas(ctx, pragmas);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(idx);
}
