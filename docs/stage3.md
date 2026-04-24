# Stage 3 — Scheduling (ASAP / ALAP)

Stage 3 takes the `AffineContext` produced by Stage 2 and assigns every inferred
hardware operation to a specific clock cycle. It runs two passes per pipelined loop:

1. **ASAP** (As Soon As Possible) — forward pass, gives minimum latency
2. **ALAP** (As Late As Possible) — backward pass, gives scheduling freedom (slack)

Stage 3 has no dependency on Clang. Its only input is the `AffineContext` IR from Stage 2.

---

## Files That Belong to Stage 3

```
FluxHLS/
└── stage3/
    ├── ScheduleContext.h    ← Stage 3 IR data structs (no logic)
    ├── Scheduler.h          ← public API
    └── Scheduler.cpp        ← op inference + ASAP + ALAP + pretty printer
```

---

## File-by-File Explanation

### `stage3/ScheduleContext.h` — Stage 3 IR, pure data

**`OpKind` enum** — the six operation types Stage 3 can infer from access patterns:

| OpKind | What it represents | Hardware unit |
|---|---|---|
| `Init` | Initialise the scalar accumulator to 0.0 | Register write |
| `Load` | Read one element from an array into a register | BRAM port / DDR |
| `Store` | Write one element from a register to an array | BRAM port / DDR |
| `FAdd` | Floating-point add: `A + B` (no accumulation) | DSP48 |
| `FMul` | Floating-point multiply: `A * B` | DSP48 |
| `MAC` | Multiply-accumulate: `acc += A * B` (loop-carried dep on acc) | DSP48 |

**`ScheduledOp`** — one operation with its cycle assignment:
```cpp
struct ScheduledOp {
    std::string      id;        // "load_A", "mac", "store_C", etc.
    OpKind           kind;
    std::string      array;     // array touched (empty for Init, FAdd, MAC)
    std::string      indexExpr; // index expression from Stage 1
    int              latency;   // cycles until result is ready
    int              asap;      // earliest start cycle (ASAP result)
    int              alap;      // latest start cycle without delaying output
    int              slack;     // ALAP - ASAP: 0 = on the critical path
    std::vector<int> depIds;    // indices of ops this one must wait for
};
```

**`ScheduledLoop`** — adds schedule fields to the AffineLoop:
```cpp
struct ScheduledLoop {
    // copied from AffineLoop: id, var, lo, hi, pragmas, requestedII, resMII, recMII, IIFeasible

    int                      pipelineDepth;  // last finish cycle = latency of one iteration
    std::vector<ScheduledOp> ops;            // flat op list in topological order
    std::vector<std::unique_ptr<ScheduledLoop>> children;
};
```

---

### `stage3/Scheduler.h` — public API

```cpp
void buildSchedule(const AffineContext &affCtx, ScheduleContext &schedCtx);
void dumpSchedule(const ScheduleContext &schedCtx);
```

---

### `stage3/Scheduler.cpp` — full implementation

Four parts:

---

#### Part 1 — Latency table

```cpp
static int latencyOf(OpKind k) {
    // simplified model: all ops = 1 cycle
    // realistic DSP48 values: FAdd=4, FMul=3, MAC=6
}
```

All latencies are set to 1 (simplified educational model). To switch to realistic
Xilinx DSP48 floating-point latencies, only this one table needs to change —
the ASAP/ALAP algorithm itself is unchanged.

---

#### Part 2 — Operation inference (`inferOps`)

Stage 1 and Stage 2 only track **array accesses**, not scalar ops like `+` or `*`.
Stage 3 infers the actual hardware operations from the access patterns:

| Stage 2 patterns in scope | Inferred ops (in topological order) |
|---|---|
| REDUCTION write + N reads | `Init`, `Load×N`, `MAC`, `Store` |
| reads-only (scalar accumulator heuristic) | `Init`, `Load×N`, `MAC` (no Store — lives in parent) |
| SEQUENTIAL/STRIDED reads + non-Reduction write | `Load×N`, `FAdd`, `Store` |

The result is always in topological order: `Init < Loads < Compute < Store`.
This is required so the ASAP forward pass can process ops left-to-right.

---

#### Part 3 — ASAP and ALAP passes

