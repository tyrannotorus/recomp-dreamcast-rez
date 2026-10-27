// Function hooks on translated guest code.
//
// In the port's game/<id>.toml:
//
//     [hooks]
//     0x8C0ABCDE = "camera_update"         # entry and exit
//     0x8C0F0000 = "draw_hud@entry"        # entry only
//     0x8C012345 = "set_viewport@exit"     # exit only
//
// and in the port's sources, one definition per side named in the config:
//
//     DC_HOOK_ENTRY(camera_update) { ...; return false; }  // true = skip the original function
//     DC_HOOK_EXIT(camera_update) { ... }
//
// `c` is the SH-4 context (registers: c.r[4..7] are the first arguments under the Hitachi ABI,
// c.r[0] the return value, c.fr[4..11] float arguments, c.fr[0] float return), `m` guest memory.
// An entry hook that returns true replaces the function: set c.r[0]/c.fr[0] as the original would.
// A hook named in the config but not defined is a link error, never a silent no-op.
#pragma once

#include <cstdint>
#include <cstring>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"

#define DC_HOOK_ENTRY(name)                                                               \
    namespace dream::gen {                                                                \
    bool dream_hook_entry_##name(dream::sh4::Ctx& c, dream::Memory& m);                   \
    }                                                                                     \
    bool dream::gen::dream_hook_entry_##name([[maybe_unused]] dream::sh4::Ctx& c,         \
                                             [[maybe_unused]] dream::Memory& m)

#define DC_HOOK_EXIT(name)                                                                \
    namespace dream::gen {                                                                \
    void dream_hook_exit_##name(dream::sh4::Ctx& c, dream::Memory& m);                    \
    }                                                                                     \
    void dream::gen::dream_hook_exit_##name([[maybe_unused]] dream::sh4::Ctx& c,          \
                                            [[maybe_unused]] dream::Memory& m)

namespace dreamcomp {

inline float read_f32(dream::Memory& m, std::uint32_t addr) {
    const std::uint32_t u = m.read32(addr);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

inline void write_f32(dream::Memory& m, std::uint32_t addr, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    m.write32(addr, u);
}

}  // namespace dreamcomp
