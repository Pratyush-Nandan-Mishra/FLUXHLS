# Stage 2 — Affine Analysis

Stage 2 takes the `HLSContext` produced by Stage 1 and enriches it with three
pieces of information that every downstream stage needs:

1. **Access pattern** — how each array is traversed in memory (sequential, strided, sliding window, reduction)
2. **Data dependence graph (DDG)** — which operations depend on which, and whether the dependence is loop-carried
3. **II feasibility** — whether the user's requested `PIPELINE II=N` is actually achievable given the loop's recurrence structure

Stage 2 has no dependency on Clang. Its only input is the `HLSContext` IR that Stage 1 produces.

---

## Files That Belong to Stage 2

```
FluxHLS/
└── stage2/
    ├── AffineContext.h       ← Stage 2 IR data structs (no logic)
    ├── AffineAnalysis.h      ← public API
    └── AffineAnalysis.cpp    ← full implementation + pretty printer
```

---

## File-by-File Explanation

### `stage2/AffineContext.h` — Stage 2 IR, pure data

Defines the data structures that Stage 2 produces. No logic, no Clang dependency.

**`AccessPattern` enum** — the four patterns Stage 2 can assign to any array access:

| Pattern | What it means | Hardware implication |
|---|---|---|
| `Sequential` | Innermost loop var appears with coefficient 1 (e.g. `A[i]`, `A[i*N+k]`) | Safe to stream from DDR — burst transfers work |
| `Strided` | Innermost loop var has a multiplier (e.g. `B[k*N+j]` — k is innermost, stride=N) | Column-major access — needs BRAM partition or local buffer |
| `Reduction` | Write whose index doesn't contain any child loop's variable (e.g. `C[i*N+j]` in j-loop when k-loop is the child) | Accumulator store — value is built up in a register across k iterations then written once |
| `SlidingWindow` | Subtraction pattern with an outer loop variable (e.g. `in[i-t]`) | Sliding window — candidates for line buffer insertion |

**`AffineAccess`** — one array access with its classified pattern:
```cpp
struct AffineAccess {
    std::string   array;
    bool          isWrite;
    std::string   indexExpr;   // original string from Stage 1
    AccessPattern pattern;
    std::string   stride;      // "1", "N", "4", etc.
};
```

**`DepEdge`** — one edge in the data dependence graph:
```cpp
struct DepEdge {
    std::string src, dst;
    DepType     type;        // RAW, WAR, or WAW
    bool        loopCarried; // true = across iterations (drives RecMII)
    int         distance;    // iteration distance (1 = adjacent)
};
```

**`AffineLoop`** — enriched version of `HLSLoop`. Adds classified accesses, dependence edges, and II analysis fields:
```cpp
struct AffineLoop {
    // same as Stage 1
    std::string id, var, lo, hi;
    std::vector<HLSPragma> pragmas;

    // Stage 2 additions
    std::vector<AffineAccess> accesses;   // classified accesses
    std::vector<DepEdge>      deps;       // dependence edges

    int  requestedII;    // from PIPELINE pragma (-1 if not pipelined)
    int  resMII;         // resource-constrained MII  (= 1, assume 1 DSP48)
    int  recMII;         // recurrence-constrained MII
    bool IIFeasible;     // requestedII >= max(resMII, recMII)
    std::string recMIIReason;
};
```

**Why separate from `HLSContext.h`:** Stage 3 will consume `AffineContext` directly. Keeping Stage 1 and Stage 2 IRs in separate headers means each downstream stage only includes what it needs.

---

### `stage2/AffineAnalysis.h` — public API

Two functions, that's all:

```cpp
// Build Stage 2 IR from Stage 1 output.
void buildAffineContext(const HLSContext &ctx, AffineContext &affCtx);

// Pretty-print the Stage 2 IR to stdout.
void dumpAffineContext(const AffineContext &affCtx);
```

`buildAffineContext` is a pure transformation — it does not modify the input `HLSContext`.

---

### `stage2/AffineAnalysis.cpp` — full implementation

All Stage 2 logic lives here. Three parts:

---