**ASAP (forward pass):**
```
for each op in topological order:
    op.asap = max(dep.asap + dep.latency  for all deps)
```
Each op starts as early as all its inputs allow. The first op with no dependencies
gets `asap = 0`.

**Pipeline depth:**
```
depth = max(op.asap + op.latency  for all ops)
```
The cycle at which the last op finishes. This is the latency of one pipeline iteration.

**ALAP (backward pass):**
```
initialise: op.alap = depth - op.latency  (latest possible finish)

for each op in reverse topological order:
    for each consumer j of this op:
        op.alap = min(op.alap,  consumer.alap - op.latency)
```
Each op is pushed as late as possible without delaying any op that needs its result.

**Slack:**
```
op.slack = op.alap - op.asap
```
- `slack = 0` → op is on the **critical path** — cannot be moved at all
- `slack > 0` → op has scheduling freedom (useful for Stage 4 resource sharing)

With the simplified latency=1 model, all ops in the current test suite have
`slack = 0` because every op is part of the single critical chain. Non-zero slack
appears when realistic latencies are used (e.g. FAdd=4 creates slack on parallel loads).

---

## What Stage 3 Produces — Actual Output for Each Test

### `test/vadd.cpp`

```
L0  line 18  for i in [0, N)  [PIPELINE II=1]

  ── Schedule (ASAP / ALAP) ─────────────────────
    Op       Description                 ASAP  ALAP  Slack  Cycles
    ----------------------------------------------------------
    LOAD   A[i]                           0     0      0  [0, 0]
    LOAD   B[i]                           0     0      0  [0, 0]
    FADD   A + B                          1     1      0  [1, 1]
    STORE  C[i]                           2     2      0  [2, 2]

  Pipeline depth = 3 cycles   II = 1  ✓ feasible
```

**What this tells downstream stages:**
- Pattern: SEQUENTIAL read×2 + SEQUENTIAL write → inferred as Load, Load, FAdd, Store
- No accumulator (C[i] is SEQUENTIAL, not REDUCTION — no child loop)
- All 4 ops on the critical path: `slack = 0` for every op
- Pipeline depth 3: iteration 0 finishes cycle 2, iteration 1 starts cycle 1
- Stage 4 needs 1 adder, 2 memory read ports, 1 memory write port

---

### `test/fir.cpp`

```
L0  line 26  for i in [0, length)  [PIPELINE II=1]

  ── Schedule (ASAP / ALAP) ─────────────────────
    Op       Description                 ASAP  ALAP  Slack  Cycles
    ----------------------------------------------------------
    INIT   acc = 0.0                      0     0      0  [0, 0]
    LOAD   in_signal[i - t]               0     0      0  [0, 0]
    LOAD   coeffs[t]                      0     0      0  [0, 0]
    MAC    acc += A * B                   1     1      0  [1, 1]
    STORE  out_signal[i]                  2     2      0  [2, 2]

  Pipeline depth = 3 cycles   II = 1  ✓ feasible

  L0.L1  line 29  for t in [0, TAPS)  [UNROLL factor=4]
```

**What this tells downstream stages:**
- `out_signal[i]` is REDUCTION → accumulator pattern → Init + MAC + Store
- `in_signal[i-t]` is SLIDING_WINDOW, `coeffs[t]` is SEQUENTIAL → both Load ops
- Init, both Loads all at cycle 0 — they have no mutual dependencies
- MAC waits for all three (cycles 0→1 finish), Store waits for MAC
- Stage 4 needs 1 accumulator register, 1 DSP48 for MAC, 1 write port

---

### `test/matmul.cpp`

```
L0.L1  line 10  for j in [0, N)  [PIPELINE II=1]

    ── Schedule (ASAP / ALAP) ─────────────────────
      Op       Description                 ASAP  ALAP  Slack  Cycles
      ----------------------------------------------------------
      INIT   acc = 0.0                      0     0      0  [0, 0]
      LOAD   A[i * N + k]                   0     0      0  [0, 0]
      LOAD   B[k * N + j]                   0     0      0  [0, 0]
      MAC    acc += A * B                   1     1      0  [1, 1]
      STORE  C[i * N + j]                   2     2      0  [2, 2]

    Pipeline depth = 3 cycles   II = 1  ✓ feasible

    L0.L1.L2  line 13  for k in [0, N)
```

