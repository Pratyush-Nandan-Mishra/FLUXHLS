# Stage 1 — Frontend

Stage 1 is the entry point of the FluxHLS pipeline. It takes a C++ source file
annotated with `#pragma HLS` directives and produces an **HLS IR** (the
`HLSContext` object) that every downstream stage consumes.

---

## Files That Belong to Stage 1

```
FluxHLS/
├── main.cpp              ← CLI driver — wires Stage 1 together
└── stage1/
    ├── Pragma.h          ← data structs for every #pragma HLS kind
    ├── HLSContext.h      ← IR nodes: HLSFunction, HLSLoop, HLSParam
    ├── Frontend.h        ← public API: parsePragmas + buildHLSContext
    ├── Frontend.cpp      ← all Clang-touching code
    ├── Dumper.h          ← IDumper interface + PrettyDumper declaration
    └── Dumper.cpp        ← PrettyDumper impl + HLSContext::dump
```

No other file in the project is touched by Stage 1.

---

## What Happens in Stage 1

```
User writes C++ with #pragma HLS annotations
              │
              ▼
      parsePragmas()              ← reads source as plain text
   extracts every #pragma HLS line and stores it with its line number
              │
              ▼
      buildHLSContext()           ← drives the libclang C API
   walks the Clang AST:
     • finds function declarations
     • extracts parameter names and types
     • finds for-loops and builds a nested loop tree (L0, L0.L1, L0.L1.L2 …)
     • detects array reads and writes inside each loop
              │
              ▼
      matchAllPragmas()           ← post-pass
   attaches PIPELINE / UNROLL pragmas to the correct loop
   (Vitis allows pragmas before the loop header OR at the top of the body)
              │
              ▼
      HLSContext::dump()
   prints an annotated human-readable representation
```

---

## File-by-File Explanation

### `stage1/Pragma.h` — pure data, no logic

Defines `HLSPragma` — a struct that holds one parsed `#pragma HLS` directive
along with its source line number. Each pragma kind has its own parameter
struct stored inside a `std::variant`.

```cpp
enum class PragmaKind { Pipeline, Unroll, ArrayPartition, Interface };

struct PipelinePragma       { int II = 1; };
struct UnrollPragma         { int factor = 0; };   // 0 = full unroll
struct ArrayPartitionPragma { std::string var, type; int factor = 1; };
struct InterfacePragma      { std::string port, mode; };

struct HLSPragma {
    PragmaKind kind;
    unsigned   line;
    std::variant<PipelinePragma, UnrollPragma,
                 ArrayPartitionPragma, InterfacePragma> data;
};
```

**Why separate:** Every other layer consumes pragma data. Keeping it isolated
means adding a new pragma type is a one-file change with zero ripple effects.

---

### `stage1/HLSContext.h` — the HLS IR

Defines three IR node types. No Clang dependency — Stage 2+ consumes these
directly without touching `Frontend.cpp`.

| Type | Represents |
|---|---|
| `HLSParam` | One function parameter (name, type, interface pragmas) |
| `HLSLoop` | One `for`-loop (ID, bounds, pragmas, accesses, children) |
| `HLSFunction` | One function (params + top-level loop tree) |

`HLSContext` is the top-level container holding all functions found in the file.

Loop IDs use dotted notation: `L0`, `L0.L1`, `L0.L1.L2` — stable identifiers
that downstream stages use to refer to specific loops in the schedule.

---

### `stage1/Frontend.h / Frontend.cpp` — all Clang-touching code

Two public functions:

#### `parsePragmas(filepath)`
Reads the source file as plain text, scans for `#pragma HLS` lines, and
tokenises each one into the appropriate `HLSPragma` struct. No Clang needed —
pragmas are preprocessor directives and do not appear in the AST.

Supported pragmas:
```
#pragma HLS PIPELINE II=1
#pragma HLS UNROLL factor=4
#pragma HLS ARRAY_PARTITION variable=A type=cyclic factor=4
#pragma HLS INTERFACE m_axi port=A
```

#### `buildHLSContext(filepath, pragmas, ctx)`
Uses the **libclang C API** (`clang-c/Index.h`) to parse the source and walk
the AST. Three internal functions do the work:

1. **`topVisitor`** — finds `FunctionDecl` definitions, extracts parameters,
   matches `INTERFACE` pragmas to port names.

2. **`bodyVisitor`** — recursively walks the function body:
   - `CXCursor_ForStmt` → creates an `HLSLoop`, assigns a hierarchical ID,
     extracts induction variable and upper bound via `extractForInfo`.
   - `CXCursor_BinaryOperator` / `CXCursor_CompoundAssignOperator` →
     detects assignment operators; sets a write flag for the LHS via
     `visitSubtree` (visits the cursor itself, not just its children).
   - `CXCursor_ArraySubscriptExpr` → records a `MemAccess` on the innermost
     active loop, marked READ or WRITE based on the flag.

3. **`matchAllPragmas`** — post-pass that walks the finished loop tree and
   attaches PIPELINE/UNROLL pragmas using Vitis semantics:
   - Pragma just before the loop header → belongs to that loop
   - Pragma at the top of the loop body (before any child loop) → also belongs
   - A `claimed` set prevents the same pragma attaching to both parent and child

**Why all Clang code is here:** If we later replace libclang with LibTooling or
MLIR ClangIR, only this file changes. Nothing else knows about Clang.

#### Key implementation details

**`cursorText()` — macro-safe source extraction**

