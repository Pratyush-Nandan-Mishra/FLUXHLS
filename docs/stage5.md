# Stage 5 — Interface Synthesis

## Overview

Stage 5 consumes the Stage 4 `BindingContext` and produces an `InterfaceContext`
containing two structures per function:

- **AXI4-Lite register map** — control/scalar interface visible to the ARM CPU
- **AXI4-Master port list** — high-bandwidth data ports (one per `m_axi` array)

This mirrors what Vitis HLS calls *Interface Synthesis*: the step where each
function parameter is assigned a concrete protocol (s_axilite, m_axi, ap_none)
and a physical register or port address.

---

## What Stage 5 Reads

Stage 5 reads **only** `BindingContext` because Stage 4 already carries everything
that is needed:

| BindingContext field | Used for |
|---|---|
| `BoundFunction.params` | HLSParam list with interface pragmas from Stage 1 |
| `BoundFunction.loops` | Loop tree used to find pipeline path and hi strings |
| `BoundLoop.arrayBindings` | Direction (read/write ports), resource type, configStr |
| `BoundLoop.hi` | Contributes to burst-length expression |
| `BoundLoop.requestedII` | Marks the pipelined loop (stopping point for path DFS) |

---

## AXI4-Lite Register Map

The standard Vitis HLS AXI4-Lite layout is:

| Offset | Name | Width | Role |
|--------|------|-------|------|
| 0x00 | ap_ctrl | 32-bit | Start/done/idle control bits |
| 0x04 | ap_gier | 32-bit | Global interrupt enable |
| 0x08 | ap_ier | 32-bit | IP interrupt enable |
| 0x0C | ap_isr | 32-bit | IP interrupt status |
| 0x10+ | scalar params | 32/64-bit | s_axilite scalars at 8-byte stride |
| after scalars | array base ptrs | 64-bit | m_axi base address registers |

### Rules applied in FluxHLS

1. **ap_ctrl block** is always emitted first (4 registers, 0x00–0x0C).
2. **`s_axilite` scalar params** (non-array, non-pointer) are placed starting at
   0x10, stepping by 8 bytes each. Width is 64-bit for pointers/64-bit types,
   32-bit for int/float.
3. **`m_axi` array params** contribute a 64-bit base-address register in the
   AXI4-Lite map (the CPU writes the DRAM pointer here before starting the kernel).
   These follow after all scalars.
4. Params with no explicit interface pragma (e.g. `int N` in fir.cpp) are **not**
   included in the register map — they are not exposed to the AXI4-Lite bus.

---

## AXI4-Master Port Generation

For every `m_axi` parameter one `AxiMasterPort` is generated:

### Direction Inference

Direction is inferred from the `ArrayBinding` found for that parameter's name:

| Condition | Direction |
|-----------|-----------|
| `readPorts > 0`, `writePorts == 0` | READ |
| `writePorts > 0`, `readPorts == 0` | WRITE |
| both > 0 | READWRITE |
| no ArrayBinding visible | Unknown |

The "no binding" case arises for conv2d because Stage 1 cannot see 2D subscript
expressions (`input[oh*W+ow]` is inside a block with `sum` accumulation, not
directly in the kw-loop that is walked by the visitor).

### Data Width

Always **32-bit** in the current implementation (the arrays in all four tests are
`float*` or `float[N][N]`, which is 4 bytes = 32 bits). A future stage would
inspect the element type.

### Channel Count

Parsed from `ArrayBinding.configStr`:

- `"N banks cyclic"` → `"N"` channels
- anything else (1 bank, depth=…, etc.) → `"1"` channel

For matmul's B array (STRIDED → N banks cyclic) this correctly yields N AXI
channels, matching Vitis HLS's `array_partition` → multiple AXI streams behaviour.

### Burst-Length Expression

The burst expression is the **product of loop `hi` strings** on the path from
the function root down to (and including) the pipelined loop.

**Algorithm — `findPipelinePath` (DFS with backtracking):**
```
push loop.hi onto path
if loop.requestedII >= 0: return true   ← found the pipeline loop
for each child:
    if findPipelinePath(child, path): return true
pop loop.hi from path                   ← backtrack
return false
```

**`computeBurstExpr` — three cases:**

| Param type | Burst expression |
|---|---|
| Fixed numeric dimensions (e.g. `float[32][32]`) | Numeric product: `"1024"` |
| Symbolic array dimensions | Join dim strings with `" * "` |
| Pointer (`float*`) | Join pipeline-path hi strings with `" * "` |

---

## Test Results

### vadd.cpp

