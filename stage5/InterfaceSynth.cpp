#include "InterfaceSynth.h" // public API: buildInterfaces(), dumpInterfaces()
#include <algorithm>        // std::all_of
#include <iomanip>          // std::setw, std::left, std::right, std::hex
#include <iostream>         // std::cout for the pretty-printer
#include <sstream>          // std::ostringstream for hex offset formatting
#include <string>           // std::string, std::to_string
#include <vector>           // std::vector

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 1 — Utility helpers
// ═══════════════════════════════════════════════════════════════════════════════

// Infer the AXI data bus width from the C parameter type string.
// float / int / unsigned → 32 bits.  double / int64_t / long → 64 bits.
static int inferDataWidth(const std::string &type) {
    if (type.find("double") != std::string::npos) return 64;  // double precision
    if (type.find("long")   != std::string::npos) return 64;  // 64-bit integer
    return 32;                                                  // float, int, etc. → 32-bit
}

// Return a short string label for ResourceKind — used in the binding description column.
static const char *resourceKindStr(ResourceKind r) {
    switch (r) {
    case ResourceKind::BRAM:          return "BRAM";
    case ResourceKind::ShiftRegister: return "ShiftRegister";
    case ResourceKind::Register:      return "Register";
    case ResourceKind::DSP48:         return "DSP48";
    }
    return "?";
}

// Compute the burst-length expression for one m_axi parameter.
//
// Strategy (string concatenation as requested):
//   1. If the parameter type has fixed numeric dimensions (e.g. "float[32][32]"),
//      multiply them together to get a concrete count (e.g. 1024).
//   2. If dimensions are present but symbolic, join them with " * " (e.g. "IH * IW").
//   3. If the type is a pointer (no '[' in type), fall back to the product of
//      all loop `hi` strings on the path from the root to the pipelined loop,
//      joined with " * " (e.g. "N", "N * N", "length").
static std::string computeBurstExpr(const HLSParam              &param,   // the m_axi parameter
                                    const std::vector<std::string> &hiPath) // root-to-pipeline hi values
{
    const std::string &type = param.type;  // e.g. "float[32][32]" or "float *"

    // ── Strategy 1 & 2: parse array dimensions from the type string ───────────
    std::vector<std::string> dims;  // collected dimension strings
    bool     allNumeric = true;     // true when every dimension is a pure integer
    long long total     = 1;        // running product of numeric dimensions
    size_t    i         = 0;        // scan position in the type string
    while ((i = type.find('[', i)) != std::string::npos) { // locate each '[' in the type
        size_t j = type.find(']', i);                      // find matching ']'
        if (j == std::string::npos) break;                  // malformed type — stop scanning
        std::string dim = type.substr(i + 1, j - i - 1);   // text between '[' and ']'
        dims.push_back(dim);                                // record this dimension
        if (!dim.empty() &&
            std::all_of(dim.begin(), dim.end(), ::isdigit)) // is it a pure number?
            total *= std::stoll(dim);                        // multiply into running product
        else
            allNumeric = false;                              // symbolic dim — can't compute total
        i = j + 1;                                          // advance past ']'
    }

    if (!dims.empty()) {                       // array type found (not a pointer)
        if (allNumeric)                        // all dimensions are constant integers?
            return std::to_string(total);      // return the product as a plain number
        std::string expr;                      // build symbolic product string
        for (size_t k = 0; k < dims.size(); k++) {
            if (k > 0) expr += " * ";         // separator between dimensions
            expr += dims[k];                   // append this dimension symbol
        }
        return expr;  // e.g. "IH * IW" when dimensions are macro symbols
    }

    // ── Strategy 3: pointer type — use loop hi string product ────────────────
    if (hiPath.empty()) return "?";            // no loop path found — placeholder
    std::string expr;
    for (size_t k = 0; k < hiPath.size(); k++) {
        if (k > 0) expr += " * ";             // join hi values with multiplication
        expr += hiPath[k];                     // append this loop's upper bound
    }
    return expr;  // e.g. "N", "N * N", "length"
}

// Depth-first search for the pipelined loop in a BoundLoop subtree.
// Collects the `hi` string of every loop on the path from the root to the
// pipelined loop (inclusive).  Returns true when the pipeline is found.
static bool findPipelinePath(const BoundLoop         &loop, // current node
                              std::vector<std::string> &path) // output: hi values on path
{
    path.push_back(loop.hi);              // tentatively add this loop's upper bound
    if (loop.requestedII >= 0) return true; //** this IS the pipelined loop — stop
    for (const auto &child : loop.children) {         // recurse into children
        if (findPipelinePath(*child, path)) return true; // pipeline found in subtree
    }
    path.pop_back();   // backtrack: no pipeline in this subtree — remove this hi
    return false;      // signal that pipeline was not found here
}

