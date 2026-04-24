#pragma once
#include "stage2/AffineContext.h"   // AffineContext — provides access patterns for binding rules
#include "stage3/ScheduleContext.h" // ScheduleContext — provides ASAP cycles and op kinds
#include "stage4/BindingContext.h"  // BindingContext — Stage 4 IR produced by this stage

// buildBindings: consume Stage 2 + Stage 3 IR and produce Stage 4 IR.
//
// Stage 4 is the first stage that reads two prior stages simultaneously:
//   AffineContext  (Stage 2) → access patterns drive the 5-rule array binding table
//   ScheduleContext (Stage 3) → ASAP cycles and op kinds determine which hardware unit each op uses
//
// The result is a BindingContext where every array is mapped to a resource
// (BRAM / ShiftRegister / Register) and every scheduled op is mapped to a
// specific hardware unit (DSP48_N, BRAM_X.portN, acc_reg, etc.).
void buildBindings(const AffineContext   &affCtx,
                   const ScheduleContext &schedCtx,
                   BindingContext        &bindCtx);

// dumpBindings: print the Stage 4 IR to stdout.
// Shows array-binding tables, op-binding tables, and per-loop resource counts.
void dumpBindings(const BindingContext &bindCtx);
