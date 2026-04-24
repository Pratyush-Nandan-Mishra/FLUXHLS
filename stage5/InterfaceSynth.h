#pragma once
#include "stage4/BindingContext.h"   // BindingContext — sole input (contains params + bindings)
#include "stage5/InterfaceContext.h" // InterfaceContext — Stage 5 IR produced here

// buildInterfaces: consume Stage 4 IR and produce Stage 5 IR.
//
// Stage 5 reads only BindingContext because it already contains everything needed:
//   BoundFunction.params  → interface pragma annotations from Stage 1
//   BoundLoop.arrayBindings → array direction (readPorts/writePorts) and resource
//   BoundLoop.hi / loop nesting → burst-length expression (string concatenation)
//
// For each function it produces:
//   - AXI4-Lite register map   (ap_ctrl block + scalar params + array base addresses)
//   - AXI4-Master port list    (one per m_axi param, with direction, width, burst, channels)
void buildInterfaces(const BindingContext  &bindCtx,
                     InterfaceContext      &ifCtx);

// dumpInterfaces: print the Stage 5 IR to stdout.
// Shows the AXI4-Lite register map table and AXI4-Master port table for every function.
void dumpInterfaces(const InterfaceContext &ifCtx);
