# Stage 6 — RTL Emission

## Overview

Stage 6 is the final stage. It takes the complete accumulated IR from Stage 4
(`BindingContext`) and Stage 5 (`InterfaceContext`) and emits one synthesisable
**SystemVerilog** file per kernel function, written to `output/<func>.sv`.

The generated SV is behavioural RTL: standard `always_ff` / `always_comb` blocks
that Vivado's synthesiser maps to real FPGA primitives automatically.

---

## What Stage 6 Reads

| Source | Used for |
|---|---|
| `InterfaceContext` — `AxiLiteReg` list | Register offsets and widths for the AXI4-Lite slave |
| `InterfaceContext` — `AxiMasterPort` list | Port direction, burst expression, binding description |
| `BindingContext` — `BoundFunction.params` | Symbolic loop bound parameters |
| `BindingContext` — `BoundLoop` tree (DFS) | Loop variable names, `hi` bounds, pipeline loop location |
| `BindingContext` — `BoundLoop.arrayBindings` | BRAM / ShiftReg declarations and array sizes |
| `BindingContext` — `BoundLoop.opBindings` | Op types (Load/FAdd/MAC/Store) and cycle assignments |

---

## Generated Module Structure

Every `.sv` file has the same seven sections in order:

```
1. File header comment  (function name, pipeline loop, depth, II)
2. Module declaration   (parameterised port list)
3. Internal declarations (FSM state, registers, BRAMs, ShiftRegs, burst-engine state)
4. AXI4-Lite register file  (write always_ff + read always_comb)
5. Kernel FSM           (IDLE / RUNNING / DONE)
6. Loop counters        (one per level on path to pipeline loop)
7. Pipeline stages      (stage-0 loads → stage-1 compute → stage-2 store)
   + AXI4-Master burst engines  (read and/or write, one per m_axi port)
```

---

## Section-by-section Explanation

### 1. Module Parameters

```systemverilog
module vadd_top #(
    parameter DATA_WIDTH = 32,    // AXI bus width — always 32 for float/int kernels
    parameter ADDR_WIDTH = 64,    // DDR address width
    parameter N          = 1024   // symbolic loop bound — override at elaboration
)(
```

Every non-numeric loop `hi` string found in the BoundLoop tree becomes a
`parameter` with default `1024`. Numeric bounds (e.g. `30` for conv2d) are
inlined directly — no parameter needed.

### 2. Port List

Three groups:
- `ap_clk`, `ap_rst_n` — global clock and active-low synchronous reset
- `s_axilite_*` — one AXI4-Lite slave (17 signals, same for every kernel)
- `m_axi_<name>_*` — one AXI4-Master port per `m_axi` parameter:
  - READ ports: `ar*` (address) + `r*` (data)
  - WRITE ports: `aw*` (address) + `w*` (data) + `b*` (response)
  - Unknown direction (conv2d): both read and write channels emitted

### 3. AXI4-Lite Register File

Matches the Stage 5 register map exactly:

| Address | Register | Width | Updated by |
|---------|----------|-------|------------|
| 0x00 | `ctrl_reg` | 32-bit | CPU writes ap_start; FSM writes ap_done/ap_idle |
| 0x04 | `gier_reg` | 32-bit | CPU only |
| 0x08 | `ier_reg` | 32-bit | CPU only |
| 0x0C | `isr_reg` | 32-bit | CPU only |
| 0x10+ | scalar regs | 32-bit | CPU sets before ap_start |
| after | `<name>_base_reg` | 64-bit | CPU writes DDR pointer in two 32-bit transactions |

The slave is **always-ready**: `awready`, `wready`, `arready`, `rvalid` are all
tied `1'b1`. Writes are latched when `awvalid && wvalid` coincide.

### 4. Kernel FSM

```systemverilog
localparam IDLE=2'd0, RUNNING=2'd1, DONE=2'd2;

always_ff @(posedge ap_clk)
    case (state)
        IDLE   : if (ctrl_reg[0]) state <= RUNNING;  // ap_start
        RUNNING: if (loop_done)   state <= DONE;
        DONE   :                  state <= IDLE;      // one-cycle ap_done pulse
    endcase
```