```
════════════════════════════════════════════════
  STAGE 5 — INTERFACE SYNTHESIS
════════════════════════════════════════════════

Function: vadd
  AXI4-Lite register map
  ┌──────────────────┬────────┬──────────────────────────────────┐
  │ Register         │ Offset │ Description                      │
  ├──────────────────┼────────┼──────────────────────────────────┤
  │ ap_ctrl          │  0x00  │ ap_start / ap_done / ap_idle     │
  │ ap_gier          │  0x04  │ global interrupt enable           │
  │ ap_ier           │  0x08  │ IP interrupt enable               │
  │ ap_isr           │  0x0C  │ IP interrupt status               │
  │ N  (32-bit)      │  0x10  │ scalar s_axilite                  │
  │ A_base  (64-bit) │  0x18  │ m_axi base address                │
  │ B_base  (64-bit) │  0x20  │ m_axi base address                │
  │ C_base  (64-bit) │  0x28  │ m_axi base address                │
  └──────────────────┴────────┴──────────────────────────────────┘

  AXI4-Master ports
  ┌────────────┬───────────┬───────┬──────────┬─────────────┬──────────────────┐
  │ Port       │ Direction │ Width │ Channels │ Burst       │ Binding          │
  ├────────────┼───────────┼───────┼──────────┼─────────────┼──────────────────┤
  │ A          │ READ      │  32   │    1     │ N elems     │ BRAM 1 bank      │
  │ B          │ READ      │  32   │    1     │ N elems     │ BRAM 1 bank      │
  │ C          │ WRITE     │  32   │    1     │ N elems     │ BRAM 1 bank      │
  └────────────┴───────────┴───────┴──────────┴─────────────┴──────────────────┘
  AXI summary: 2 read port(s), 1 write port(s), 0 unknown
```

- N is the only scalar `s_axilite` param — placed at 0x10.
- A, B, C are all `m_axi` pointers; burst = pipeline-path hi = `"N"`.
- Direction: A and B have readPorts=1 → READ; C has writePorts=1 → WRITE.

---

### fir.cpp

```
════════════════════════════════════════════════
  STAGE 5 — INTERFACE SYNTHESIS
════════════════════════════════════════════════

Function: fir
  AXI4-Lite register map
  ┌──────────────────────┬────────┬──────────────────────────────────┐
  │ Register             │ Offset │ Description                      │
  ├──────────────────────┼────────┼──────────────────────────────────┤
  │ ap_ctrl              │  0x00  │ ap_start / ap_done / ap_idle     │
  │ ap_gier              │  0x04  │ global interrupt enable           │
  │ ap_ier               │  0x08  │ IP interrupt enable               │
  │ ap_isr               │  0x0C  │ IP interrupt status               │
  │ in_signal_base (64-bit) │ 0x10 │ m_axi base address              │
  │ out_signal_base (64-bit) │ 0x18 │ m_axi base address             │
  └──────────────────────┴────────┴──────────────────────────────────┘

  AXI4-Master ports
  ┌────────────┬───────────┬───────┬──────────┬──────────────┬──────────────────────────┐
  │ Port       │ Direction │ Width │ Channels │ Burst        │ Binding                  │
  ├────────────┼───────────┼───────┼──────────┼──────────────┼──────────────────────────┤
  │ in_signal  │ READ      │  32   │    1     │ length elems │ ShiftRegister depth=TAPS │
  │ out_signal │ WRITE     │  32   │    1     │ length elems │ BRAM 1 bank              │
  └────────────┴───────────┴───────┴──────────┴──────────────┴──────────────────────────┘
  AXI summary: 1 read port(s), 1 write port(s), 0 unknown
```

- `int length` has no explicit `s_axilite` pragma in fir.cpp → not in register map.
- Burst = pipeline-path hi = `"length"`.
- in_signal: SLIDING_WINDOW → ShiftRegister — still READ because data flows in.
- out_signal: REDUCTION → BRAM — WRITE because accumulator is written out.

---

### matmul.cpp

```
════════════════════════════════════════════════
  STAGE 5 — INTERFACE SYNTHESIS
════════════════════════════════════════════════

Function: matmul
  AXI4-Lite register map
  ┌──────────────────┬────────┬──────────────────────────────────┐
  │ Register         │ Offset │ Description                      │
  ├──────────────────┼────────┼──────────────────────────────────┤
  │ ap_ctrl          │  0x00  │ ap_start / ap_done / ap_idle     │
  │ ap_gier          │  0x04  │ global interrupt enable           │
  │ ap_ier           │  0x08  │ IP interrupt enable               │
  │ ap_isr           │  0x0C  │ IP interrupt status               │
  │ A_base  (64-bit) │  0x10  │ m_axi base address                │
  │ B_base  (64-bit) │  0x18  │ m_axi base address                │
  │ C_base  (64-bit) │  0x20  │ m_axi base address                │
  └──────────────────┴────────┴──────────────────────────────────┘

  AXI4-Master ports
  ┌────────────┬───────────┬───────┬──────────┬─────────────┬─────────────────────┐
  │ Port       │ Direction │ Width │ Channels │ Burst       │ Binding             │
  ├────────────┼───────────┼───────┼──────────┼─────────────┼─────────────────────┤
  │ A          │ READ      │  32   │    1     │ N * N elems │ BRAM 1 bank         │
  │ B          │ READ      │  32   │    N     │ N * N elems │ BRAM N banks cyclic │
  │ C          │ WRITE     │  32   │    1     │ N * N elems │ BRAM 1 bank         │
  └────────────┴───────────┴───────┴──────────┴─────────────┴─────────────────────┘
  AXI summary: 2 read port(s), 1 write port(s), 0 unknown
```

