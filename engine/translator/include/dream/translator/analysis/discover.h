// Function discovery: which addresses in an image are code, and where functions start and end.
// The image model must be right first (link address, relocated copies), recursive descent from
// the entry alone finds almost nothing, and literal-pool pointers are candidates to be tested,
// not facts.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dream/translator/emit.h"

namespace dream::translator {

struct DiscoverOptions {
    std::vector<std::uint32_t> seeds;  // known function entries (image entry point etc.)
    // Entries walked after the pointer pass and the sweep, so they only add: those passes find
    // the same with or without them, whereas ordinary seeds change what the passes accept.
    std::vector<std::uint32_t> late_seeds;
    bool follow_pointers = true;       // seed from aligned words that point into the image
    bool aggressive_sweep = true;      // after descent, sweep unreached gaps for code
    std::uint32_t max_function_bytes = 0x40000;
};

struct DiscoveredFunction {
    std::uint32_t entry;
    std::uint32_t end;   // exclusive; covers every reachable block of the function
    std::string origin;  // "seed", "call", "pointer", "sweep"
};

struct DiscoverResult {
    std::vector<DiscoveredFunction> functions;  // sorted by entry
    // Constant targets of calls and jumps through a register (JSR, JMP, BSRF) outside the image:
    // for a relocated view, entries of the main image or of another unit. Sorted, unique.
    std::vector<std::uint32_t> external_targets;
};

DiscoverResult discover(const Image& image, const DiscoverOptions& options);

// The discovered functions as the emitter's specs, named by address.
std::vector<FunctionSpec> to_specs(const DiscoverResult& r);

}  // namespace dream::translator
