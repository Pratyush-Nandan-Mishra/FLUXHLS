#include "stage6/RTLEmitter.h"       // buildRTL() and dumpRTL() declarations
#include "stage6/RTLContext.h"        // RTLModule, RTLContext
#include <algorithm>                  // std::find, std::all_of
#include <cctype>                     // std::isdigit
#include <filesystem>                 // std::filesystem::create_directories
#include <fstream>                    // std::ofstream for writing .sv files
#include <iomanip>                    // std::setw, std::setfill for hex formatting
#include <iostream>                   // std::cout for stdout summary
#include <sstream>                    // std::ostringstream for building SV text
#include <string>                     // std::string
#include <vector>                     // std::vector

namespace fs = std::filesystem;       // alias for brevity

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool isNumeric(const std::string &s) {                          // true if s contains only digit characters
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);  // empty string → false
}

static void collectSymbolicParams(const BoundLoop &loop,               // DFS: append unique non-numeric hi strings
                                   std::vector<std::string> &out) {
    if (!isNumeric(loop.hi) &&                                         // hi is symbolic (e.g. "N", "length")
        std::find(out.begin(), out.end(), loop.hi) == out.end())       // not already collected
        out.push_back(loop.hi);                                        // add to parameter list
    for (const auto &c : loop.children)                                // recurse into all children
        collectSymbolicParams(*c, out);
}

static const BoundLoop* findPipelineLoop(const BoundLoop &loop) {      // DFS: return first loop with PIPELINE pragma
    if (loop.requestedII >= 0) return &loop;                          // found: pragma present
    for (const auto &c : loop.children) {
        const BoundLoop *f = findPipelineLoop(*c);
        if (f) return f;                                               // propagate result upward
    }
    return nullptr;                                                    // not in this subtree
}

static bool findLoopVarsOnPath(const BoundLoop &loop,                  // DFS with backtracking: collect (var,hi) on path
    std::vector<std::pair<std::string,std::string>> &path) {
    path.push_back({loop.var, loop.hi});                               // push current loop onto path
    if (loop.requestedII >= 0) return true;                           // reached pipeline loop — success
    for (const auto &c : loop.children)
        if (findLoopVarsOnPath(*c, path)) return true;                 // propagate success
    path.pop_back();                                                   // backtrack: this loop is not on path
    return false;
}

static std::vector<std::pair<std::string,std::string>> getPipelinePath( // collect (var,hi) pairs from root to pipeline loop
    const BoundFunction &fn) {
    std::vector<std::pair<std::string,std::string>> path;
    for (const auto &loop : fn.loops)
        if (findLoopVarsOnPath(*loop, path)) break;                    // stop after first success
    return path;
}

static bool isArrayBound(const std::string &name,                      // true if a BRAM was declared for this array
    const BoundLoop *pl) {
    if (!pl) return false;
    for (const auto &ab : pl->arrayBindings)
        if (ab.array == name && ab.resource == ResourceKind::BRAM) return true;
    return false;
}

static bool isShiftRegBound(const std::string &name,                   // true if a ShiftReg was declared for this array
    const BoundLoop *pl) {
    if (!pl) return false;
    for (const auto &ab : pl->arrayBindings)
        if (ab.array == name && ab.resource == ResourceKind::ShiftRegister) return true;
    return false;
}

static std::string burstExpr(const std::string &portName,              // look up burst-length expression for a port
    const std::vector<AxiMasterPort> &ports) {
    for (const auto &p : ports)
        if (p.name == portName) return p.burstExpr;                    // return matching expression
    return "1024";                                                     // fallback
}

static std::string hx(int v) {                                         // format integer as 2-digit hex (e.g. 0x10)
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(2) << v;          // always 2 digits
    return s.str();
}

// ── AXI4-Master port signal emitters ─────────────────────────────────────────

static void emitReadPorts(std::ostringstream &s, const std::string &n) { //** emit AXI4-Master read channel port declarations
    s << "    output logic [ADDR_WIDTH-1:0]    m_axi_" << n << "_araddr,\n";  // read address channel
    s << "    output logic [7:0]               m_axi_" << n << "_arlen,\n";   // burst length (beats−1)
    s << "    output logic [2:0]               m_axi_" << n << "_arsize,\n";  // beat size (010 = 4 bytes)
    s << "    output logic [1:0]               m_axi_" << n << "_arburst,\n"; // burst type (01 = INCR)
    s << "    output logic                     m_axi_" << n << "_arvalid,\n"; // address valid
    s << "    input  logic                     m_axi_" << n << "_arready,\n"; // slave ready to accept address
    s << "    input  logic [DATA_WIDTH-1:0]    m_axi_" << n << "_rdata,\n";   // read data beat
    s << "    input  logic [1:0]               m_axi_" << n << "_rresp,\n";   // read response (00 = OKAY)
    s << "    input  logic                     m_axi_" << n << "_rvalid,\n";  // data valid
    s << "    output logic                     m_axi_" << n << "_rready,\n";  // master ready for data
    s << "    input  logic                     m_axi_" << n << "_rlast";      // last beat of burst
}