#### Part 1 — Index expression tokeniser and pattern classifier

The array index strings from Stage 1 (e.g. `"i * N + k"`, `"k * N + j"`, `"i - t"`) are plain text. Stage 2 tokenises them and applies four classification rules in order:

**Rule 1 — Reduction detection (writes only)**

A write access is classified `Reduction` when its index string does not contain any child loop's variable:

```
WRITE C[i * N + j]   in the j-loop   where k-loop is the child
  → tokens: ["i","*","N","+","j"]
  → does "k" appear? No
  → REDUCTION  ✓
```

This captures the classic accumulator pattern: the value is computed across k iterations (into a scalar register like `sum`), then stored once after the k-loop drains.

**Rule 2 — Sliding window detection**

An access is `SlidingWindow` when: the innermost loop variable IS present, AND there is a subtraction in the index, AND an outer loop variable also appears:

```
READ in_signal[i - t]   in the t-loop   where i is the outer var
  → "t" present? Yes
  → subtraction? Yes
  → outer var "i" present? Yes
  → SLIDING_WINDOW  ✓
```

The hardware implication: `in[i]`, `in[i-1]`, `in[i-2]` … are accessed as `t` increments. Rather than going back to DDR for each tap, Stage 4 can insert a shift register (line buffer) that slides one element forward each outer iteration.

**Rule 3 — Strided detection**

An access is `Strided` when the innermost loop variable appears next to a `*` token:

```
READ B[k * N + j]   in the k-loop
  → tokens: ["k","*","N","+","j"]
  → "k" at index 0, next token is "*"  → STRIDED, stride = "N"  ✓
```

`stride = N` means every step in `k` jumps `N` memory locations — a column-major access pattern. Stage 4 needs to partition `B` into N banks so all k iterations get simultaneous access.

**Rule 4 — Sequential (default)**

If the innermost variable is present but none of the above conditions apply, the access is `Sequential` with stride 1.

```
READ A[i * N + k]   in the k-loop
  → "k" present? Yes. Subtraction? No. Multiplied? No (the "*" is between i and N, not k)
  → SEQUENTIAL, stride = 1  ✓
```

---

#### Part 2 — Build AffineContext from HLSContext

`buildAffineLoop` recursively mirrors the `HLSLoop` tree into `AffineLoop` nodes, adding the classified accesses and then running II analysis.

**II Analysis — RecMII computation**

RecMII (Recurrence-constrained Minimum Initiation Interval) is the floor imposed by loop-carried dependencies. A loop cannot achieve II below RecMII no matter how many resources it has.

Two sources of RecMII are detected:

*Source 1 — Explicit Reduction write*

If the pipelined loop (or any of its children) has a `Reduction`-classified write, there is a loop-carried dependency on the accumulator register. Stage 2 sets `RecMII = 1` because:
- The dependency distance is 1 (each iteration reads the accumulator updated by the previous one)
- A floating-point add/accumulate has latency 1 in a pipelined DSP48
- `RecMII = latency / distance = 1 / 1 = 1`

*Source 2 — Reads-only pipeline (conservative heuristic)*

If the pipelined loop has no writes at all in its entire subtree, Stage 1's array-only tracking missed a scalar accumulator. The classic case is `sum += A[...] * B[...]` where `sum` is a `float` variable — Stage 1 only records array subscript expressions, not scalar assignments.

Stage 2 applies a conservative heuristic: any reads-only pipeline implies a scalar accumulation → `RecMII = 1`. This is correct for every accumulator loop in the test suite.

**II Feasibility check**
```
AchievableII = max(ResMII, RecMII)
IIFeasible   = (requestedII >= AchievableII)
```

If the user wrote `#pragma HLS PIPELINE II=1` but the loop actually needs `II=2`, Stage 2 flags it as infeasible.

**Dependence edge construction**

- A loop-carried dep edge (`src = dst = "accumulator"`) is added whenever `RecMII > 0`
- Intra-iteration RAW edges are added for write-read pairs on the same array within the pipeline scope

---

#### Part 3 — Pretty printer (`dumpAffineContext`)

