#pragma once
#include "stage1/HLSContext.h"
#include "AffineContext.h"

// Build Stage 2 IR from Stage 1 output.
// Classifies every array access, builds dependence edges, and computes II.
void buildAffineContext(const HLSContext &ctx, AffineContext &affCtx);

// Pretty-print the Stage 2 IR to stdout.
void dumpAffineContext(const AffineContext &affCtx);