`ap_start` is auto-cleared (bit 0) once RUNNING. `ap_done` (bit 1) pulses for
exactly one cycle in DONE. `ap_idle` (bit 2) is high in IDLE.

### 5. Loop Counters

One `logic [31:0]` counter per loop on the path from the function root down to
the pipelined loop.

For a single loop (vadd):
```systemverilog
assign loop_done = (i_cnt >= N);
always_ff @(posedge ap_clk)
    if (!ap_rst_n || state == IDLE) i_cnt <= '0;
    else if (state == RUNNING && !loop_done) i_cnt <= i_cnt + 1;
```

For nested loops (matmul — path is i → j → pipeline):
```systemverilog
// j_cnt wraps at N, carries into i_cnt
if (j_cnt >= N - 1) j_cnt <= '0;
else                j_cnt <= j_cnt + 1;
if (j_cnt >= N - 1) i_cnt <= i_cnt + 1;
```

For four nested loops (conv2d — path is oh → ow → kh → kw → pipeline):
the same carry chain extends to all four levels.

### 6. Pipeline Stages

Stage depth and op types come directly from `BoundLoop.pipelineDepth` and
`BoundLoop.opBindings`.

**Stage 0 — Load:**
```systemverilog
always_ff @(posedge ap_clk) begin
    pipe_v0    <= (state == RUNNING) && !loop_done;
    store_addr <= i_cnt;                    // delayed address for STORE
    pipe_s0_A  <= mem_A[i_cnt];            // synchronous BRAM read
    pipe_s0_B  <= mem_B[i_cnt];
end
```

**Stage 1 — Compute (FADD or MAC):**
```systemverilog
// FADD path (vadd):
pipe_s1_result <= pipe_s0_A + pipe_s0_B;  // Vivado infers DSP48 FP adder

// MAC path (fir, matmul, conv2d):
if (!pipe_v0) acc_reg <= '0;              // reset at start of dot product
else          acc_reg <= acc_reg + (pipe_s0_A * pipe_s0_B);  // Vivado infers DSP48
```

**Stage 2 — Store:**
```systemverilog
always_ff @(posedge ap_clk) begin
    pipe_v2 <= pipe_v1;
    if (pipe_v1) mem_C[store_addr] <= pipe_s1_result;  // BRAM write
end
```

The `store_addr` register is captured at Stage 0 alongside the load, so by
Stage 2 it correctly addresses the element that was loaded two cycles earlier.

### 7. AXI4-Master Burst Engines

**Read engine** (fills BRAM from DDR before pipeline starts):
```
AR_IDLE → (state==RUNNING) → issue araddr + arlen → AR_ADDR
AR_ADDR → arready          → drop arvalid, assert rready → AR_DATA
AR_DATA → rvalid           → mem_X[beat] = rdata; beat++
          rlast             → drop rready → AR_IDLE
```

**Write engine** (drains BRAM to DDR after pipeline drains):
```
AW_IDLE → (state==DONE)    → issue awaddr + awlen → AW_ADDR
AW_ADDR → awready          → assert wvalid → AW_DATA
AW_DATA → wready           → wdata = mem_X[beat]; beat++; wlast on last
          wlast             → drop wvalid, assert bready → AW_RESP
AW_RESP → bvalid           → drop bready → AW_IDLE
```

The write engine starts in `DONE` state — after the pipeline has processed
all `N` elements. The burst length matches the `burstExpr` from Stage 5
(`N`, `N * N`, `length`, `1024`, etc.).

---

## Vivado Primitive Inference

| Behavioural RTL construct | Vivado infers |
|---|---|
| `logic [W-1:0] mem [0:D-1]` + sync read/write | RAMB18 or RAMB36 |
| `logic [W-1:0] shreg [0:D-1]` + shift pattern | SRL16E or SRL32E |
| `a + b` (32-bit) | DSP48E2 (or LUT adder for small widths) |
| `acc + (a * b)` pattern | DSP48E2 pre-adder MAC |
| `always_ff` state register | FDRE flip-flop |

