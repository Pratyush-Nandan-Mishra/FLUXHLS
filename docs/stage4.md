# Stage 4 — Resource Binding

Stage 4 takes both the `AffineContext` (Stage 2) and the `ScheduleContext` (Stage 3)
and produces a `BindingContext` — the first stage to consume two prior stages simultaneously.

It answers two questions for every pipelined loop:

1. **What hardware resource does each array live in?** (BRAM / ShiftRegister / Register)
2. **Which specific unit executes each scheduled op?** (DSP48_N, BRAM_X.portN, acc_reg)

---

## Files That Belong to Stage 4

```
FluxHLS/
└── stage4/
    ├── BindingContext.h    ← Stage 4 IR data structs (no logic)
    ├── Binder.h            ← public API
    └── Binder.cpp          ← 5-rule binding table + op assignment + pretty printer
```

---

## File-by-File Explanation

### `stage4/BindingContext.h` — Stage 4 IR, pure data

**`ResourceKind` enum** — the four hardware resource types Stage 4 assigns:

| ResourceKind | Xilinx primitive | Latency | Notes |
|---|---|---|---|
| `BRAM` | RAMB36/18 | 1 cycle | Dual-port; 1 read + 1 write simultaneously |
| `ShiftRegister` | SRL16E chain | 1 cycle | Fixed depth; used for sliding-window buffers |
| `Register` | FDRE flip-flop | 0 cycles | Scalar accumulators and small fully-partitioned arrays |
| `DSP48` | DSP48E2 | 1 cycle (simplified) | FAdd, FMul, MAC |

**`ArrayBinding`** — the binding decision for one array:
```cpp
struct ArrayBinding {
    std::string   array;       // array name
    ResourceKind  resource;    // chosen hardware resource
    std::string   configStr;   // "1 bank", "N banks cyclic", "depth=TAPS"
    int           readPorts;   // simultaneous reads per pipeline iteration
    int           writePorts;  // simultaneous writes per pipeline iteration
    AccessPattern pattern;     // Stage 2 pattern that drove the decision
    std::string   reason;      // one-line rationale
};
```

**`OpBinding`** — the binding for one scheduled operation:
```cpp
struct OpBinding {
    std::string  opId;   // e.g. "load_A", "mac", "store_C"
    OpKind       kind;   // operation category
    std::string  unit;   // "DSP48_0", "BRAM_A.port0", "Register_C_acc", etc.
    int          cycle;  // ASAP start cycle from Stage 3
    std::string  array;  // array touched (empty for compute ops)
    std::string  desc;   // human-readable description
};
```

**`BoundLoop`** — adds binding tables and resource counts to the loop node:
```cpp
struct BoundLoop {
    // copied from ScheduledLoop: id, var, lo, hi, pragmas, II fields, pipelineDepth

    std::vector<ArrayBinding> arrayBindings; // one per distinct array in pipeline scope
    std::vector<OpBinding>    opBindings;    // one per ScheduledOp

    int numDSP48s;    // DSP48 blocks used
    int numBRAMs;     // distinct BRAM arrays (each may have multiple banks)
    int numShiftRegs; // SRL16 shift-register chains
    int numRegisters; // accumulator registers + small-array register partitions
};
```

---

### `stage4/Binder.h` — public API

```cpp
void buildBindings(const AffineContext   &affCtx,
                   const ScheduleContext &schedCtx,
                   BindingContext        &bindCtx);

void dumpBindings(const BindingContext &bindCtx);
```

---

### `stage4/Binder.cpp` — full implementation

Six parts:

---

#### Part 1 — Utility helpers

**`gatherAccesses(loop, out)`** — same as Stage 3's helper: recursively collects all
`AffineAccess` pointers from the pipelined loop and all its descendants. Needed because
the REDUCTION write may live on the j-loop while the reads live on the k-loop child.

**`parseArraySize(type)`** — parses a C type string for fixed numeric dimensions:
- `"float [3][3]"` → 9
- `"float [16]"` → 16
- `"float *"` → −1 (pointer, unknown)

Returns −1 if any dimension is a variable (e.g. `"float [N]"`).

