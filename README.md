# FluxHLS — Mini High-Level Synthesis Compiler

An educational HLS compiler modelled after Xilinx Vitis HLS.
The goal is to understand **how Vitis converts C++ into FPGA RTL** by
replicating each stage of its pipeline from first principles.

```
C/C++ + #pragma HLS
        │
        ▼  Stage 1 — Frontend            ✅ Done
   Clang AST  +  extracted pragma map
        │
        ▼  Stage 2 — Affine IR / DDG     Planned
   normalised loop IR + data-dependence graph
        │
        ▼  Stage 3 — Scheduling          Planned
   every op assigned a clock cycle  (ASAP / ALAP / SDC)
        │
        ▼  Stage 4 — Resource binding    Planned
   ops mapped to DSP48 / LUT / BRAM instances
        │
        ▼  Stage 5 — Interface synthesis Planned
   AXI4-Master / AXI4-Lite / ap_ctrl wrappers
        │
        ▼  Stage 6 — RTL emission        Planned
   synthesisable SystemVerilog (.sv)
```

For a detailed explanation of Stage 1 — what it does, how it works, and what
each file is responsible for — see [docs/stage1.md](docs/stage1.md).

---

## Project Structure

```
FluxHLS/
├── CMakeLists.txt
├── main.cpp              ← CLI entry point
├── stage1/               ← Stage 1: Frontend (pragma extraction + AST walk)
│   ├── Pragma.h
│   ├── HLSContext.h
│   ├── Frontend.h / Frontend.cpp
│   └── Dumper.h  / Dumper.cpp
├── docs/
│   └── stage1.md         ← Stage 1 deep-dive
├── scripts/
│   └── setup_ubuntu.sh   ← one-command Ubuntu setup
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

### Windows

```powershell
# Install LLVM from https://releases.llvm.org/ (tick "Add LLVM to PATH")
# Open a Visual Studio 2022 Developer Command Prompt
cmake -S . -B build
cmake --build build --config Release
```

### Manual Linux build

```bash
sudo apt install -y llvm-17-dev libclang-17-dev clang-17 cmake build-essential
cmake -S . -B build -DLLVM_INSTALL_DIR=/usr/lib/llvm-17
cmake --build build --parallel
```

---

## Run

```bash
# Linux / macOS
./build/fluxhls test/vadd.cpp
./build/fluxhls test/fir.cpp
./build/fluxhls test/matmul.cpp
./build/fluxhls test/conv2d.cpp

# Windows
./build/Release/fluxhls.exe test/vadd.cpp

# Extra compiler flags after --
./build/fluxhls test/matmul.cpp -- -I/some/include
```