Real Vitis HLS uses Xilinx floating-point IPs for true IEEE-754 FP operations.
The emitted `+` and `*` are integer-width operations — to get full IEEE-754
fidelity in Vivado, replace them with `floating_point_v7_1` IP core instances.

---

## Loading into Vivado

1. Open Vivado → Create Project → RTL Project
2. **Add Sources** → Add or create design sources → select `output/*.sv`
3. Set top module to `<func>_top`
4. **Run Elaboration** (`Flow → Open Elaborated Design`) — checks syntax and
   infers port directions
5. **Run Synthesis** — reports resource utilisation (LUT, FF, BRAM, DSP)
6. To simulate: add a testbench that drives `ap_clk`, `ap_rst_n`, writes
   `ctrl_reg[0]=1` (ap_start), and provides AXI4-Master memory responses

---

## Test Results

### vadd.sv — 347 lines

```
module vadd_top #(DATA_WIDTH=32, ADDR_WIDTH=64, N=1024)
Ports : ap_clk, ap_rst_n
        s_axilite_* (17 signals)
        m_axi_A_*   READ  (11 signals)
        m_axi_B_*   READ  (11 signals)
        m_axi_C_*   WRITE (14 signals)
BRAMs : mem_A[N], mem_B[N], mem_C[N]
Pipeline: stage-0 BRAM read → stage-1 FADD → stage-2 BRAM write
AXI-Lite: ctrl@0x00, N@0x10, A_base@0x18, B_base@0x20, C_base@0x28
```

### fir_filter.sv

```
module fir_filter_top #(DATA_WIDTH=32, ADDR_WIDTH=64, length=1024)
ShiftReg : shreg_in_signal[SR_DEPTH_in_signal]  (depth = TAPS = 16)
BRAM     : mem_out_signal[length], mem_coeffs[TAPS]
Pipeline : stage-0 ShiftReg + BRAM reads → stage-1 MAC → stage-2 BRAM write
AXI-Lite : ctrl@0x00, in_signal_base@0x10, out_signal_base@0x18
```

### matmul.sv

```
module matmul_top #(DATA_WIDTH=32, ADDR_WIDTH=64, N=1024)
BRAMs    : mem_C[N*N], mem_A[N*N], mem_B[N*N]
Counters : i_cnt (outer) → j_cnt (inner, pipelined) with carry
Pipeline : stage-0 BRAM reads → stage-1 MAC (acc_reg) → stage-2 BRAM write
AXI-Lite : ctrl@0x00, A_base@0x10, B_base@0x18, C_base@0x20
```

### conv2d.sv

```
module conv2d_top #(DATA_WIDTH=32, ADDR_WIDTH=64)
Note     : loop bounds 30/30/3/3 are numeric — no symbolic parameters
Counters : oh_cnt → ow_cnt → kh_cnt → kw_cnt (4-level carry chain)
Pipeline : stage-0 placeholder loads → stage-1 MAC (Stage 1 cannot see 2D accesses)
AXI-Lite : ctrl@0x00, input_base@0x10, kernel_base@0x18, output_base@0x20
AXI ports: input/kernel/output emit both read AND write channels (direction=Unknown)
```

---

## Known Limitations

| Limitation | Root cause | Fix |
|---|---|---|
| `+` and `*` are integer ops, not IEEE-754 | Stage 6 emits behavioural RTL | Instantiate `floating_point_v7_1` Xilinx IP |
| conv2d stage-0 loads are placeholders (`32'h0`) | Stage 1 cannot parse 2D subscripts | Extend Stage 1 to walk multi-dimensional indexing |
| Write engine starts in DONE not mid-pipeline | Simplification: assumes all pipeline output is buffered in BRAM first | Interleave write engine with pipeline for streaming output |
| Accumulator reset logic uses `!pipe_v0` | Correct only for single-iteration pipelines; multi-iteration (k-loop unrolled) needs explicit k-counter reset | Add k-counter and reset on `k_cnt == 0` |