static void emitWritePorts(std::ostringstream &s, const std::string &n) { //** emit AXI4-Master write channel port declarations
    s << "    output logic [ADDR_WIDTH-1:0]    m_axi_" << n << "_awaddr,\n";  // write address channel
    s << "    output logic [7:0]               m_axi_" << n << "_awlen,\n";   // burst length
    s << "    output logic [2:0]               m_axi_" << n << "_awsize,\n";  // beat size
    s << "    output logic [1:0]               m_axi_" << n << "_awburst,\n"; // burst type
    s << "    output logic                     m_axi_" << n << "_awvalid,\n"; // address valid
    s << "    input  logic                     m_axi_" << n << "_awready,\n"; // slave ready
    s << "    output logic [DATA_WIDTH-1:0]    m_axi_" << n << "_wdata,\n";   // write data beat
    s << "    output logic [DATA_WIDTH/8-1:0]  m_axi_" << n << "_wstrb,\n";   // byte lane enables
    s << "    output logic                     m_axi_" << n << "_wvalid,\n";  // data valid
    s << "    input  logic                     m_axi_" << n << "_wready,\n";  // slave ready
    s << "    output logic                     m_axi_" << n << "_wlast,\n";   // last beat of burst
    s << "    input  logic [1:0]               m_axi_" << n << "_bresp,\n";   // write response
    s << "    input  logic                     m_axi_" << n << "_bvalid,\n";  // response valid
    s << "    output logic                     m_axi_" << n << "_bready";     // master accepts response
}

// ── Section builders — each returns a string of SV text ──────────────────────

static std::string buildHeader(const BoundFunction &fn,                // file-level comment + timescale
    const BoundLoop *pl) {
    std::ostringstream s;
    s << "// ============================================================\n";
    s << "// Generated by FluxHLS Stage 6 — RTL Emitter\n";
    s << "// Function  : " << fn.name << "\n";
    if (pl)
        s << "// Pipeline  : " << pl->id
          << "  depth=" << pl->pipelineDepth
          << "  II="    << pl->requestedII << "\n";
    s << "// ============================================================\n";
    s << "`timescale 1ns/1ps\n\n";
    return s.str();
}

static std::string buildModuleBegin(const BoundFunction &fn,           // module declaration with parameter list
    const std::vector<std::string> &symParams) {
    std::ostringstream s;
    s << "module " << fn.name << "_top #(\n";                         // module name: <func>_top
    s << "    parameter DATA_WIDTH = 32,\n";                           // AXI data bus width in bits
    s << "    parameter ADDR_WIDTH = 64";                              // AXI address width (64-bit for DDR)
    for (const auto &sym : symParams) {                                // one parameter per symbolic loop bound
        std::string pad(std::max(0, 12 - (int)sym.size()), ' ');       // align = signs
        s << ",\n    parameter " << sym << pad << "= 1024";            // default 1024; override at elaboration
    }
    s << "\n)(\n";
    return s.str();
}

static std::string buildPortList(const InterfaceSpec &spec) {          // complete port list
    std::ostringstream s;
    s << "    // Global\n";
    s << "    input  logic                     ap_clk,\n";             // kernel clock
    s << "    input  logic                     ap_rst_n,\n\n";         // active-low synchronous reset

    s << "    // AXI4-Lite slave (s_axilite) — control + scalar registers\n";
    s << "    input  logic [11:0]              s_axilite_awaddr,\n";   // write address (4 KB space)
    s << "    input  logic                     s_axilite_awvalid,\n";
    s << "    output logic                     s_axilite_awready,\n";
    s << "    input  logic [DATA_WIDTH-1:0]    s_axilite_wdata,\n";    // write data (32-bit)
    s << "    input  logic [DATA_WIDTH/8-1:0]  s_axilite_wstrb,\n";   // byte enable
    s << "    input  logic                     s_axilite_wvalid,\n";
    s << "    output logic                     s_axilite_wready,\n";
    s << "    output logic [1:0]               s_axilite_bresp,\n";    // OKAY / SLVERR
    s << "    output logic                     s_axilite_bvalid,\n";
    s << "    input  logic                     s_axilite_bready,\n";
    s << "    input  logic [11:0]              s_axilite_araddr,\n";   // read address
    s << "    input  logic                     s_axilite_arvalid,\n";
    s << "    output logic                     s_axilite_arready,\n";
    s << "    output logic [DATA_WIDTH-1:0]    s_axilite_rdata,\n";    // read data
    s << "    output logic [1:0]               s_axilite_rresp,\n";
    s << "    output logic                     s_axilite_rvalid,\n";
    s << "    input  logic                     s_axilite_rready";      // last s_axilite port (no trailing comma)

    for (size_t pi = 0; pi < spec.axiMaster.size(); ++pi) {            // one block per m_axi port
        const auto &port = spec.axiMaster[pi];
        const std::string &n = port.name;
        bool doRead  = (port.direction == AxiDir::Read  ||
                        port.direction == AxiDir::ReadWrite ||
                        port.direction == AxiDir::Unknown);             // unknown → emit both channels
        bool doWrite = (port.direction == AxiDir::Write ||
                        port.direction == AxiDir::ReadWrite ||
                        port.direction == AxiDir::Unknown);

        s << ",\n\n    // AXI4-Master " << n << "  (m_axi  ";
        if      (port.direction == AxiDir::Read)      s << "READ";
        else if (port.direction == AxiDir::Write)     s << "WRITE";
        else if (port.direction == AxiDir::ReadWrite) s << "READWRITE";
        else                                           s << "dir=unknown";
        s << "  burst=" << port.burstExpr << ")\n";

        if (doRead)            emitReadPorts(s, n);
        if (doRead && doWrite) s << ",\n";                             // comma between read and write groups
        if (doWrite)           emitWritePorts(s, n);
    }
    s << "\n);\n\n";
    return s.str();
}