**What this tells downstream stages:**
- `C[i*N+j]` is REDUCTION (j-loop write, k-loop is child — k-invariant store)
- `A[i*N+k]` is SEQUENTIAL (stride=1), `B[k*N+j]` is STRIDED (stride=N)
- Identical structure to fir: Init/Load/Load at cycle 0, MAC at cycle 1, Store at cycle 2
- Stage 4: must partition B into N BRAM banks (STRIDED) so the k-loop's N load
  operations don't fight over one memory port — the schedule tells it this load
  lands at cycle 0 and is on the critical path

---

### `test/conv2d.cpp`

```
L0.L1.L2.L3  line 34  for kw in [0, KW)  [PIPELINE II=1]

          ── Schedule (ASAP / ALAP) ─────────────────────
            Op       Description                 ASAP  ALAP  Slack  Cycles
            ----------------------------------------------------------
            INIT   acc = 0.0                      0     0      0  [0, 0]
            MAC    acc += A * B                   1     1      0  [1, 1]

          Pipeline depth = 2 cycles   II = 1  ✓ feasible
```

**What this tells downstream stages:**
- The kw-loop has no array accesses visible to Stage 1 (`sum` is scalar, and the
  2D array subscripts `input[oh+kh][ow+kw]` are not captured in the innermost index).
  Stage 2's reads-only heuristic → `isAccumulator = true` → Stage 3 emits Init + MAC.
- No Load or Store ops: Stage 3 faithfully reflects what Stage 1/2 could see.
  Stage 4 will need to handle the memory access separately via the parent ow-loop's
  write to `output[oh][ow]`.
- Pipeline depth = 2: the shortest of all four test cases, since there are no
  visible Load or Store ops in the innermost pipeline.

---

## Latency Model: Simplified vs. Realistic

Stage 3 uses a **simplified latency model** (all ops = 1 cycle) for clarity.
The single `latencyOf()` table can be updated to realistic DSP48 values at any time:

| Op | Simplified (current) | Realistic (DSP48 float) |
|---|---|---|
| Load | 1 | 1 (BRAM) / 50–200 (DDR) |
| Store | 1 | 1 (BRAM) |
| FAdd | 1 | 4 |
| FMul | 1 | 3 |
| MAC | 1 | 6 |

With realistic latencies, the pipeline depth for vadd becomes:
```
Cycle 0:    Load A[i],  Load B[i]
Cycle 1–4:  FAdd (4-cycle latency)
Cycle 5:    Store C[i]
Pipeline depth = 6 cycles
```
And non-zero slack would appear: the two parallel Loads both finish at cycle 1,
but FAdd doesn't start until cycle 0 — so both Loads would have `slack = 0` still,
but operations that run in parallel with long-latency ops gain slack.

---

## What Stage 3 Gives to Every Later Stage

| What Stage 3 produces | Who consumes it |
|---|---|
| `op.asap` per op | Stage 4 — binds ops to hardware units in cycle order |
| `op.alap` per op | Stage 4 — two ops with overlapping [asap, alap] windows can share a unit |
| `op.slack` per op | Stage 4 — zero-slack ops are on the critical path; bind them first |
| `pipelineDepth` per loop | Stage 5 — used to compute total kernel latency for AXI handshake timing |
| Op kind + array + index | Stage 4 — determines which hardware unit (DSP48 / BRAM / LUT) each op maps to |

---

## How Stage 3 Connects to the Pipeline

```
AffineContext (Stage 2 output)
      │
      ▼  buildSchedule()
ScheduleContext
  ├── ScheduledFunction
  │     ├── ScheduledLoop  (pipelineDepth, ops with ASAP/ALAP/slack)
  │     │     └── ScheduledLoop  (children, recursively)
  │     └── ...
  └── ...
      │
      ▼  dumpSchedule()      (Stage 3 output to stdout)
      │
      ▼  Stage 4 — Resource Binding  (consumes ScheduleContext directly)
```

Stage 3 adds no new Clang dependency. It is a pure C++17 transformation over the IR
that Stage 2 built.
