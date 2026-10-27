// SH-4 to C++ emitter. Its contract with the runtime is runtime/sh4/abi.h.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dream::translator {

// A loaded guest image: raw bytes at a link address.
struct Image {
    std::vector<std::uint8_t> bytes;
    std::uint32_t base = 0;

    bool contains(std::uint32_t addr, std::uint32_t len = 1) const noexcept {
        return addr >= base && addr - base + len <= bytes.size();
    }
    std::uint16_t read16(std::uint32_t addr) const noexcept {
        const std::size_t o = addr - base;
        return static_cast<std::uint16_t>(bytes[o] | (bytes[o + 1] << 8));
    }
    std::uint32_t read32(std::uint32_t addr) const noexcept {
        const std::size_t o = addr - base;
        return static_cast<std::uint32_t>(bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16) |
                                          (static_cast<std::uint32_t>(bytes[o + 3]) << 24));
    }
};

// A function to translate: the half-open address range of its code.
struct FunctionSpec {
    std::uint32_t entry = 0;
    std::uint32_t end = 0;  // exclusive
    std::string name;       // C++ identifier; defaults to fn_<entry hex>
    // FPSCR assumed live at entry. Katana's default is 0x00040001 (PR=0, SZ=0). Set the
    // PR/SZ bits to what callers establish; 0xFFFFFFFF means "unknown", forcing runtime branches.
    std::uint32_t fpscr_entry = 0x00040001;
    // Set by emit_unit's entry-mode inference when callers reach this function in different modes:
    // the bit is then tested at run time instead of assumed.
    bool entry_pr_unknown = false, entry_sz_unknown = false;
    // [hooks]: when non-empty, the function's wrapper calls
    // `bool dream_hook_entry_<hook>(Ctx&, Memory&)` first (true = the hook replaced the function,
    // skip the body) and/or `void dream_hook_exit_<hook>(Ctx&, Memory&)` after the body returns.
    // The port defines them (include/dreamcomp/hook.h has the macros).
    std::string hook;
    bool hook_entry = false, hook_exit = false;
};

struct EmitOptions {
    bool fold_literals = true;  // MOV.L @(disp,PC) inside the image becomes a constant
    bool overlay = false;       // table entries carry the code words they were translated from
    // Functions of another unit that this one may call directly by name: for a relocated view, the
    // main image's functions at addresses no relocation covers. A call through a register holding
    // one of these constants calls its `__in` directly instead of looking it up. Null: only the
    // unit's own functions.
    const std::map<std::uint32_t, std::string>* imports = nullptr;
    // Split the function bodies over this many extra translation units (EmitResult::parts) so
    // the C++ compiler can build them in parallel; 1 keeps the single-file output byte for byte.
    // Above 1 the resume bodies get external linkage, since the table refers to them across
    // units.
    unsigned shards = 1;
};

struct EmitResult {
    std::string source;                   // one translation unit (with shards: prologue + table)
    std::vector<std::string> parts;       // with EmitOptions::shards > 1: the function bodies
    std::string header;                   // declarations of the emitted functions
    std::size_t fp_runtime_branches = 0;  // FP instructions lowered behind an FPSCR test
};

std::string default_name(std::uint32_t entry);

// Translate the given functions of one image into a C++ translation unit plus a header.
EmitResult emit_unit(const Image& image, const std::vector<FunctionSpec>& functions,
                     const EmitOptions& options);

}  // namespace dream::translator