static std::string buildInternalDecls(const InterfaceSpec &spec,       // all internal signal declarations
    const BoundFunction &fn, const BoundLoop *pl) {
    std::ostringstream s;

    // FSM
    s << "    // ── FSM ─────────────────────────────────────────────────────────\n";
    s << "    localparam IDLE    = 2'd0;   // waiting for ap_start\n";
    s << "    localparam RUNNING = 2'd1;   // pipeline executing\n";
    s << "    localparam DONE    = 2'd2;   // one-cycle ap_done pulse\n";
    s << "    logic [1:0] state;\n\n";

    // AXI4-Lite register file declarations
    s << "    // ── AXI4-Lite register file ──────────────────────────────────────\n";
    s << "    logic [31:0] ctrl_reg;   // 0x00  ap_start[0] ap_done[1] ap_idle[2]\n";
    s << "    logic [31:0] gier_reg;   // 0x04  global interrupt enable\n";
    s << "    logic [31:0] ier_reg;    // 0x08  IP interrupt enable\n";
    s << "    logic [31:0] isr_reg;    // 0x0C  IP interrupt status\n";
    for (const auto &r : spec.axiLite) {                               // one declaration per non-ctrl register
        if (r.kind == "handshake" || r.kind == "interrupt") continue;  // already declared above
        if (r.widthBits == 64)
            s << "    logic [63:0] " << r.name << "_reg;";             // 64-bit base address pointer
        else
            s << "    logic [31:0] " << r.name << "_reg;";             // 32-bit scalar register
        s << "   // 0x" << hx(r.offset) << "  " << r.description << "\n";
    }
    s << "\n";

    // Loop counters
    auto path = getPipelinePath(fn);
    if (!path.empty()) {
        s << "    // ── Loop counters ────────────────────────────────────────────\n";
        for (const auto &[var, hi] : path)
            s << "    logic [31:0] " << var << "_cnt;   // iterates 0 .. " << hi << " - 1\n";
        s << "    logic        loop_done;   // high when all loop iterations complete\n\n";
    }

    // Pipeline valid signals (one per stage transition)
    if (pl && pl->pipelineDepth > 0) {
        s << "    // ── Pipeline stage valid signals (depth=" << pl->pipelineDepth << ") ─────────────────\n";
        for (int cyc = 0; cyc <= pl->pipelineDepth; ++cyc)
            s << "    logic pipe_v" << cyc << ";   // valid: data entering stage " << cyc << " is meaningful\n";
        s << "\n";
    }

    // Pipeline data registers
    if (pl) {
        bool hasAcc = false;
        s << "    // ── Pipeline data registers ──────────────────────────────────\n";
        for (const auto &op : pl->opBindings) {
            if (op.kind == OpKind::Load)
                s << "    logic [DATA_WIDTH-1:0] pipe_s" << op.cycle
                  << "_" << op.array << ";   // stage-" << op.cycle << " captured load: " << op.desc << "\n";
            if (op.kind == OpKind::FAdd || op.kind == OpKind::FMul)
                s << "    logic [DATA_WIDTH-1:0] pipe_s" << op.cycle
                  << "_result;   // DSP output: " << op.desc << "\n";
            if (op.kind == OpKind::Init || op.kind == OpKind::MAC) hasAcc = true;
        }
        if (hasAcc)
            s << "    logic [DATA_WIDTH-1:0] acc_reg;   // REDUCTION accumulator (maps to Register)\n";
        // Pipelined address register for STORE
        bool hasStore = false;
        for (const auto &op : pl->opBindings) if (op.kind == OpKind::Store) hasStore = true;
        if (hasStore)
            s << "    logic [31:0]           store_addr;   // address delayed through pipeline to match STORE\n";
        s << "\n";

        // BRAM declarations
        bool hasBRAM = false;
        for (const auto &ab : pl->arrayBindings) {
            if (ab.resource != ResourceKind::BRAM) continue;
            if (!hasBRAM) {
                s << "    // ── Behavioral BRAMs (Vivado infers RAMB36/18) ───────────────\n";
                hasBRAM = true;
            }
            std::string bExpr = burstExpr(ab.array, spec.axiMaster);  // array size expression
            s << "    localparam DEPTH_" << ab.array << " = " << bExpr << ";\n";
            s << "    logic [DATA_WIDTH-1:0] mem_" << ab.array
              << " [0:DEPTH_" << ab.array << "-1];   // " << ab.reason << "\n";
        }
        if (hasBRAM) s << "\n";

        // ShiftReg declarations
        bool hasSR = false;
        for (const auto &ab : pl->arrayBindings) {
            if (ab.resource != ResourceKind::ShiftRegister) continue;
            if (!hasSR) {
                s << "    // ── Behavioral ShiftRegisters (Vivado infers SRL16/32) ────────\n";
                hasSR = true;
            }
            std::string depth = "16";                                  // fallback depth
            size_t pos = ab.configStr.find("depth=");                  // parse "depth=TAPS" from configStr
            if (pos != std::string::npos)
                depth = ab.configStr.substr(pos + 6);                  // extract value after "depth="
            s << "    localparam SR_DEPTH_" << ab.array << " = " << depth << ";\n";
            s << "    logic [DATA_WIDTH-1:0] shreg_" << ab.array
              << " [0:SR_DEPTH_" << ab.array << "-1];   // delay line for sliding-window access\n";
        }
        if (hasSR) s << "\n";
    }

    // AXI burst engine state machine signals (one set per port)
    for (const auto &port : spec.axiMaster) {
        const std::string &n = port.name;
        bool doRead  = (port.direction == AxiDir::Read  || port.direction == AxiDir::ReadWrite || port.direction == AxiDir::Unknown);
        bool doWrite = (port.direction == AxiDir::Write || port.direction == AxiDir::ReadWrite || port.direction == AxiDir::Unknown);
        if (doRead) {
            s << "    localparam AR_IDLE_" << n << " = 2'd0, "
              << "AR_ADDR_" << n << " = 2'd1, "
              << "AR_DATA_" << n << " = 2'd2;\n";
            s << "    logic [1:0]  ar_state_" << n << ";   // read burst FSM state\n";
            s << "    logic [31:0] ar_beat_"  << n << ";   // current read beat index\n";
        }
        if (doWrite) {
            s << "    localparam AW_IDLE_" << n << " = 2'd0, "
              << "AW_ADDR_" << n << " = 2'd1, "
              << "AW_DATA_" << n << " = 2'd2, "
              << "AW_RESP_" << n << " = 2'd3;\n";
            s << "    logic [1:0]  aw_state_" << n << ";   // write burst FSM state\n";
            s << "    logic [31:0] aw_beat_"  << n << ";   // current write beat index\n";
        }
        s << "\n";
    }

    return s.str();
}

