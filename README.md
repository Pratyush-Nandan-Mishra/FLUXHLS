# FluxHLS — Mini High-Level Synthesis Compiler

An educational HLS compiler modelled after Xilinx Vitis HLS.
The goal is to understand **how Vitis converts C++ into FPGA RTL** by
replicating each stage of its pipeline from first principles.

```
C/C++ + #pragma HLS
        │
        ▼  Stage 1 — Frontend                  ✅ Done
   Clang AST  +  extracted pragma map
        │
        ▼  Stage 2 — Affine Analysis / DDG     ✅ Done
   access patterns + dependence graph + II feasibility
        │
        ▼  Stage 3 — Scheduling                ✅ Done
   every op assigned a clock cycle  (ASAP / ALAP)
        │
        ▼  Stage 4 — Resource Binding          ✅ Done
   arrays → BRAM / ShiftReg / Register
   ops   → DSP48 / BRAM port / acc register
        │
        ▼  Stage 5 — Interface Synthesis       ✅ Done
   AXI4-Lite register map + AXI4-Master burst ports
        │
        ▼  Stage 6 — RTL emission              Planned
   synthesisable SystemVerilog (.sv)
```

Deep-dive docs for each completed stage:
- [docs/stage1.md](docs/stage1.md) — Frontend: pragma extraction + AST walk
- [docs/stage2.md](docs/stage2.md) — Affine analysis: access patterns, DDG, II feasibility
- [docs/stage3.md](docs/stage3.md) — Scheduling: ASAP/ALAP, op inference, pipeline depth
- [docs/stage4.md](docs/stage4.md) — Resource binding: 5-rule table, DSP48/BRAM/ShiftReg assignment
- [docs/stage5.md](docs/stage5.md) — Interface synthesis: AXI4-Lite register map, AXI4-Master burst ports

---

## Project Structure

```
FluxHLS/
├── CMakeLists.txt
├── main.cpp                  ← CLI entry point (runs Stage 1 → Stage 2 → Stage 3 → Stage 4 → Stage 5)
├── stage1/                   ← Stage 1: Frontend
│   ├── Pragma.h
│   ├── HLSContext.h
│   ├── Frontend.h / Frontend.cpp
│   └── Dumper.h  / Dumper.cpp
├── stage2/                   ← Stage 2: Affine Analysis
│   ├── AffineContext.h
│   ├── AffineAnalysis.h
│   └── AffineAnalysis.cpp
├── stage3/                   ← Stage 3: Scheduling
│   ├── ScheduleContext.h
│   ├── Scheduler.h
│   └── Scheduler.cpp
├── stage4/                   ← Stage 4: Resource Binding
│   ├── BindingContext.h
│   ├── Binder.h
│   └── Binder.cpp
├── stage5/                   ← Stage 5: Interface Synthesis
│   ├── InterfaceContext.h
│   ├── InterfaceSynth.h
│   └── InterfaceSynth.cpp
├── docs/
│   ├── stage1.md
│   ├── stage2.md
│   ├── stage3.md
│   ├── stage4.md
│   └── stage5.md
├── scripts/
│   └── setup_ubuntu.sh       ← one-command Ubuntu setup
└── test/
    ├── vadd.cpp
    ├── fir.cpp
    ├── matmul.cpp
    └── conv2d.cpp
```

---

## Get Started

### Clone

```bash
git clone <repo-url>
cd FluxHLS
```

### Ubuntu / Debian (one command)

```bash
chmod +x scripts/setup_ubuntu.sh
./scripts/setup_ubuntu.sh
```

The script installs LLVM, configures, and builds automatically.
Force a specific version with `LLVM_VERSION=17 ./scripts/setup_ubuntu.sh`.

### macOS

```bash
brew install llvm cmake

# Apple Silicon
cmake -S . -B build -DLLVM_INSTALL_DIR=/opt/homebrew/opt/llvm
cmake --build build --parallel

# Intel Mac
cmake -S . -B build -DLLVM_INSTALL_DIR=/usr/local/opt/llvm
cmake --build build --parallel
```

### Manual Linux build

```bash
sudo apt install -y llvm-17-dev libclang-17-dev clang-17 cmake build-essential
cmake -S . -B build -DLLVM_INSTALL_DIR=/usr/lib/llvm-17
cmake --build build --parallel
```

---

## Run

Each invocation runs Stage 1 → Stage 2 → Stage 3 → Stage 4 → Stage 5 and prints all outputs.

```bash
# Linux / macOS
./build/fluxhls test/vadd.cpp
./build/fluxhls test/fir.cpp
./build/fluxhls test/matmul.cpp
./build/fluxhls test/conv2d.cpp

# Extra compiler flags after --
./build/fluxhls test/matmul.cpp -- -I/some/include
```