- No scalar `s_axilite` params (matmul.cpp has no explicit s_axilite N pragma).
- Pipeline path = L0 (hi="N") → L0.L1 (hi="N") → pipelined at L0.L1. Path = ["N","N"].
- Burst = `"N"` + `" * "` + `"N"` = `"N * N"` for all three arrays.
- B: STRIDED → N banks cyclic → `parseNumChannels("N banks cyclic")` = `"N"` channels.

---

### conv2d.cpp

```
════════════════════════════════════════════════
  STAGE 5 — INTERFACE SYNTHESIS
════════════════════════════════════════════════

Function: conv2d
  AXI4-Lite register map
  ┌──────────────────────┬────────┬──────────────────────────────────┐
  │ Register             │ Offset │ Description                      │
  ├──────────────────────┼────────┼──────────────────────────────────┤
  │ ap_ctrl              │  0x00  │ ap_start / ap_done / ap_idle     │
  │ ap_gier              │  0x04  │ global interrupt enable           │
  │ ap_ier               │  0x08  │ IP interrupt enable               │
  │ ap_isr               │  0x0C  │ IP interrupt status               │
  │ input_base  (64-bit) │  0x10  │ m_axi base address                │
  │ kernel_base (64-bit) │  0x18  │ m_axi base address                │
  │ output_base (64-bit) │  0x20  │ m_axi base address                │
  └──────────────────────┴────────┴──────────────────────────────────┘

  AXI4-Master ports
  ┌────────────┬───────────┬───────┬──────────┬─────────────┬──────────────────────────────────┐
  │ Port       │ Direction │ Width │ Channels │ Burst       │ Binding                          │
  ├────────────┼───────────┼───────┼──────────┼─────────────┼──────────────────────────────────┤
  │ input      │ ?         │  32   │    1     │ 1024 elems  │ (no binding — Stage 1 limitation) │
  │ kernel     │ ?         │  32   │    1     │ 9 elems     │ (no binding — Stage 1 limitation) │
  │ output     │ ?         │  32   │    1     │ 900 elems   │ (no binding — Stage 1 limitation) │
  └────────────┴───────────┴───────┴──────────┴─────────────┴──────────────────────────────────┘
  AXI summary: 0 read port(s), 0 write port(s), 3 unknown
```

- Stage 1 cannot see the 2D array subscripts inside the kw-loop body → no ArrayBinding.
- Direction = Unknown (AxiDir::Unknown) for all three ports.
- Burst computed from fixed type dimensions: `float[32][32]` → 32×32=1024, `float[3][3]` → 3×3=9, `float[30][30]` → 30×30=900.

---

## Design Decisions

### Why only BindingContext as input?

Stage 4 already aggregates HLSParam (with pragmas) and ArrayBinding (with direction
and resource). Reading Stage 1/2/3 contexts again would duplicate work and create
coupling. Stage 5 is intentionally isolated.

### Why string concatenation for burst?

Vitis HLS burst annotations are often symbolic (the actual length is not known
until elaboration). String concatenation — joining hi values with `" * "` —
faithfully represents what the tool would emit as a C expression, e.g. `N * N`.
For fixed-size arrays the numeric product is computed immediately (e.g. `"1024"`).

### Why Unknown direction for conv2d?

This is an honest reflection of what Stage 1 can see. The 2D array subscript
visitor does not descend into the scalar `sum` accumulation block, so no
`MemAccess` records are created. Rather than guessing, Stage 5 marks direction
as `?` and documents the limitation. A future Stage 1 improvement (deeper
expression walking) would fix this.

### Channel count from configStr

The STRIDED binding rule produces `"N banks cyclic"` in `configStr`. Stage 5
parses this string to extract `"N"` as the channel count. All other resource
configurations produce a single channel. This avoids a separate field in
`ArrayBinding` just for channel count.

---

## Data Flow Summary

```
BindingContext
    BoundFunction
        params  ─────────────────────────────────────────────────────────────────┐
        loops[0]                                                                  │
            hi="N"          ─── findPipelinePath ──► hiPath=["N"]  (vadd)        │
            children[0]                              hiPath=["N","N"] (matmul)   │
                hi="N"                               hiPath=["length"] (fir)     │
                requestedII=1 ◄── pipeline here                                  │
                arrayBindings                                                     │
                    {array="A", readPorts=1, ...}  ─► direction=READ             │
                    {array="B", readPorts=1, ...}  ─► direction=READ             │
                    {array="C", writePorts=1,...}  ─► direction=WRITE            │
                                                                                  │
InterfaceSpec                                                                     │
    axiLite   ◄─ ap_ctrl block + scalar params + m_axi base ptrs ◄───────────────┘
    axiMaster ◄─ one per m_axi param: dir + width + channels + burst
```