static std::string buildAxiLiteLogic(const InterfaceSpec &spec) {      //** AXI4-Lite register file logic
    std::ostringstream s;

    // Always-ready handshake: simplified slave (no backpressure)
    s << "    // ── AXI4-Lite handshake (always-ready) ──────────────────────────\n";
    s << "    assign s_axilite_awready = 1'b1;\n";                     // accept write address immediately
    s << "    assign s_axilite_wready  = 1'b1;\n";                     // accept write data immediately
    s << "    assign s_axilite_bresp   = 2'b00;\n";                    // always OKAY
    s << "    assign s_axilite_bvalid  = 1'b1;\n";                     // response always valid
    s << "    assign s_axilite_arready = 1'b1;\n";                     // accept read address immediately
    s << "    assign s_axilite_rresp   = 2'b00;\n";                    // always OKAY
    s << "    assign s_axilite_rvalid  = 1'b1;\n\n";                   // read data always valid

    // Write channel: synchronous register update on AW+W valid
    s << "    // Register write — latched on coincident AW + W valid\n";
    s << "    always_ff @(posedge ap_clk) begin\n";
    s << "        if (!ap_rst_n) begin\n";
    s << "            ctrl_reg <= 32'h4;   // ap_idle = 1 on reset\n"; // idle bit asserted at reset
    s << "            gier_reg <= '0;\n";
    s << "            ier_reg  <= '0;\n";
    s << "            isr_reg  <= '0;\n";
    for (const auto &r : spec.axiLite) {
        if (r.kind == "handshake" || r.kind == "interrupt") continue;
        s << "            " << r.name << "_reg <= '0;\n";              // clear all user registers
    }
    s << "        end else begin\n";
    s << "            if (s_axilite_awvalid && s_axilite_wvalid) begin\n"; //** latch on AW + W simultaneous
    s << "                case (s_axilite_awaddr[7:0])\n";
    for (const auto &r : spec.axiLite) {
        std::string rn;
        if      (r.kind == "handshake")      rn = "ctrl_reg";
        else if (r.name == "ap_gier")        rn = "gier_reg";
        else if (r.name == "ap_ier")         rn = "ier_reg";
        else if (r.name == "ap_isr")         rn = "isr_reg";
        else                                 rn = r.name + "_reg";
        if (r.widthBits == 32) {
            s << "                    8'h" << hx(r.offset)
              << ": " << rn << " <= s_axilite_wdata;\n";               // 32-bit single-word write
        } else {                                                        // 64-bit split into two 32-bit words
            s << "                    8'h" << hx(r.offset)
              << ": " << rn << "[31:0]  <= s_axilite_wdata;\n";        // lower word at base offset
            s << "                    8'h" << hx(r.offset + 4)
              << ": " << rn << "[63:32] <= s_axilite_wdata;\n";        // upper word at offset + 4
        }
    }
    s << "                    default: ;\n";
    s << "                endcase\n";
    s << "            end\n";
    s << "            // FSM drives ap_start auto-clear, ap_done, ap_idle\n";
    s << "            if (state == RUNNING) ctrl_reg[0] <= 1'b0;   // auto-clear ap_start once running\n";
    s << "            ctrl_reg[1] <= (state == DONE);               // pulse ap_done for one cycle\n";
    s << "            ctrl_reg[2] <= (state == IDLE);               // ap_idle when not executing\n";
    s << "        end\n";
    s << "    end\n\n";

    // Read channel: combinational mux
    s << "    // Register read — combinational address decode\n";
    s << "    always_comb begin\n";
    s << "        case (s_axilite_araddr[7:0])\n";
    s << "            8'h00: s_axilite_rdata = ctrl_reg;\n";
    s << "            8'h04: s_axilite_rdata = gier_reg;\n";
    s << "            8'h08: s_axilite_rdata = ier_reg;\n";
    s << "            8'h0C: s_axilite_rdata = isr_reg;\n";
    for (const auto &r : spec.axiLite) {
        if (r.kind == "handshake" || r.kind == "interrupt") continue;
        std::string rn = r.name + "_reg";
        if (r.widthBits == 32) {
            s << "            8'h" << hx(r.offset)
              << ": s_axilite_rdata = " << rn << ";\n";
        } else {
            s << "            8'h" << hx(r.offset)
              << ": s_axilite_rdata = " << rn << "[31:0];\n";          // read lower word
            s << "            8'h" << hx(r.offset + 4)
              << ": s_axilite_rdata = " << rn << "[63:32];\n";         // read upper word
        }
    }
    s << "            default: s_axilite_rdata = 32'hDEAD_BEEF;   // undefined address\n";
    s << "        endcase\n";
    s << "    end\n\n";

    return s.str();
}