---

## Verify Stage 2

Run all four test samples and check the key Stage 2 findings:

```bash
# vadd  — no loop-carried dep → RecMII=0, II=1 purely from ResMII
./build/fluxhls test/vadd.cpp
# Expected Stage 2:
#   L0  WRITE C[i] → SEQUENTIAL
#   RecMII = 0  (no loop-carried dependences)
#   Achievable II = 1  ✓ feasible

# fir  — outer loop writes after inner unrolled loop → REDUCTION → RecMII=1
./build/fluxhls test/fir.cpp
# Expected Stage 2:
#   L0   WRITE out_signal[i] → REDUCTION
#   RecMII = 1  (accumulator dep)
#   L0.L1  READ in_signal[i-t] → SLIDING_WINDOW
#          READ coeffs[t]      → SEQUENTIAL

# matmul  — j-loop write is k-invariant → REDUCTION + B is column-major → STRIDED
./build/fluxhls test/matmul.cpp
# Expected Stage 2:
#   L0.L1    WRITE C[i*N+j]   → REDUCTION
#            RecMII = 1  (accumulator dep)
#   L0.L1.L2 READ A[i*N+k]   → SEQUENTIAL   stride=1
#            READ B[k*N+j]   → STRIDED       stride=N

# conv2d  — innermost kw loop has only reads (scalar sum accumulator) → RecMII=1
./build/fluxhls test/conv2d.cpp
# Expected Stage 2:
#   L0.L1.L2.L3  [PIPELINE II=1]
#   RecMII = 1  (reads-only loop → scalar accumulator)
#   Achievable II = 1  ✓ feasible
```

---

## Verify Stage 3

Run all four test samples and check the key Stage 3 schedule tables:

```bash
# vadd  — 2 loads, FAdd, Store → pipeline depth = 3, all ops on critical path (slack=0)
./build/fluxhls test/vadd.cpp
# Expected Stage 3:
#   LOAD  A[i]       ASAP=0  ALAP=0  Slack=0
#   LOAD  B[i]       ASAP=0  ALAP=0  Slack=0
#   FADD  A+B        ASAP=1  ALAP=1  Slack=0
#   STORE C[i]       ASAP=2  ALAP=2  Slack=0
#   Pipeline depth = 3   II = 1  ✓ feasible

# fir  — Init + 2 loads + MAC + Store → pipeline depth = 3
./build/fluxhls test/fir.cpp
# Expected Stage 3:
#   INIT  acc=0.0              ASAP=0  ALAP=0  Slack=0
#   LOAD  in_signal[i-t]       ASAP=0  ALAP=0  Slack=0
#   LOAD  coeffs[t]            ASAP=0  ALAP=0  Slack=0
#   MAC   acc += A * B         ASAP=1  ALAP=1  Slack=0
#   STORE out_signal[i]        ASAP=2  ALAP=2  Slack=0
#   Pipeline depth = 3   II = 1  ✓ feasible

# matmul  — Init + 2 loads + MAC + Store → pipeline depth = 3
./build/fluxhls test/matmul.cpp
# Expected Stage 3:
#   INIT  acc=0.0              ASAP=0  ALAP=0  Slack=0
#   LOAD  A[i*N+k]             ASAP=0  ALAP=0  Slack=0
#   LOAD  B[k*N+j]             ASAP=0  ALAP=0  Slack=0
#   MAC   acc += A * B         ASAP=1  ALAP=1  Slack=0
#   STORE C[i*N+j]             ASAP=2  ALAP=2  Slack=0
#   Pipeline depth = 3   II = 1  ✓ feasible

# conv2d  — reads-only kw loop: Init + MAC (no loads, no Store) → pipeline depth = 2
./build/fluxhls test/conv2d.cpp
# Expected Stage 3:
#   INIT  acc=0.0              ASAP=0  ALAP=0  Slack=0
#   MAC   acc += A * B         ASAP=1  ALAP=1  Slack=0
#   Pipeline depth = 2   II = 1  ✓ feasible
```

---

## Verify Stage 4

Run all four test samples and check the key Stage 4 resource bindings:

