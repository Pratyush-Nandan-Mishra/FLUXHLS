#pragma once                            // guard against multiple inclusion
#include "stage2/AffineContext.h"       // AffineContext — Stage 3 input
#include "stage3/ScheduleContext.h"     // ScheduleContext — Stage 3 output

// Build Stage 3 IR from Stage 2 output.
// Infers operations from access patterns, then runs ASAP and ALAP passes
// for each pipelined loop.  Pure transformation — no Clang dependency.
void buildSchedule(const AffineContext &affCtx, ScheduleContext &schedCtx);

// Pretty-print the Stage 3 IR to stdout.
// Shows the ASAP/ALAP schedule table and pipeline depth for every pipelined loop.
void dumpSchedule(const ScheduleContext &schedCtx);