static std::string buildFSM(const BoundFunction &fn) {                 //** kernel FSM: IDLE → RUNNING → DONE → IDLE
    auto path = getPipelinePath(fn);
    std::ostringstream s;
    s << "    // ── Kernel FSM ───────────────────────────────────────────────────\n";
    s << "    always_ff @(posedge ap_clk) begin\n";
    s << "        if (!ap_rst_n) state <= IDLE;\n";                    // synchronous active-low reset
    s << "        else case (state)\n";
    s << "            IDLE   : if (ctrl_reg[0]) state <= RUNNING;   // ap_start triggers execution\n";
    if (!path.empty())
        s << "            RUNNING: if (loop_done)   state <= DONE;   // all loop iterations complete\n";
    else
        s << "            RUNNING:                  state <= DONE;   // single-cycle kernel\n";
    s << "            DONE   :                      state <= IDLE;   // one-cycle ap_done pulse\n";
    s << "            default:                      state <= IDLE;\n";
    s << "        endcase\n";
    s << "    end\n\n";
    return s.str();
}

static std::string buildLoopCounters(const BoundFunction &fn) {        //** nested loop counter logic with carry chain
    auto path = getPipelinePath(fn);
    if (path.empty()) return "";                                        // no counters for pipeline-free kernels

    std::ostringstream s;
    const auto &[outerVar, outerHi] = path.front();                    // outermost loop
    s << "    // ── Loop counters ────────────────────────────────────────────────\n";

    // loop_done when outermost counter reaches its bound
    if (isNumeric(outerHi))
        s << "    assign loop_done = (" << outerVar << "_cnt == " << outerHi << ");\n\n";
    else
        s << "    assign loop_done = (" << outerVar << "_cnt >= " << outerHi << ");\n\n";

    s << "    always_ff @(posedge ap_clk) begin\n";
    s << "        if (!ap_rst_n || state == IDLE) begin\n";            // reset counters at start
    for (const auto &[var, hi] : path)
        s << "            " << var << "_cnt <= '0;\n";
    s << "        end else if (state == RUNNING && !loop_done) begin\n";

    if (path.size() == 1) {                                            // single loop: simple increment
        s << "            " << path[0].first << "_cnt <= " << path[0].first << "_cnt + 1;\n";
    } else {                                                           // nested loops: carry chain, innermost first
        const auto &[innerVar, innerHi] = path.back();
        // Innermost counter: wrap or increment
        std::string innerWrap = isNumeric(innerHi)
            ? (innerVar + "_cnt == " + innerHi + " - 1")
            : (innerVar + "_cnt >= " + innerHi + " - 1");
        s << "            // Innermost counter wraps; each outer counter carries\n";
        s << "            if (" << innerWrap << ")\n";
        s << "                " << innerVar << "_cnt <= '0;\n";
        s << "            else\n";
        s << "                " << innerVar << "_cnt <= " << innerVar << "_cnt + 1;\n";
        // Outer counters increment on inner wrap
        for (int i = (int)path.size() - 2; i >= 0; --i) {
            const auto &[ovar, ohi] = path[i];
            const auto &[ivar, ihi] = path[i + 1];
            std::string iw = isNumeric(ihi)
                ? (ivar + "_cnt == " + ihi + " - 1")
                : (ivar + "_cnt >= " + ihi + " - 1");
            s << "            if (" << iw << ")\n";
            s << "                " << ovar << "_cnt <= " << ovar << "_cnt + 1;\n";
        }
    }
    s << "        end\n";
    s << "    end\n\n";
    return s.str();
}

