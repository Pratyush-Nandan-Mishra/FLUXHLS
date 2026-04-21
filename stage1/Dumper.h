#pragma once
#include "HLSContext.h"

// Abstract interface — swap PrettyDumper for JSONDumper/MLIRDumper later
// without touching any other file.
struct IDumper {
    virtual void onFunction(const HLSFunction &fn, int idx) = 0;
    virtual void onLoop    (const HLSLoop     &loop, int depth) = 0;
    virtual ~IDumper() = default;
};

struct PrettyDumper : IDumper {
    void onFunction(const HLSFunction &fn, int idx) override;
    void onLoop    (const HLSLoop     &loop, int depth) override;
};
