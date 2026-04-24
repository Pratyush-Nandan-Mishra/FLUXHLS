#include "Dumper.h"    // IDumper, PrettyDumper, HLSContext, HLSLoop, HLSPragma
#include <iostream>    // std::cout
#include <string>      // std::string, std::to_string

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string pad(int depth) { return std::string(depth * 2, ' '); }  // produce 2*depth spaces for indentation

static std::string pragmaTag(const HLSPragma &p) {   // convert a pragma to its printable display string
    switch (p.kind) {
    case PragmaKind::Pipeline: {                      // PIPELINE pragma case
        auto &pp = std::get<PipelinePragma>(p.data);  // extract the PipelinePragma from the variant
        return "[PIPELINE II=" + std::to_string(pp.II) + "]";  // format as [PIPELINE II=n]
    }
    case PragmaKind::Unroll: {                        // UNROLL pragma case
        auto &up = std::get<UnrollPragma>(p.data);    // extract the UnrollPragma from the variant
        return up.factor == 0 ? "[UNROLL full]"       // factor 0 means fully unroll all iterations
                              : "[UNROLL factor=" + std::to_string(up.factor) + "]";  // partial unroll
    }
    case PragmaKind::ArrayPartition: {                // ARRAY_PARTITION pragma case
        auto &ap = std::get<ArrayPartitionPragma>(p.data);  // extract the ArrayPartitionPragma from the variant
        return "[ARRAY_PARTITION var=" + ap.var +     // include target variable name
               " type=" + ap.type +                   // partition type: cyclic, block, or complete
               " factor=" + std::to_string(ap.factor) + "]";  // number of banks/partitions
    }
    case PragmaKind::Interface: {                     // INTERFACE pragma case
        auto &ip = std::get<InterfacePragma>(p.data); // extract the InterfacePragma from the variant
        return "[" + ip.mode + "]";                   // format as [m_axi] or [s_axilite]
    }
    }
    return "";  // unknown pragma kind — should never reach here
}

// ─── recursive loop dump ──────────────────────────────────────────────────────

static void dumpLoop(IDumper &d, const HLSLoop &loop, int depth) {   // recursively walk and print the loop tree
    d.onLoop(loop, depth);                            // print this loop node at its current depth
    for (const auto &child : loop.children)           // iterate over nested child loops
        dumpLoop(d, *child, depth + 1);               // recurse with depth incremented for each nesting level
}

// ─── HLSContext::dump  (defined here so Dumper.cpp is the only TU that needs
//     both HLSContext and IDumper fully defined) ────────────────────────────────

void HLSContext::dump(IDumper &d) const {             // print the entire IR through the injected dumper
    for (int i = 0; i < (int)functions.size(); ++i)  // iterate over every extracted function
        d.onFunction(functions[i], i);                // pass the index so the first call can print the header
}

// ─── PrettyDumper ─────────────────────────────────────────────────────────────

void PrettyDumper::onFunction(const HLSFunction &fn, int idx) {   // called once for each function in the IR
    if (idx == 0)                                                   // only print the stage banner before the first function
        std::cout << "=== FluxHLS Stage 1 Output ===\n\n";         // stage header line

    std::cout << "Function: " << fn.name << "  (line " << fn.line << ")\n";  // function name and source line
    std::cout << "  Parameters:\n";                                // parameter section label
    for (const auto &p : fn.params) {                              // iterate over every function parameter
        std::cout << "    " << p.type << " " << p.name;           // print type and parameter name
        for (const auto &pr : p.pragmas)                           // print any interface pragmas attached to this param
            std::cout << "  " << pragmaTag(pr);                    // e.g. "  [m_axi]"
        std::cout << "\n";                                         // newline after each parameter
    }
    std::cout << "\n";                                             // blank line separates parameters from loops
    for (const auto &loop : fn.loops)                              // iterate over top-level loops in this function
        dumpLoop(*this, *loop, 1);                                 // start at depth 1 (one level inside the function)
    std::cout << "\n";                                             // blank line after the function block
}

void PrettyDumper::onLoop(const HLSLoop &loop, int depth) {       // called once for each loop node during tree walk
    std::cout << pad(depth)                                        // indent to the current nesting depth
              << loop.id                                           // loop identifier e.g. "L0", "L0.L1"
              << "  line " << loop.line                            // source line of the for-statement
              << "  for " << loop.var                              // induction variable name
              << " in [" << loop.lo << ", " << loop.hi << ")";    // loop trip bounds
    for (const auto &p : loop.pragmas)                             // print any pragmas attached to this loop
        std::cout << "  " << pragmaTag(p);                        // e.g. "  [PIPELINE II=1]"
    std::cout << "\n";                                             // end the loop header line

    for (const auto &acc : loop.accesses) {                        // print each array access recorded inside this loop
        std::cout << pad(depth + 1)                                // indent one level deeper than the loop header
                  << (acc.isWrite ? "WRITE " : "READ  ")           // access direction label
                  << acc.array << "[" << acc.index << "]\n";       // array name and raw index expression
    }
}