static std::string buildPipeline(const BoundFunction &fn,              //** pipeline stage registers + DSP inference
    const BoundLoop *pl) {
    if (!pl) return "";
    std::ostringstream s;
    auto path = getPipelinePath(fn);
    // Index used by pipeline to address BRAMs (innermost counter on path)
    std::string pipeIdx = path.empty() ? "0" : path.back().first + "_cnt";

    s << "    // ── Pipeline stages (depth=" << pl->pipelineDepth << ") ─────────────────────────────────\n\n";

    // Identify op kinds present
    std::vector<std::string> loadArrays;                               // arrays read in stage 0
    bool hasAcc  = false;                                              // MAC / Init present
    bool hasFAdd = false;                                              // FAdd / FMul present
    bool hasStore = false;
    std::string storeArray;
    for (const auto &op : pl->opBindings) {
        if (op.kind == OpKind::Load)  loadArrays.push_back(op.array);
        if (op.kind == OpKind::MAC  || op.kind == OpKind::Init)  hasAcc  = true;
        if (op.kind == OpKind::FAdd || op.kind == OpKind::FMul)  hasFAdd = true;
        if (op.kind == OpKind::Store) { hasStore = true; storeArray = op.array; }
    }

    // ── Stage 0: capture inputs ──────────────────────────────────────────────
    s << "    // Stage 0 — capture inputs from BRAM / ShiftReg into pipeline registers\n";
    s << "    always_ff @(posedge ap_clk) begin\n";
    s << "        pipe_v0    <= (state == RUNNING) && !loop_done;   // valid when kernel is active\n";
    s << "        store_addr <= " << pipeIdx << ";   // capture load address (delayed to match STORE stage)\n";
    for (const auto &array : loadArrays) {
        if (isArrayBound(array, pl)) {                                 // BRAM: synchronous array read
            s << "        pipe_s0_" << array
              << " <= mem_" << array << "[" << pipeIdx << "];   // BRAM synchronous read\n";
        } else if (isShiftRegBound(array, pl)) {                       // ShiftReg: read tap 0
            s << "        pipe_s0_" << array
              << " <= shreg_" << array << "[0];   // ShiftReg tap 0 (most-recent element)\n";
        } else {                                                       // no binding visible (conv2d limitation)
            s << "        // NOTE: " << array << " has no visible binding (Stage 1 limitation)\n";
            s << "        pipe_s0_" << array << " <= '0;   // placeholder\n";
        }
    }
    s << "    end\n\n";

    // ShiftReg advance (separate always_ff to avoid multiple drivers)
    for (const auto &ab : pl->arrayBindings) {
        if (ab.resource != ResourceKind::ShiftRegister) continue;
        s << "    // Advance shift register " << ab.array << " on every active cycle\n";
        s << "    always_ff @(posedge ap_clk) begin\n";
        s << "        if (state == RUNNING) begin\n";
        s << "            for (int sr_i = SR_DEPTH_" << ab.array << " - 1; sr_i > 0; sr_i--)\n";
        s << "                shreg_" << ab.array << "[sr_i] <= shreg_" << ab.array << "[sr_i - 1];\n";
        s << "            shreg_" << ab.array                          // AXI data feeds the head of the delay line
          << "[0] <= m_axi_" << ab.array << "_rdata;   // new sample from AXI read engine\n";
        s << "        end\n";
        s << "    end\n\n";
    }

    // ── Stage 1: compute ─────────────────────────────────────────────────────
    s << "    // Stage 1 — compute (Vivado infers DSP48 for multiply/add)\n";
    s << "    always_ff @(posedge ap_clk) begin\n";
    s << "        pipe_v1 <= pipe_v0;\n";                              // propagate valid through pipeline
    if (hasFAdd && loadArrays.size() >= 2) {                           // FADD: sum two loaded values
        s << "        // Floating-point add — synthesis maps to DSP48 + FP IP\n";
        s << "        pipe_s1_result <= pipe_s0_" << loadArrays[0]
          << " + pipe_s0_" << loadArrays[1] << ";   // 32-bit behavioral add\n";
    } else if (hasAcc) {                                               // MAC: multiply-accumulate
        s << "        // Multiply-accumulate — synthesis maps to DSP48\n";
        if (loadArrays.size() >= 2) {
            s << "        if (!pipe_v0)\n";
            s << "            acc_reg <= '0;   // reset accumulator at start of each new dot product\n";
            s << "        else\n";
            s << "            acc_reg <= acc_reg + (pipe_s0_" << loadArrays[0]
              << " * pipe_s0_" << loadArrays[1] << ");   // MAC\n";
        } else {                                                       // no visible operands (conv2d)
            s << "        // conv2d: Stage 1 cannot see 2D array subscripts\n";
            s << "        if (!pipe_v0)\n";
            s << "            acc_reg <= '0;   // reset for each output pixel\n";
            s << "        else\n";
            s << "            acc_reg <= acc_reg + 32'h0;   // placeholder: replace with input*kernel\n";
        }
    }
    s << "    end\n\n";

    // ── Stage 2: store ───────────────────────────────────────────────────────
    if (hasStore && !storeArray.empty() && isArrayBound(storeArray, pl)) {
        std::string resultSig = hasAcc ? "acc_reg" : "pipe_s1_result"; // what to store
        s << "    // Stage 2 — store result to BRAM (pipeline latency = " << pl->pipelineDepth << " cycles)\n";
        s << "    always_ff @(posedge ap_clk) begin\n";
        s << "        pipe_v2 <= pipe_v1;\n";
        s << "        if (pipe_v1)\n";
        s << "            mem_" << storeArray
          << "[store_addr] <= " << resultSig                           // write result using delayed address
          << ";   // BRAM write (address pipelined alongside data)\n";
        s << "    end\n\n";
    } else if (pl->pipelineDepth > 0) {                                // no visible BRAM for store
        s << "    // Stage 2 — valid propagation (no BRAM store binding visible)\n";
        s << "    always_ff @(posedge ap_clk) pipe_v2 <= pipe_v1;\n\n";
    }

    return s.str();
}

