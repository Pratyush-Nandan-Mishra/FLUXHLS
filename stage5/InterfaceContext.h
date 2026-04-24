#pragma once
#include "stage4/BindingContext.h" // BindingContext is the sole input to Stage 5
#include <string>                  // std::string for names, expressions, descriptions
#include <vector>                  // std::vector for port lists and register maps

// ── Direction of an AXI4-Master port ─────────────────────────────────────────
// Inferred from Stage 4 ArrayBinding readPorts / writePorts counts.

enum class AxiDir {
    Read,      // array is only read  by the kernel (input from DDR)
    Write,     // array is only written by the kernel (output to DDR)
    ReadWrite, // array is both read and written (rare: in-place update)
    Unknown    // no ArrayBinding was visible — Stage 1 limitation (e.g. conv2d)
};

// ── One AXI4-Master port (one per m_axi function parameter) ──────────────────
// Generated for every array parameter with a #pragma HLS INTERFACE m_axi annotation.

struct AxiMasterPort {
    std::string name;           // matches HLSParam::name (e.g. "A", "in_signal")
    AxiDir      direction;      //** READ / WRITE / READWRITE / UNKNOWN
    int         dataWidthBits;  // AXI data bus width: 32 for float/int, 64 for double
    std::string numChannelsStr; //** "1" for sequential/reduction, "N" for cyclic-strided
    std::string burstExpr;      //** symbolic total-element count: "N", "N * N", "1024"
    std::string bindingDesc;    // from Stage 4 (e.g. "BRAM 1 bank", "ShiftRegister depth=TAPS")
};

// ── One AXI4-Lite register (control plane entry) ─────────────────────────────
// Four fixed entries for ap_ctrl, then one entry per scalar param and per m_axi base address.

struct AxiLiteReg {
    std::string name;         // register label (e.g. "ap_ctrl", "N", "A_base")
    int         offset;       // byte offset in the AXI4-Lite address space
    int         widthBits;    // 32-bit or 64-bit register width
    std::string kind;         // "handshake", "interrupt", "scalar", "array_ptr"
    std::string description;  // human-readable description for this register
};

// ── Full interface specification for one kernel function ─────────────────────

struct InterfaceSpec {
    std::string              funcName;         // function name
    int                      line;             // source line

    bool                     hasApCtrl;        // always true — Vitis HLS always generates ap_ctrl
    std::vector<AxiLiteReg>  axiLite;          //** ap_ctrl block + scalar regs + array base ptrs
    std::vector<AxiMasterPort> axiMaster;      //** one entry per m_axi parameter

    // Summary counts (AxiDir::Unknown ports not included in read/write totals)
    int totalReadPorts;    // number of READ  + READWRITE m_axi ports
    int totalWritePorts;   // number of WRITE + READWRITE m_axi ports
    int totalUnknownPorts; // number of ports with no visible binding
};

// ── Top-level Stage 5 IR ─────────────────────────────────────────────────────

class InterfaceContext {
public:
    std::vector<InterfaceSpec> specs; // one entry per HLS kernel function
};