**`findSlidingDepth(indexExpr, pipelineLoop)`** — for a SLIDING_WINDOW access like
`in_signal[i - t]`, finds the child loop whose variable appears in the index expression
(here `t`) and returns its upper bound (`TAPS`). This becomes the shift-register depth.

---

#### Part 2 — Array binding (5-rule table)

`makeArrayBinding()` applies rules in priority order — highest priority wins:

| Rule | Condition | Resource | Config |
|---|---|---|---|
| 1 | Fixed param type, product ≤ 64 elements | Register | complete partition |
| 2 | SLIDING_WINDOW access | ShiftRegister | depth = inner loop bound |
| 3 | STRIDED access (stride = N) | BRAM | N banks, cyclic partition |
| 4 | REDUCTION write | BRAM | 1 bank (accumulator lives in a Register) |
| 5 | SEQUENTIAL (default) | BRAM | 1 bank |

**Why these rules map to FPGA primitives:**

- **Rule 1 (Register)**: A 9-element array (like `kernel[3][3]`) fully unrolled into
  flip-flops allows all 9 elements to be read simultaneously with zero latency and no
  port conflicts. Vitis HLS applies this automatically for arrays ≤ a threshold.

- **Rule 2 (ShiftRegister)**: `in_signal[i - t]` requires simultaneous access to
  elements `i, i−1, …, i−TAPS+1` in the same cycle. A BRAM can only serve two ports;
  an SRL16 chain naturally presents a sliding window at every tap. Each `SRL16E` holds
  16 1-bit values; 32 of them per float implement a 32-deep shift register.

- **Rule 3 (BRAM N-bank cyclic)**: `B[k*N+j]` with stride N means the k-loop reads
  column j of matrix B, hitting elements 0, N, 2N, … — all in the same BRAM bank if
  B is stored flat. Cyclic partitioning (`element i → bank i%N`) guarantees each
  successive element lands in a different bank, eliminating the memory bottleneck.

- **Rule 4 (BRAM for REDUCTION)**: The array itself (`out_signal`, `C`) needs BRAM
  because it has N elements across all outer loop iterations. The accumulator within
  one iteration is a separate Register (counted in `numRegisters`, created by the
  Init op).

- **Rule 5 (BRAM default)**: SEQUENTIAL access (stride-1) streams through a single
  BRAM with one read or write per cycle — optimal for a pipelined loop with II=1.

---

#### Part 3 — Op binding

`bindOps()` maps each `ScheduledOp` to a hardware unit:

| Op kind | Unit assigned | Notes |
|---|---|---|
| `Init` | `Register_<array>_acc` | Named after REDUCTION target; "acc_reg" if none visible |
| `Load` | `<prefix>_<array>.portN` | Port index increments per array (`BRAM_A.port0`, `BRAM_A.port1`, …) |
| `Store` | `<prefix>_<array>.port0` | Always port0 — BRAM write port |
| `FAdd` / `FMul` / `MAC` | `DSP48_<N>` | Sequential counter within the loop |

The `<prefix>` comes from the array's `ResourceKind`:
- BRAM → `BRAM_<array>`
- ShiftRegister → `ShiftReg_<array>`
- Register → `Reg_<array>`

---

#### Part 4 — Resource counting

`countResources()` tallies after binding:
- `numDSP48s` — one per FAdd/FMul/MAC op binding
- `numRegisters` — one per Init op (accumulator) + one per small-array Register binding
- `numBRAMs` — one per distinct array mapped to BRAM (regardless of bank count)
- `numShiftRegs` — one per distinct array mapped to ShiftRegister

---

#### Part 5 — Build BindingContext

`bindLoop()` walks the `AffineLoop` and `ScheduledLoop` trees **in parallel by index**.
Both trees mirror the same `HLSLoop` structure from Stage 1, so child index matching is
always safe. Only pipelined loops (`requestedII >= 0`) get array and op bindings;
non-pipelined loops carry the same children but no binding tables.

---

## What Stage 4 Produces — Actual Output for Each Test

### `test/vadd.cpp`