Uses `clang_getExpansionLocation` (where a macro is *used*) instead of
`clang_getCursorExtent` (which can follow a `#define` back to its definition
and return the entire rest of the file as the token stream). This fixes loop
bound extraction for files that use macros like `#define TAPS 16`.

**`visitSubtree()` — visiting a cursor itself**

`clang_visitChildren` visits a cursor's *children*, not the cursor itself.
`visitSubtree(c, data)` calls `bodyVisitor(c, c, data)` directly then recurses,
making it possible to visit the LHS of an assignment as the write target.

**`matchPragmasToLoop()` — `claimed` set**

Without the `claimed` set, a pragma inside the body of `for j` (e.g., at
line 11) would be seen as "top of body" by `for k` (line 13) as well, causing
double attachment. The `claimed` set marks each pragma line as owned the moment
it is attached, so child loops cannot re-claim it.

---

### `stage1/Dumper.h / Dumper.cpp` — output layer

`IDumper` is a pure abstract interface:
```cpp
struct IDumper {
    virtual void onFunction(const HLSFunction &, int idx) = 0;
    virtual void onLoop    (const HLSLoop &, int depth)   = 0;
};
```

`PrettyDumper` implements it for human-readable stdout output.
`HLSContext::dump(IDumper &d)` is also defined here — it is the only translation
unit that needs both the IR and the dumper interface fully defined.

**Why an interface:** Stage 1 uses `PrettyDumper`. Later stages can add
`JSONDumper` or `MLIRDumper` without touching any other file.

---

### `main.cpp` — CLI driver

~30 lines. Reads `argv[1]`, calls `parsePragmas`, calls `buildHLSContext`,
creates a `PrettyDumper`, calls `ctx.dump(dumper)`. Any arguments after `--`
are forwarded to Clang as compiler flags.

---

## Example Output

Running `fluxhls test/matmul.cpp`:

```
=== FluxHLS Stage 1 Output ===

Function: matmul  (line 8)
  Parameters:
    float * A  [m_axi]
    float * B  [m_axi]
    float * C  [m_axi]
    int N

  L0  line 9   for i in [0, N)
    L0.L1  line 10  for j in [0, N)  [PIPELINE II=1]
      WRITE C[i * N + j]
      L0.L1.L2  line 13  for k in [0, N)
        READ  A[i * N + k]
        READ  B[k * N + j]
```

This tells us:
- Which arrays have AXI4 master interfaces (`m_axi`)
- The loop nesting structure with stable IDs
- Which loop is pipelined and at what initiation interval
- Which arrays are read vs. written at each loop level

---

## Test Samples

| File | Algorithm | Key HLS concept |
|---|---|---|
| `test/vadd.cpp` | Vector addition | Basic `PIPELINE`, single loop, no dependency |
| `test/fir.cpp` | FIR filter | `UNROLL` + `ARRAY_PARTITION` interaction, sliding window |
| `test/matmul.cpp` | Matrix multiply | 3-level nest, accumulator, strided access |
| `test/conv2d.cpp` | 2D convolution | 4-level nest, innermost pipeline, 2D sliding window |

### `test/vadd.cpp` — Vector Addition

Add two arrays element by element: `C[i] = A[i] + B[i]`.
The simplest streaming kernel — data flows in, gets processed, flows out.

Stage 1 establishes: all three arrays need AXI4-Master ports; there is no
loop-carried dependency so `II=1` is valid; the loop bound is `N` so total
latency is exactly `N` cycles.

### `test/fir.cpp` — FIR Filter

Every output sample is a weighted sum of the last 16 input samples:
`out[i] = in[i]*c[0] + in[i-1]*c[1] + ... + in[i-15]*c[15]`.

The inner loop (over 16 taps) is unrolled by 4. Unrolling creates 4 simultaneous
reads of `coeffs[]`, requiring `ARRAY_PARTITION cyclic factor=4` so all 4 copies
get their own BRAM bank.

Stage 1 establishes: PIPELINE on the outer loop, UNROLL on the inner loop, and
that `coeffs[t]` is read inside the unrolled loop — signalling Stage 4 to
partition it.

### `test/matmul.cpp` — Matrix Multiplication

`C[i*N+j] = sum_k( A[i*N+k] * B[k*N+j] )`.

Three nested loops. Stage 1 reveals that PIPELINE is on `L0.L1` (j loop),
`C[i*N+j]` is written at the j level (reduction variable), and `B[k*N+j]`
has a strided (column-major) index — a signal for Stage 4 to partition B.

### `test/conv2d.cpp` — 2D Convolution

Slide a 3×3 kernel over a 32×32 image: 4-level loop nest.

Stage 1 establishes: PIPELINE is on the innermost loop `L0.L1.L2.L3` (kw loop),
giving one MAC per cycle. The 2D sliding window access `input[oh+kh][ow+kw]`
is captured with its full index expression — Stage 2 will detect this pattern
and flag it for line buffer insertion.

---

## What Stage 1 Gives to Every Later Stage

| What Stage 1 captures | Who consumes it |
|---|---|
| AXI interface mode per port (`m_axi`, `s_axilite`) | Stage 5 — generates AXI wrappers |
| Loop nesting tree with stable IDs | Stage 3 — builds the schedule per loop |
| PIPELINE / UNROLL / ARRAY_PARTITION per loop | Stage 3 + Stage 4 — respects user hints |
| Loop induction variable and upper bound | Stage 3 — computes latency and II |
| READ / WRITE per array per loop level | Stage 2 — builds the data dependence graph |
| Array dimensions and access index expressions | Stage 2 — detects sliding window, strided, reduction patterns |

Without Stage 1, none of the downstream stages have enough information to make
a single correct hardware decision.
