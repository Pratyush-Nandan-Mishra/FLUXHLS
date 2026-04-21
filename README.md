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
        ▼  Stage 3 — Scheduling                Planned
   every op assigned a clock cycle  (ASAP / ALAP / SDC)
        │
        ▼  Stage 4 — Resource binding          Planned
   ops mapped to DSP48 / LUT / BRAM instances
        │
        ▼  Stage 5 — Interface synthesis       Planned
   AXI4-Master / AXI4-Lite / ap_ctrl wrappers
        │
        ▼  Stage 6 — RTL emission              Planned
   synthesisable SystemVerilog (.sv)
```

Deep-dive docs for each completed stage:
- [docs/stage1.md](docs/stage1.md) — Frontend: pragma extraction + AST walk
- [docs/stage2.md](docs/stage2.md) — Affine analysis: access patterns, DDG, II feasibility

---

## Project Structure

```
FluxHLS/
├── CMakeLists.txt
├── main.cpp                  ← CLI entry point (runs Stage 1 then Stage 2)
├── stage1/                   ← Stage 1: Frontend
│   ├── Pragma.h
│   ├── HLSContext.h
│   ├── Frontend.h / Frontend.cpp
│   └── Dumper.h  / Dumper.cpp
├── stage2/                   ← Stage 2: Affine Analysis
│   ├── AffineContext.h
│   ├── AffineAnalysis.h
│   └── AffineAnalysis.cpp
├── docs/
│   ├── stage1.md
│   └── stage2.md
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

Each invocation runs Stage 1 then Stage 2 and prints both outputs.

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