```
L0  line 18  for i in [0, N)  [PIPELINE II=1]

  ── Array Bindings ────────────────────────────────────────────
    Array             Resource       Config            Ports     Pattern
    ------------------------------------------------------------------------
    C                 BRAM           1 bank            0R 1W     SEQUENTIAL
    A                 BRAM           1 bank            1R 0W     SEQUENTIAL
    B                 BRAM           1 bank            1R 0W     SEQUENTIAL

  ── Op Bindings ───────────────────────────────────────────────
    Op     Description                   Unit                        Cycle
    ------------------------------------------------------------------------
    LOAD   A[i]                          BRAM_A.port0                0
    LOAD   B[i]                          BRAM_B.port0                0
    FADD   A + B                         DSP48_0                     1
    STORE  C[i]                          BRAM_C.port0                2

  Resources: 1 DSP48  3 BRAM  0 ShiftReg  0 Reg
  Pipeline depth = 3 cycles   II = 1  ✓ feasible
```

**What this tells downstream stages:**
- 3 independent BRAMs, each with 1 active port per cycle → no port conflicts
- 1 DSP48 for the FAdd — the only compute resource
- No accumulator register (C is SEQUENTIAL, not REDUCTION — it's a direct write)
- Stage 5 will wrap A, B with AXI4-Master read channels; C with an AXI4-Master write channel

---

### `test/fir.cpp`

```
L0  line 26  for i in [0, length)  [PIPELINE II=1]

  ── Array Bindings ────────────────────────────────────────────
    Array             Resource       Config            Ports     Pattern
    ------------------------------------------------------------------------
    out_signal        BRAM           1 bank            0R 1W     REDUCTION
    in_signal         ShiftRegister  depth=TAPS        1R 0W     SLIDING_WINDOW
    coeffs            BRAM           1 bank            1R 0W     SEQUENTIAL

  ── Op Bindings ───────────────────────────────────────────────
    Op     Description                   Unit                        Cycle
    ------------------------------------------------------------------------
    INIT   acc = 0.0                     Register_out_signal_acc     0
    LOAD   in_signal[i - t]              ShiftReg_in_signal.port0    0
    LOAD   coeffs[t]                     BRAM_coeffs.port0           0
    MAC    acc += A * B                  DSP48_0                     1
    STORE  out_signal[i]                 BRAM_out_signal.port0       2

  Resources: 1 DSP48  2 BRAM  1 ShiftReg  1 Reg
  Pipeline depth = 3 cycles   II = 1  ✓ feasible
```

**What this tells downstream stages:**
- `in_signal` maps to a 16-deep SRL16 chain (depth=TAPS=16): a new sample shifts in each
  cycle, making `in_signal[i−t]` for all t simultaneously accessible
- `out_signal` is BRAM (N elements of final output) with a separate flip-flop accumulator
  (`Register_out_signal_acc`) that holds the running sum until the Store writes it back
- `coeffs` is a 16-element local array — Stage 4 classifies it as BRAM (SEQUENTIAL).
  The ARRAY_PARTITION pragma in the source would split it into 4 banks (Stage 4 currently
  uses access-pattern rules; pragma-driven overrides are a planned Stage 5 feature)

---

### `test/matmul.cpp`

```
L0.L1  line 10  for j in [0, N)  [PIPELINE II=1]

    ── Array Bindings ────────────────────────────────────────────
      Array             Resource       Config            Ports     Pattern
      ------------------------------------------------------------------------
      C                 BRAM           1 bank            0R 1W     REDUCTION
      A                 BRAM           1 bank            1R 0W     SEQUENTIAL
      B                 BRAM           N banks cyclic    1R 0W     STRIDED

    ── Op Bindings ───────────────────────────────────────────────
      Op     Description                   Unit                        Cycle
      ------------------------------------------------------------------------
      INIT   acc = 0.0                     Register_C_acc              0
      LOAD   A[i * N + k]                  BRAM_A.port0                0
      LOAD   B[k * N + j]                  BRAM_B.port0                0
      MAC    acc += A * B                  DSP48_0                     1
      STORE  C[i * N + j]                  BRAM_C.port0                2

    Resources: 1 DSP48  3 BRAM  0 ShiftReg  1 Reg
    Pipeline depth = 3 cycles   II = 1  ✓ feasible

    L0.L1.L2  line 13  for k in [0, N)
```

**What this tells downstream stages:**
- `B[k*N+j]` is STRIDED (stride=N) → N-bank cyclic partition: element `k*N+j` → bank
  `(k*N+j) % N = j`. All k-loop iterations (each with different k) access bank j, so
  there is only ever one outstanding request to that bank per cycle — no conflict
- `Register_C_acc` holds the running partial sum `C[i][j]` across the entire k-loop;
  only at k=N-1 does the Store write it to `BRAM_C`
- Stage 5 needs to generate an AXI burst of N elements for each row of A and column of B

---

### `test/conv2d.cpp`

```
L0.L1.L2.L3  line 34  for kw in [0, KW)  [PIPELINE II=1]

          ── Op Bindings ───────────────────────────────────────────────
            Op     Description                   Unit                        Cycle
            ------------------------------------------------------------------------
            INIT   acc = 0.0                     acc_reg                     0
            MAC    acc += A * B                  DSP48_0                     1

          Resources: 1 DSP48  0 BRAM  0 ShiftReg  1 Reg
          Pipeline depth = 2 cycles   II = 1  ✓ feasible
```

**What this tells downstream stages:**
- No array bindings: Stage 1 cannot capture the 2D subscripts `input[oh+kh][ow+kw]`
  and `kernel[kh][kw]` in the innermost kw-loop. Stage 4 faithfully reflects this.
- `acc_reg` is an anonymous accumulator (no REDUCTION write is visible to Stage 4).
  The actual `output[oh][ow]` write lives in the ow-loop scope and will be bound
  in a future pass once Stage 1 captures nested subscripts.
- `kernel[3][3]` has type `float[3][3]` (Clang resolves the #defines) — 9 elements ≤ 64,
  which would qualify for Register partition under Rule 1. However, since the kw-loop's
  AffineAccesses are empty, kernel never appears in the binding; the Register rule for
  it would only apply if Stage 1 were extended to capture 2D subscripts.

---

## The 5-Rule Binding Table at a Glance

```
Stage 2 AccessPattern      ──▶  Stage 4 ResourceKind
──────────────────────────────────────────────────────
small fixed param (≤64)    ──▶  Register        (complete partition, zero-latency)
SLIDING_WINDOW             ──▶  ShiftRegister   (SRL16 chain, depth = inner loop bound)
STRIDED  (stride N)        ──▶  BRAM N banks    (cyclic partition, one bank per column)
REDUCTION                  ──▶  BRAM 1 bank     (+ implicit Register for accumulator)
SEQUENTIAL                 ──▶  BRAM 1 bank     (streaming access, one port per cycle)
```

---

## What Stage 4 Gives to Every Later Stage

| What Stage 4 produces | Who consumes it |
|---|---|
| `ArrayBinding.resource` per array | Stage 5 — interface synthesis wraps BRAMs with AXI ports, Registers need no interface |
| `ArrayBinding.configStr` (N banks) | Stage 5 — each BRAM bank needs its own AXI burst channel |
| `OpBinding.unit` per op | Stage 6 — RTL emission instantiates exactly the named primitives |
| `numDSP48s`, `numBRAMs` per loop | Stage 5 — reports resource utilisation estimate |
| `cycle` per op | Stage 6 — drives `always_ff` clock-enable logic for each pipeline stage register |

---

## How Stage 4 Connects to the Pipeline

```
AffineContext  (Stage 2 output)   ─────┐
                                        ▼  buildBindings()
ScheduleContext (Stage 3 output)  ─────▶  BindingContext
                                        │  ├── BoundFunction
                                        │  │     ├── BoundLoop  (arrayBindings, opBindings, counts)
                                        │  │     │     └── BoundLoop  (children, recursively)
                                        │  │     └── …
                                        │  └── …
                                        │
                                        ▼  dumpBindings()   (Stage 4 output to stdout)
                                        │
                                        ▼  Stage 5 — Interface Synthesis  (consumes BindingContext)
```

Stage 4 adds no new Clang dependency. It is a pure C++17 transformation over the IRs
that Stages 2 and 3 built.