// Recursively search all BoundLoop subtrees in a BoundFunction for the pipelined loop.
// Returns the collected hi-path, or an empty vector if no pipeline exists.
static std::vector<std::string> gatherPipelinePath(const BoundFunction &fn) {
    std::vector<std::string> path;        // will be filled by findPipelinePath
    for (const auto &loop : fn.loops) {   // search each top-level loop tree
        if (findPipelinePath(*loop, path)) break; // stop at first pipeline found
    }
    return path;  // empty if no pipelined loop exists in this function
}

// Search all BoundLoop subtrees for the ArrayBinding of a named array.
// Returns nullptr if the array has no visible binding (e.g. conv2d's kw-loop).
static const ArrayBinding *findArrayBinding(const std::string &array,  // target array name
                                            const BoundLoop   &loop)   // root of search
{
    for (const auto &ab : loop.arrayBindings)   // check this loop's bindings first
        if (ab.array == array) return &ab;       // found — return pointer
    for (const auto &child : loop.children) {   // recurse into children
        const ArrayBinding *r = findArrayBinding(array, *child);
        if (r) return r;                          // propagate found result
    }
    return nullptr;  // not found in this subtree
}

// Parse the leading token from ArrayBinding.configStr to extract the channel count.
// "1 bank"          → "1"    (single-bank BRAM)
// "N banks cyclic"  → "N"    (cyclic-partitioned BRAM — N parallel AXI channels)
// "depth=TAPS"      → "1"    (ShiftRegister is internal; only 1 AXI stream needed)
// "1 regs"          → "1"    (Register — internal; still 1 AXI port for loading)
static std::string parseNumChannels(const std::string &configStr) {
    size_t banksPos = configStr.find(" banks");   // look for " banks" keyword
    if (banksPos != std::string::npos)            // found cyclic BRAM partition
        return configStr.substr(0, banksPos);     // return the token before " banks" (e.g. "N")
    return "1";  // all other resource types use a single AXI channel
}