Prints the Stage 2 IR to stdout, structured to follow the Stage 1 output format. For each pipelined loop, it prints:
- Classified accesses with pattern labels and stride values
- II Analysis block (ResMII, RecMII, achievability)
- Dependence edges

---

## What Stage 2 Produces — Actual Output for Each Test

### `test/vadd.cpp`

```
Function: vadd  (line 17)
  L0  line 18  for i in [0, N)  [PIPELINE II=1]
    WRITE  C[i]   →  SEQUENTIAL
    READ   A[i]   →  SEQUENTIAL
    READ   B[i]   →  SEQUENTIAL

    ── II Analysis ──────────────────────────────
    ResMII = 1  (1 MAC/cycle, 1 DSP48)
    RecMII = 0  (no loop-carried dependences)
    Achievable II = max(1, 0) = 1   →  requested II=1 ✓ feasible
```

**What this tells downstream stages:**
- All three arrays are `SEQUENTIAL` (stride=1) → Stage 5 generates burst AXI-Master transfers for A, B, C in DDR
- `RecMII = 0` — there is no loop-carried dependency. `C[i]` depends only on `A[i]` and `B[i]` of the same iteration `i`, never on `C[i-1]`. The II floor is purely `ResMII = 1`
- Stage 3 can schedule one add per cycle with no recurrence constraint

---

### `test/fir.cpp`

```
Function: fir_filter  (line 19)
  L0  line 26  for i in [0, length)  [PIPELINE II=1]
    WRITE  out_signal[i]   →  REDUCTION

    ── II Analysis ──────────────────────────────
    ResMII = 1  (1 MAC/cycle, 1 DSP48)
    RecMII = 1  (accumulator dep: latency=1, distance=1)
    Achievable II = max(1, 1) = 1   →  requested II=1 ✓ feasible

    ── Dependences ─────────────────────────────
      accumulator  ──RAW (loop-carried, dist=1)──►  accumulator

    L0.L1  line 29  for t in [0, TAPS)  [UNROLL factor=4]
      READ   in_signal[i - t]   →  SLIDING_WINDOW  stride=1
      READ   coeffs[t]           →  SEQUENTIAL      stride=1
```

**What this tells downstream stages:**
- `WRITE out_signal[i]` is `REDUCTION` — the i-loop accumulates 16 products into a scalar `acc`, then stores once. Stage 3 sees this as an accumulator register that must be initialised at the start of each i iteration and read back 16 times
- `RecMII = 1` — the accumulation `acc += in[i-t] * coeffs[t]` is a loop-carried dependency on `acc`. The recurrence distance is 1 (each t-iteration reads `acc` written by t-1). This sets the minimum II for the outer loop at 1
- `in_signal[i - t]` is `SLIDING_WINDOW` — as `t` increments, the index moves backward through `in_signal`. Stage 4 will insert a shift register: load `in[i]` once per i-iteration, shift 16 times for each tap. This avoids 16 DDR reads per output sample and replaces them with 1 DDR read + 15 register shifts
- `coeffs[t]` is `SEQUENTIAL` — but since the loop is `UNROLL factor=4`, four copies of `coeffs` are read simultaneously. Stage 4 needs `ARRAY_PARTITION cyclic factor=4` on `coeffs` to give each unrolled copy its own BRAM bank

---

### `test/matmul.cpp`

```
Function: matmul  (line 8)
  L0  line 9  for i in [0, N)
    L0.L1  line 10  for j in [0, N)  [PIPELINE II=1]
      WRITE  C[i * N + j]   →  REDUCTION

      ── II Analysis ──────────────────────────────
      ResMII = 1  (1 MAC/cycle, 1 DSP48)
      RecMII = 1  (accumulator dep: latency=1, distance=1)
      Achievable II = max(1, 1) = 1   →  requested II=1 ✓ feasible

      ── Dependences ─────────────────────────────
        accumulator  ──RAW (loop-carried, dist=1)──►  accumulator

      L0.L1.L2  line 13  for k in [0, N)
        READ   A[i * N + k]   →  SEQUENTIAL   stride=1
        READ   B[k * N + j]   →  STRIDED       stride=N
```

