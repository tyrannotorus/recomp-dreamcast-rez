// Native graphics: Kamui's hardware-facing entry points, replaced by native code.
#pragma once

#include <cstdint>

namespace dream {
class System;
namespace mem {
class MmioHandler;
}
}  // namespace dream

namespace rez {

// Once, before the first guest instruction.
void native_gfx_start(dream::System& sys);
// The PowerVR's register block as the rest of the game reaches it (Shinobi's video handlers, the
// BIOS stand-in): the same frame settings Kamui's accessors keep.
dream::mem::MmioHandler* native_gfx_regs();

// Displayed-frame flips (FB_R_SOF1 writes) so far.
std::uint64_t native_gfx_frame_swaps();

// What a device register access does on the way, for hooks that replace one: the scheduler is
// brought up to the guest clock.
void device_access();

}  // namespace rez