static std::string buildAxiReadEngine(const AxiMasterPort &port,       //** AXI4-Master burst read state machine
    const BoundLoop *pl) {
    const std::string &n  = port.name;
    const std::string  bx = port.burstExpr;                            // burst length expression
    std::string baseReg   = n + "_base_reg";                           // AXI4-Lite base address register
    std::ostringstream s;
    s << "    // ── AXI4-Master " << n << " read engine ─────────────────────────────────\n";
    s << "    always_ff @(posedge ap_clk) begin\n";
    s << "        if (!ap_rst_n) begin\n";
    s << "            ar_state_" << n << " <= AR_IDLE_" << n << ";\n";
    s << "            ar_beat_"  << n << " <= '0;\n";
    s << "            m_axi_" << n << "_arvalid <= 1'b0;\n";
    s << "            m_axi_" << n << "_rready  <= 1'b0;\n";
    s << "            m_axi_" << n << "_arburst <= 2'b01;   // INCR\n";
    s << "            m_axi_" << n << "_arsize  <= 3'b010;  // 4 bytes\n";
    s << "        end else case (ar_state_" << n << ")\n";
    // AR_IDLE: issue read address when kernel starts
    s << "            AR_IDLE_" << n << ": if (state == RUNNING && ar_beat_" << n << " == 0) begin\n";
    s << "                m_axi_" << n << "_araddr  <= " << baseReg << ";\n"; //** start at base address
    s << "                m_axi_" << n << "_arlen   <= " << bx << " - 1;\n"; // burst = all elements
    s << "                m_axi_" << n << "_arvalid <= 1'b1;\n";
    s << "                ar_state_" << n << " <= AR_ADDR_" << n << ";\n";
    s << "            end\n";
    // AR_ADDR: wait for arready
    s << "            AR_ADDR_" << n << ": if (m_axi_" << n << "_arready) begin\n";
    s << "                m_axi_" << n << "_arvalid <= 1'b0;\n";
    s << "                m_axi_" << n << "_rready  <= 1'b1;\n";
    s << "                ar_state_" << n << " <= AR_DATA_" << n << ";\n";
    s << "            end\n";
    // AR_DATA: receive data beats, fill BRAM or ShiftReg
    s << "            AR_DATA_" << n << ": if (m_axi_" << n << "_rvalid) begin\n";
    if (isArrayBound(n, pl)) {                                         // store beat to BRAM
        s << "                mem_" << n
          << "[ar_beat_" << n << "] <= m_axi_" << n << "_rdata;   // fill BRAM from DDR\n";
        s << "                ar_beat_" << n << " <= ar_beat_" << n << " + 1;\n";
    } else if (isShiftRegBound(n, pl)) {                               // ShiftReg fed in pipeline stage 0
        s << "                // ShiftReg " << n << " is fed by m_axi_" << n
          << "_rdata in pipeline stage 0\n";
        s << "                ar_beat_" << n << " <= ar_beat_" << n << " + 1;\n";
    } else {                                                           // no binding (conv2d direction=unknown)
        s << "                // No BRAM binding visible for " << n
          << " — data arrives but cannot be stored locally\n";
        s << "                ar_beat_" << n << " <= ar_beat_" << n << " + 1;\n";
    }
    s << "                if (m_axi_" << n << "_rlast) begin\n";       // last beat of burst
    s << "                    m_axi_" << n << "_rready <= 1'b0;\n";
    s << "                    ar_state_" << n << " <= AR_IDLE_" << n << ";\n";
    s << "                end\n";
    s << "            end\n";
    s << "            default: ar_state_" << n << " <= AR_IDLE_" << n << ";\n";
    s << "        endcase\n";
    s << "    end\n\n";
    return s.str();
}

static std::string buildAxiWriteEngine(const AxiMasterPort &port,      //** AXI4-Master burst write state machine
    const BoundLoop *pl) {
    const std::string &n  = port.name;
    const std::string  bx = port.burstExpr;
    std::string baseReg   = n + "_base_reg";
    std::ostringstream s;
    s << "    // ── AXI4-Master " << n << " write engine ────────────────────────────────\n";
    s << "    always_ff @(posedge ap_clk) begin\n";
    s << "        if (!ap_rst_n) begin\n";
    s << "            aw_state_" << n << " <= AW_IDLE_" << n << ";\n";
    s << "            aw_beat_"  << n << " <= '0;\n";
    s << "            m_axi_" << n << "_awvalid <= 1'b0;\n";
    s << "            m_axi_" << n << "_wvalid  <= 1'b0;\n";
    s << "            m_axi_" << n << "_wlast   <= 1'b0;\n";
    s << "            m_axi_" << n << "_bready  <= 1'b0;\n";
    s << "            m_axi_" << n << "_awburst <= 2'b01;   // INCR\n";
    s << "            m_axi_" << n << "_awsize  <= 3'b010;  // 4 bytes\n";
    s << "            m_axi_" << n << "_wstrb   <= 4'hF;   // all byte lanes valid\n";
    s << "        end else case (aw_state_" << n << ")\n";
    // AW_IDLE: start write after pipeline drains (DONE state)
    s << "            AW_IDLE_" << n << ": if (state == DONE) begin\n";
    s << "                m_axi_" << n << "_awaddr  <= " << baseReg << ";\n";
    s << "                m_axi_" << n << "_awlen   <= " << bx << " - 1;\n";
    s << "                m_axi_" << n << "_awvalid <= 1'b1;\n";
    s << "                aw_state_" << n << " <= AW_ADDR_" << n << ";\n";
    s << "            end\n";
    // AW_ADDR: wait for awready
    s << "            AW_ADDR_" << n << ": if (m_axi_" << n << "_awready) begin\n";
    s << "                m_axi_" << n << "_awvalid <= 1'b0;\n";
    s << "                m_axi_" << n << "_wvalid  <= 1'b1;\n";
    s << "                aw_state_" << n << " <= AW_DATA_" << n << ";\n";
    s << "            end\n";
    // AW_DATA: send data beats from BRAM
    s << "            AW_DATA_" << n << ": if (m_axi_" << n << "_wready) begin\n";
    if (isArrayBound(n, pl)) {                                         // drain BRAM to DDR
        s << "                m_axi_" << n << "_wdata <= mem_" << n
          << "[aw_beat_" << n << "];   // read result from BRAM\n";    //** drain output BRAM over AXI
    } else {
        s << "                m_axi_" << n << "_wdata <= 32'h0;   // no BRAM binding visible\n";
    }
    s << "                m_axi_" << n << "_wlast  <= (aw_beat_" << n << " == " << bx << " - 2);\n";
    s << "                aw_beat_" << n << " <= aw_beat_" << n << " + 1;\n";
    s << "                if (m_axi_" << n << "_wlast) begin\n";
    s << "                    m_axi_" << n << "_wvalid <= 1'b0;\n";
    s << "                    m_axi_" << n << "_bready <= 1'b1;\n";
    s << "                    aw_state_" << n << " <= AW_RESP_" << n << ";\n";
    s << "                end\n";
    s << "            end\n";
    // AW_RESP: wait for write response
    s << "            AW_RESP_" << n << ": if (m_axi_" << n << "_bvalid) begin\n";
    s << "                m_axi_" << n << "_bready <= 1'b0;\n";
    s << "                aw_state_" << n << " <= AW_IDLE_" << n << ";\n";
    s << "            end\n";
    s << "            default: aw_state_" << n << " <= AW_IDLE_" << n << ";\n";
    s << "        endcase\n";
    s << "    end\n\n";
    return s.str();
}