```bash
# vadd  — 3 sequential arrays → 3 BRAM, 1 FAdd → 1 DSP48, no accumulator
./build/fluxhls test/vadd.cpp
# Expected Stage 4:
#   A  → BRAM  1 bank   1R 0W   SEQUENTIAL
#   B  → BRAM  1 bank   1R 0W   SEQUENTIAL
#   C  → BRAM  1 bank   0R 1W   SEQUENTIAL
#   LOAD A[i]      → BRAM_A.port0    cycle 0
#   LOAD B[i]      → BRAM_B.port0    cycle 0
#   FADD A + B     → DSP48_0         cycle 1
#   STORE C[i]     → BRAM_C.port0    cycle 2
#   Resources: 1 DSP48  3 BRAM  0 ShiftReg  0 Reg

# fir  — REDUCTION → BRAM, SLIDING_WINDOW → ShiftReg, accumulator → Register
./build/fluxhls test/fir.cpp
# Expected Stage 4:
#   out_signal → BRAM           1 bank       REDUCTION
#   in_signal  → ShiftRegister  depth=TAPS   SLIDING_WINDOW
#   coeffs     → BRAM           1 bank       SEQUENTIAL
#   INIT acc   → Register_out_signal_acc  cycle 0
#   LOAD in_signal[i - t]  → ShiftReg_in_signal.port0  cycle 0
#   LOAD coeffs[t]         → BRAM_coeffs.port0          cycle 0
#   MAC acc += A * B       → DSP48_0                    cycle 1
#   STORE out_signal[i]    → BRAM_out_signal.port0       cycle 2
#   Resources: 1 DSP48  2 BRAM  1 ShiftReg  1 Reg

# matmul  — C:REDUCTION→BRAM, A:SEQUENTIAL→BRAM, B:STRIDED→BRAM N banks
./build/fluxhls test/matmul.cpp
# Expected Stage 4:
#   C  → BRAM  1 bank        REDUCTION
#   A  → BRAM  1 bank        SEQUENTIAL
#   B  → BRAM  N banks cyclic  STRIDED
#   INIT acc   → Register_C_acc   cycle 0
#   LOAD A     → BRAM_A.port0     cycle 0
#   LOAD B     → BRAM_B.port0     cycle 0
#   MAC        → DSP48_0          cycle 1
#   STORE C    → BRAM_C.port0     cycle 2
#   Resources: 1 DSP48  3 BRAM  0 ShiftReg  1 Reg

# conv2d  — no visible array accesses in kw-loop; only Init + MAC
./build/fluxhls test/conv2d.cpp
# Expected Stage 4:
#   (no array bindings — Stage 1 cannot see 2D subscripts in kw-loop)
#   INIT acc   → acc_reg    cycle 0
#   MAC        → DSP48_0    cycle 1
#   Resources: 1 DSP48  0 BRAM  0 ShiftReg  1 Reg
```

---

## Verify Stage 5

Run all four test samples and check the AXI4-Lite register maps and AXI4-Master port tables:

```bash
# vadd  — 3 m_axi arrays, 1 s_axilite scalar (N), ap_ctrl, burst = N per port
./build/fluxhls test/vadd.cpp
# Expected Stage 5:
#   AXI4-Lite: ap_ctrl@0x00, N@0x10, A_base@0x18, B_base@0x20, C_base@0x28
#   A  → READ   32-bit  1 channel  N elems      BRAM 1 bank
#   B  → READ   32-bit  1 channel  N elems      BRAM 1 bank
#   C  → WRITE  32-bit  1 channel  N elems      BRAM 1 bank
#   AXI summary: 2 read port(s), 1 write port(s)

# fir  — in_signal READ/ShiftReg, out_signal WRITE/BRAM, burst = length
./build/fluxhls test/fir.cpp
# Expected Stage 5:
#   AXI4-Lite: ap_ctrl@0x00, in_signal_base@0x10, out_signal_base@0x18
#   in_signal  → READ   32-bit  1 channel  length elems  ShiftRegister depth=TAPS
#   out_signal → WRITE  32-bit  1 channel  length elems  BRAM 1 bank
#   AXI summary: 1 read port(s), 1 write port(s)

# matmul  — B uses N AXI channels (cyclic partition), burst = N * N per array
./build/fluxhls test/matmul.cpp
# Expected Stage 5:
#   AXI4-Lite: ap_ctrl@0x00, A_base@0x10, B_base@0x18, C_base@0x20
#   A  → READ   32-bit  1 channel   N * N elems  BRAM 1 bank
#   B  → READ   32-bit  N channels  N * N elems  BRAM N banks cyclic
#   C  → WRITE  32-bit  1 channel   N * N elems  BRAM 1 bank
#   AXI summary: 2 read port(s), 1 write port(s)

# conv2d  — 3 m_axi arrays with fixed type dims; direction unknown (no binding visible)
./build/fluxhls test/conv2d.cpp
# Expected Stage 5:
#   AXI4-Lite: ap_ctrl@0x00, input_base@0x10, kernel_base@0x18, output_base@0x20
#   input  → ?  32-bit  1 channel  1024 elems  (no binding — Stage 1 limitation)
#   kernel → ?  32-bit  1 channel  9 elems     (no binding — Stage 1 limitation)
#   output → ?  32-bit  1 channel  900 elems   (no binding — Stage 1 limitation)
#   AXI summary: 0 read, 0 write, 3 unknown
```
