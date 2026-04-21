#pragma once
#include <string>
#include <variant>

enum class PragmaKind { Pipeline, Unroll, ArrayPartition, Interface };

struct PipelinePragma       { int II = 1; };
struct UnrollPragma         { int factor = 0; };           // 0 = full unroll
struct ArrayPartitionPragma { std::string var, type; int factor = 1; };
struct InterfacePragma      { std::string port, mode; };

struct HLSPragma {
    PragmaKind kind;
    unsigned   line;
    std::variant<PipelinePragma,
                 UnrollPragma,
                 ArrayPartitionPragma,
                 InterfacePragma> data;
};
