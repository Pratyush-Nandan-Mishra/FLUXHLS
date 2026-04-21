#include "Dumper.h"
#include <iostream>
#include <string>

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string pad(int depth) { return std::string(depth * 2, ' '); }

static std::string pragmaTag(const HLSPragma &p) {
    switch (p.kind) {
    case PragmaKind::Pipeline: {
        auto &pp = std::get<PipelinePragma>(p.data);
        return "[PIPELINE II=" + std::to_string(pp.II) + "]";
    }
    case PragmaKind::Unroll: {
        auto &up = std::get<UnrollPragma>(p.data);
        return up.factor == 0 ? "[UNROLL full]"
                              : "[UNROLL factor=" + std::to_string(up.factor) + "]";
    }
    case PragmaKind::ArrayPartition: {
        auto &ap = std::get<ArrayPartitionPragma>(p.data);
        return "[ARRAY_PARTITION var=" + ap.var +
               " type=" + ap.type +
               " factor=" + std::to_string(ap.factor) + "]";
    }
    case PragmaKind::Interface: {
        auto &ip = std::get<InterfacePragma>(p.data);
        return "[" + ip.mode + "]";
    }
    }
    return "";
}

// ─── recursive loop dump ──────────────────────────────────────────────────────

static void dumpLoop(IDumper &d, const HLSLoop &loop, int depth) {
    d.onLoop(loop, depth);
    for (const auto &child : loop.children)
        dumpLoop(d, *child, depth + 1);
}

// ─── HLSContext::dump  (defined here so Dumper.cpp is the only TU that needs
//     both HLSContext and IDumper fully defined) ────────────────────────────────

void HLSContext::dump(IDumper &d) const {
    for (int i = 0; i < (int)functions.size(); ++i)
        d.onFunction(functions[i], i);
}

// ─── PrettyDumper ─────────────────────────────────────────────────────────────

void PrettyDumper::onFunction(const HLSFunction &fn, int idx) {
    if (idx == 0)
        std::cout << "=== FluxHLS Stage 1 Output ===\n\n";

    std::cout << "Function: " << fn.name << "  (line " << fn.line << ")\n";
    std::cout << "  Parameters:\n";
    for (const auto &p : fn.params) {
        std::cout << "    " << p.type << " " << p.name;
        for (const auto &pr : p.pragmas)
            std::cout << "  " << pragmaTag(pr);
        std::cout << "\n";
    }
    std::cout << "\n";
    for (const auto &loop : fn.loops)
        dumpLoop(*this, *loop, 1);
    std::cout << "\n";
}

void PrettyDumper::onLoop(const HLSLoop &loop, int depth) {
    std::cout << pad(depth)
              << loop.id
              << "  line " << loop.line
              << "  for " << loop.var
              << " in [" << loop.lo << ", " << loop.hi << ")";
    for (const auto &p : loop.pragmas)
        std::cout << "  " << pragmaTag(p);
    std::cout << "\n";

    for (const auto &acc : loop.accesses) {
        std::cout << pad(depth + 1)
                  << (acc.isWrite ? "WRITE " : "READ  ")
                  << acc.array << "[" << acc.index << "]\n";
    }
}