// ── Top-level SV assembler ────────────────────────────────────────────────────

static std::string buildSV(const BoundFunction &fn,                    //** assemble complete SV module for one function
    const InterfaceSpec &spec) {
    // Collect symbolic loop-bound parameters (e.g. "N", "length")
    std::vector<std::string> symParams;
    for (const auto &loop : fn.loops) collectSymbolicParams(*loop, symParams);

    // Find the pipelined loop (first loop tree with requestedII >= 0)
    const BoundLoop *pl = nullptr;
    for (const auto &loop : fn.loops) {
        pl = findPipelineLoop(*loop);
        if (pl) break;
    }

    std::string sv;
    sv += buildHeader(fn, pl);                                         // file comment + timescale
    sv += buildModuleBegin(fn, symParams);                             // module <name>_top #(params)
    sv += buildPortList(spec);                                         // port list  );\n
    sv += buildInternalDecls(spec, fn, pl);                            // internal signals, BRAMs, ShiftRegs
    sv += buildAxiLiteLogic(spec);                                     // register file write + read logic
    sv += buildFSM(fn);                                                // IDLE/RUNNING/DONE state machine
    sv += buildLoopCounters(fn);                                       // loop counter always_ff + loop_done

    if (pl) {
        sv += buildPipeline(fn, pl);                                   // pipeline stages + DSP inference

        for (const auto &port : spec.axiMaster) {                      // one burst engine per m_axi port
            bool doRead  = (port.direction == AxiDir::Read  ||
                            port.direction == AxiDir::ReadWrite ||
                            port.direction == AxiDir::Unknown);
            bool doWrite = (port.direction == AxiDir::Write ||
                            port.direction == AxiDir::ReadWrite ||
                            port.direction == AxiDir::Unknown);
            if (doRead)  sv += buildAxiReadEngine(port, pl);           // AR + R channels
            if (doWrite) sv += buildAxiWriteEngine(port, pl);          // AW + W + B channels
        }
    }

    sv += "endmodule   // " + fn.name + "_top\n";                     // close the module
    return sv;
}

// ── Count lines in a string ───────────────────────────────────────────────────

static int countLines(const std::string &s) {                          // count '\n' characters
    return (int)std::count(s.begin(), s.end(), '\n');
}

// ── Public API ────────────────────────────────────────────────────────────────

void buildRTL(const InterfaceContext &ifCtx,                           //** top-level: build SV for every function
              const BindingContext   &bindCtx,
              RTLContext             &rtlCtx) {
    for (const auto &spec : ifCtx.specs) {                             // iterate over every interface spec
        const BoundFunction *fn = nullptr;                             // find matching BoundFunction by name
        for (const auto &f : bindCtx.functions)
            if (f.name == spec.funcName) { fn = &f; break; }
        if (!fn) continue;                                             // no match → skip

        RTLModule mod;
        mod.funcName  = fn->name;                                      // store function name
        mod.fileName  = "output/" + fn->name + ".sv";                  // output path
        mod.svText    = buildSV(*fn, spec);                            // generate complete SV text
        mod.lineCount = countLines(mod.svText);                        // count lines for display
        rtlCtx.modules.push_back(std::move(mod));                      // append to result
    }
}

void dumpRTL(const RTLContext &rtlCtx) {                               //** write .sv files + print summary to stdout
    // Create output/ directory (no-op if it already exists)
    fs::create_directories("output");                                  // safe: create_directories is idempotent

    std::cout << std::string(56, '=') << "\n\n";
    std::cout << "  STAGE 6 — RTL EMISSION\n\n";

    for (const auto &mod : rtlCtx.modules) {
        // Write .sv file to disk
        std::ofstream ofs(mod.fileName);                               // open output file
        ofs << mod.svText;                                             // write complete SV text
        ofs.close();                                                   // flush and close

        // Print summary to stdout
        std::cout << "  Function : " << mod.funcName << "\n";
        std::cout << "  Module   : " << mod.funcName << "_top\n";
        std::cout << "  Written  : " << mod.fileName
                  << "  (" << mod.lineCount << " lines)\n";

        // Print module declaration (parameters + port summary) from the SV text
        // Extract lines up to and including the closing ");\n" of the port list
        std::istringstream stream(mod.svText);
        std::string line;
        int printed = 0;
        bool inPortList = false;
        while (std::getline(stream, line) && printed < 60) {           // cap at 60 lines for readability
            // Skip long internal-declaration lines (start of body)
            if (line.find("// ── FSM") != std::string::npos) break;   // stop before internal body
            std::cout << "  " << line << "\n";
            ++printed;
            if (line == ");") { std::cout << "\n"; break; }            // stop after port list closes
        }
        std::cout << "\n";
    }
}
