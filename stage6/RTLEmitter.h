#pragma once
#include "stage5/InterfaceContext.h"  // InterfaceContext — AXI port and register map info
#include "stage4/BindingContext.h"    // BindingContext   — pipeline structure + array bindings
#include "stage6/RTLContext.h"        // RTLContext       — Stage 6 IR produced here

// buildRTL: consume Stage 4 + Stage 5 IR and produce Stage 6 IR.
//
// For each kernel function it walks the BoundFunction tree (for pipeline structure
// and array bindings) together with the matching InterfaceSpec (for AXI port list
// and register map offsets) and emits a complete SystemVerilog module string.
//
// Generated SV sections per function:
//   - Module header + parameterised port list (ap_clk, s_axilite_*, m_axi_*)
//   - AXI4-Lite register file (write always_ff + read always_comb)
//   - Kernel FSM (IDLE / RUNNING / DONE)
//   - Loop counters (one per loop level on path to pipeline loop)
//   - Pipeline stage registers (depth = BoundLoop::pipelineDepth)
//   - Behavioral BRAMs / ShiftRegs (Vivado infers RAMB36/SRL16)
//   - AXI4-Master burst engines (read and/or write, one per m_axi port)
void buildRTL(const InterfaceContext &ifCtx,
              const BindingContext   &bindCtx,
              RTLContext             &rtlCtx);

// dumpRTL: write each RTLModule to disk and print a summary to stdout.
//
// Writes output/<funcName>.sv for every module.
// Stdout shows: module name, parameters, port summary, pipeline info, file path.
void dumpRTL(const RTLContext &rtlCtx);