**What this tells downstream stages:**
- `WRITE C[i*N+j]` is `REDUCTION` — `C[i][j]` is not written inside the k-loop. Instead, the k-loop accumulates into a scalar `sum`, then `C[i*N+j] = sum` after k finishes. The j-loop is the pipeline stage: it initialises `sum`, runs the k-loop, then stores
- `RecMII = 1` — `sum += A[...] * B[...]` in the k-loop is a loop-carried dep on `sum` (each k-iteration reads the sum written by k-1). II cannot go below 1
- `A[i*N+k]` is `SEQUENTIAL` (stride=1 in k) — row-major access pattern. As k increments by 1, the address increments by 1 float. Burst-friendly
- `B[k*N+j]` is `STRIDED` (stride=N in k) — column-major access pattern. As k increments by 1, the address jumps N floats forward. Every k-iteration touches a completely different cache line. Stage 4 must either partition B by columns into N BRAM banks or load a column into local memory before the k-loop starts

---

### `test/conv2d.cpp`

```
Function: conv2d  (line 29)
  L0  line 30  for oh in [0, OH)
    L0.L1  line 31  for ow in [0, OW)
      L0.L1.L2  line 33  for kh in [0, KH)
        L0.L1.L2.L3  line 34  for kw in [0, KW)  [PIPELINE II=1]

          ── II Analysis ──────────────────────────────
          ResMII = 1  (1 MAC/cycle, 1 DSP48)
          RecMII = 1  (reads-only loop → scalar accumulator (e.g. sum +=))
          Achievable II = max(1, 1) = 1   →  requested II=1 ✓ feasible

          ── Dependences ─────────────────────────────
            scalar acc  ──RAW (loop-carried, dist=1)──►  scalar acc
```

**What this tells downstream stages:**
- The kw pipeline loop has no array writes — `sum` is a scalar accumulator that Stage 1 does not track (Stage 1 only tracks array subscript expressions). Stage 2 applies a heuristic: any reads-only pipeline loop implies a scalar accumulation, so `RecMII = 1`
- This is correct: `sum += input[oh+kh][ow+kw] * kernel[kh][kw]` is a loop-carried dependency on `sum` across kw iterations, with distance=1
- Stage 3 can schedule one MAC per kw cycle (II=1 is achievable)
- Total latency per output pixel = `KH × KW = 3 × 3 = 9` cycles. For a 30×30 output: `900 × 9 = 8100` cycles
- Stage 4 will note that `kernel` is small (3×3 = 9 floats = 36 bytes) and can be cached entirely in registers, avoiding BRAM instantiation entirely

---

## What Stage 2 Gives to Every Later Stage

| What Stage 2 produces | Who consumes it |
|---|---|
| `SEQUENTIAL` flag per array | Stage 5 — enables burst AXI-Master DMA |
| `STRIDED` flag + stride value | Stage 4 — triggers BRAM column partition |
| `SLIDING_WINDOW` flag | Stage 4 — triggers line buffer / shift register insertion |
| `REDUCTION` flag on writes | Stage 3 — schedules accumulator register; Stage 4 allocates it |
| `RecMII` per pipelined loop | Stage 3 — sets the hard lower bound on II when building the schedule |
| `IIFeasible` flag | Stage 3 — rejects or warns before attempting an impossible schedule |
| DDG edges (loop-carried RAW) | Stage 3 — builds the SDC constraint graph from these edges |

---

## How Stage 2 Connects to the Pipeline

```
HLSContext (Stage 1 output)
      │
      ▼  buildAffineContext()
AffineContext
  ├── AffineFunction
  │     ├── AffineLoop  (with classified accesses + deps + II analysis)
  │     │     └── AffineLoop  (children, recursively)
  │     └── ...
  └── ...
      │
      ▼  dumpAffineContext()      (Stage 2 output to stdout)
      │
      ▼  Stage 3 — Scheduling     (consumes AffineContext directly)
```

Stage 2 adds no new Clang dependency. It is a pure C++17 transformation over the IR that Stage 1 built.