// Format an integer as a hexadecimal offset string (e.g. 16 → "0x10").
static std::string hexOffset(int offset) {
    std::ostringstream ss;                 // string stream for hex formatting
    ss << "0x" << std::uppercase          // "0x" prefix with uppercase hex digits
       << std::hex << offset;             // convert integer to hex
    return ss.str();                      // return e.g. "0x10", "0x1C"
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 2 — Build InterfaceSpec for one function
// ═══════════════════════════════════════════════════════════════════════════════

static InterfaceSpec buildSpec(const BoundFunction &fn) {  //** core per-function builder
    InterfaceSpec spec;             // result to fill
    spec.funcName = fn.name;        // copy function name
    spec.line     = fn.line;        // copy source line
    spec.hasApCtrl = true;          // Vitis HLS always generates ap_ctrl (even if not explicit)
    spec.totalReadPorts    = 0;     // reset port counts
    spec.totalWritePorts   = 0;
    spec.totalUnknownPorts = 0;

    // ── Find the pipeline path (hi values from root to pipelined loop) ────────
    std::vector<std::string> hiPath = gatherPipelinePath(fn); //** used for pointer burst exprs

    // ── AXI4-Lite register map ─────────────────────────────────────────────────
    // Fixed ap_ctrl block at offsets 0x00-0x0C (Vitis HLS standard layout).
    spec.axiLite.push_back({"ap_ctrl", 0x00, 32, "handshake",
                             "[0]=start  [1]=done  [2]=idle  [3]=ready"});
    spec.axiLite.push_back({"ap_gier",  0x04, 32, "interrupt", "global interrupt enable"});
    spec.axiLite.push_back({"ap_ier",   0x08, 32, "interrupt", "interrupt enable register"});
    spec.axiLite.push_back({"ap_isr",   0x0C, 32, "interrupt", "interrupt status register"});

    int nextOffset = 0x10;  // next available byte offset after the fixed ap_ctrl block

    // Scalar s_axilite parameters: assign 32-bit (or 64-bit) register slots.
    for (const auto &p : fn.params) {        // scan all function parameters
        bool isMaster = false, isLite = false;
        for (const auto &pr : p.pragmas) {   // check each pragma attached to this param
            if (pr.kind != PragmaKind::Interface) continue; // skip non-interface pragmas
            const auto &ip = std::get<InterfacePragma>(pr.data);
            if (ip.mode == "m_axi")     isMaster = true; // array with AXI4-Master interface
            if (ip.mode == "s_axilite") isLite   = true; // scalar with AXI4-Lite interface
        }
        if (!isLite || isMaster) continue;  // skip: not a scalar s_axilite param

        int width = inferDataWidth(p.type);  // 32 for float/int, 64 for double
        AxiLiteReg reg;
        reg.name        = p.name;            // register named after the parameter
        reg.offset      = nextOffset;        // assign next available offset
        reg.widthBits   = width;             // data width in bits
        reg.kind        = "scalar";          // this is a scalar configuration register
        reg.description = p.type + " " + p.name;  // human-readable type + name
        spec.axiLite.push_back(reg);         // append to the register map
        nextOffset += 0x08;                  // advance by 8 bytes (64-bit aligned slots)
    }

    // Array base-address registers: each m_axi array's DDR pointer goes into a 64-bit slot.
    for (const auto &p : fn.params) {        // scan params again for m_axi arrays
        bool isMaster = false;
        for (const auto &pr : p.pragmas) {
            if (pr.kind != PragmaKind::Interface) continue;
            if (std::get<InterfacePragma>(pr.data).mode == "m_axi") isMaster = true;
        }
        if (!isMaster) continue;  // skip non-m_axi params

        AxiLiteReg reg;
        reg.name        = p.name + "_base"; // e.g. "A_base", "in_signal_base"
        reg.offset      = nextOffset;        // assign next available offset
        reg.widthBits   = 64;               //** 64-bit DDR pointer (AXI address space)
        reg.kind        = "array_ptr";       // this holds the DDR base address
        reg.description = "DDR base address of " + p.name + "  [m_axi]";
        spec.axiLite.push_back(reg);
        nextOffset += 0x08;                  // 8-byte slot for 64-bit pointer
    }

    // ── AXI4-Master ports ──────────────────────────────────────────────────────
    for (const auto &p : fn.params) {        // scan params for m_axi arrays
        bool isMaster = false;
        for (const auto &pr : p.pragmas) {
            if (pr.kind != PragmaKind::Interface) continue;
            if (std::get<InterfacePragma>(pr.data).mode == "m_axi") isMaster = true;
        }
        if (!isMaster) continue;  // skip non-m_axi params

        AxiMasterPort port;
        port.name          = p.name;                  // param name → port name
        port.dataWidthBits = inferDataWidth(p.type);  // AXI data width

        // Look up the Stage 4 ArrayBinding for this param across all bound loops.
        const ArrayBinding *ab = nullptr;
        for (const auto &loop : fn.loops) {           // search all top-level loop trees
            ab = findArrayBinding(p.name, *loop);
            if (ab) break;                             // stop at first match
        }

        if (ab) {
            // ── Direction: inferred from read/write port counts ───────────────
            if      (ab->readPorts > 0 && ab->writePorts == 0) port.direction = AxiDir::Read;
            else if (ab->readPorts == 0 && ab->writePorts > 0) port.direction = AxiDir::Write;
            else                                                port.direction = AxiDir::ReadWrite;

            // ── Channel count: 1 for single-bank, N for cyclic partition ─────
            port.numChannelsStr = parseNumChannels(ab->configStr); //** "1" or "N"

            // ── Binding description: resource type + config ───────────────────
            port.bindingDesc = std::string(resourceKindStr(ab->resource))
                               + " " + ab->configStr;  // e.g. "BRAM 1 bank"
        } else {
            // No ArrayBinding visible — Stage 1 could not capture the accesses.
            port.direction      = AxiDir::Unknown;                           // direction unknown
            port.numChannelsStr = "1";                                        // assume 1 channel
            port.bindingDesc    = "(no binding — Stage 1 cannot see accesses)"; // explain why
        }

        // ── Burst expression: total elements transferred per kernel invocation ─
        port.burstExpr = computeBurstExpr(p, hiPath); //** string concatenation of hi values

        // ── Accumulate summary port counts ────────────────────────────────────
        if (port.direction == AxiDir::Read)      spec.totalReadPorts++;
        if (port.direction == AxiDir::Write)     spec.totalWritePorts++;
        if (port.direction == AxiDir::ReadWrite) { spec.totalReadPorts++; spec.totalWritePorts++; }
        if (port.direction == AxiDir::Unknown)   spec.totalUnknownPorts++;

        spec.axiMaster.push_back(port);  // append completed port
    }

    return spec;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 3 — Top-level build entry
// ═══════════════════════════════════════════════════════════════════════════════

void buildInterfaces(const BindingContext &bindCtx,  //** Stage 4 IR input
                     InterfaceContext     &ifCtx)     //** Stage 5 IR output
{
    for (const auto &fn : bindCtx.functions)  // process every bound function
        ifCtx.specs.push_back(buildSpec(fn));  // build and store one InterfaceSpec per function
}

// ═══════════════════════════════════════════════════════════════════════════════
//  PART 4 — Pretty printer
// ═══════════════════════════════════════════════════════════════════════════════

// Unicode horizontal rule (UTF-8 U+2500) — matches Stage 3 and Stage 4 style.
static const char *HR = "\xe2\x94\x80";

// Build a string of N horizontal-rule characters.
static std::string hrs(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += HR;  // concatenate N ─ characters
    return s;
}

// Convert AxiDir to a fixed-width display label.
static const char *dirLabel(AxiDir d) {
    switch (d) {
    case AxiDir::Read:      return "READ     ";  // kernel reads from DDR
    case AxiDir::Write:     return "WRITE    ";  // kernel writes to DDR
    case AxiDir::ReadWrite: return "RDWR     ";  // bidirectional
    case AxiDir::Unknown:   return "?        ";  // no binding visible
    }
    return "?        ";
}

void dumpInterfaces(const InterfaceContext &ifCtx) {          // top-level printer
    std::cout << "=== FluxHLS Stage 5 Output ===\n\n";        // stage banner

    for (const auto &spec : ifCtx.specs) {                    // one block per function
        std::cout << "Function: " << spec.funcName
                  << "  (line " << spec.line << ")\n";

        // ── AXI4-Lite Register Map ─────────────────────────────────────────────
        std::cout << "\n  " << HR << HR << " AXI4-Lite Register Map " << hrs(42) << "\n";

        // Column header
        std::cout << "  " << "  "
                  << std::left << std::setw(8)  << "Offset"
                  << std::left << std::setw(16) << "Name"
                  << std::left << std::setw(7)  << "Width"
                  << std::left << std::setw(12) << "Kind"
                  << "Description\n";
        std::cout << "  " << std::string(76, '-') << "\n";

        for (const auto &r : spec.axiLite) {
            std::string offStr  = hexOffset(r.offset);          // format as "0x10"
            std::string wStr    = std::to_string(r.widthBits) + "-bit";  // "32-bit" or "64-bit"
            std::cout << "  " << "  "
                      << std::left << std::setw(8)  << offStr   // offset column
                      << std::left << std::setw(16) << r.name   // register name
                      << std::left << std::setw(7)  << wStr     // data width
                      << std::left << std::setw(12) << r.kind   // register kind
                      << r.description << "\n";                  // description
        }

        // ── AXI4-Master Ports ──────────────────────────────────────────────────
        std::cout << "\n  " << HR << HR << " AXI4-Master Ports " << hrs(47) << "\n";

        // Column header
        std::cout << "  " << "  "
                  << std::left << std::setw(16) << "Port"
                  << std::left << std::setw(10) << "Dir"
                  << std::left << std::setw(9)  << "Width"
                  << std::left << std::setw(11) << "Channels"
                  << std::left << std::setw(20) << "Burst"
                  << "Binding\n";
        std::cout << "  " << std::string(76, '-') << "\n";

        for (const auto &port : spec.axiMaster) {
            std::string widthStr = std::to_string(port.dataWidthBits) + "-bit"; // "32-bit"
            std::string burst    = port.burstExpr + " elems";  // append unit suffix
            if (burst.size() > 18) burst = burst.substr(0, 15) + "..."; // truncate if long
            std::string binding  = port.bindingDesc;
            // binding is the last column — no truncation needed
            std::cout << "  " << "  "
                      << std::left << std::setw(16) << port.name           // port name
                      << std::left << std::setw(10) << dirLabel(port.direction) // direction
                      << std::left << std::setw(9)  << widthStr            // data width
                      << std::left << std::setw(11) << port.numChannelsStr // channel count
                      << std::left << std::setw(20) << burst               // burst expression
                      << binding << "\n";                                   // binding source
        }

        // ── Summary line ───────────────────────────────────────────────────────
        std::cout << "\n  AXI summary: "
                  << spec.totalReadPorts    << " read port(s)  "
                  << spec.totalWritePorts   << " write port(s)";
        if (spec.totalUnknownPorts > 0)
            std::cout << "  " << spec.totalUnknownPorts
                      << " unknown (no binding visible)";
        std::cout << "\n\n";
    }
}
