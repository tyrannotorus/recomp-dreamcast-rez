// Native system hooks: Shinobi's timer read, the idle passes of Kamui's vsync wait, Rez's logic
// steps per drawn frame, and Rez's debug print.
#include "native_sys.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"
#include "dreamcomp/hook.h"

namespace rez {
namespace {

dream::System* g_sys = nullptr;
unsigned g_frame_rate = 60;
std::uint64_t g_last = 0, g_pass = 0;

}  // namespace

void native_sys_frame_rate(unsigned fps) {
    g_frame_rate = fps == 30 ? 30 : 60;
    std::printf("rez: %u fps\n", g_frame_rate);
}

void native_sys_start(dream::System& sys) {
    g_sys = &sys;
}

// Kamui's vsync wait (0x8C046000) spins until the vblank interrupt advances a frame counter,
// calling the dispatcher 0x8C06F0C0 (indices 7 and 12, both empty handlers) and counting a timeout
// down in a stack slot. Once two passes take the same cycles, the hook jumps the clock over the
// whole passes that end before the next guest-visible event and lowers the timeout by as many.
// Nothing observable happens in them: no event is due, and their only stores are stack values the
// next real pass writes again. The pass the hook is in runs normally, so the event lands on it.
bool vsync_wait_pass(dream::sh4::Ctx& c, dream::Memory& m) {
    // Only the wait loop's second dispatcher call (pr after `jsr @r14` at 0x8C04603E, r4 = 12).
    if (!g_sys || c.pr != 0x8C046042u || c.r[4] != 12u)
        return false;
    const std::uint64_t now = c.cycles, delta = now - g_last;
    g_last = now;
    if (delta == 0 || delta != g_pass) {
        g_pass = delta;  // learn the pass length; skip only once it repeats
        return false;
    }
    // Up to the scheduler's next event the guest can see. The line-boundary polls in between
    // (System::arm_next_poll) find nothing due, and host-only events (the launcher's watchdog) run
    // at the next poll at their own deadlines, so the jump may pass both. A poll already due (an
    // interrupt deliverable now) stops it.
    if (c.next_event <= now)
        return false;
    // An interrupt raised since the last poll is taken at the next poll, so the jump stops there.
    std::uint64_t deadline = g_sys->sched.next_guest_deadline();
    if (g_sys->intc.select(dream::sh4::read_sr(c)))
        deadline = std::min<std::uint64_t>(deadline, c.next_event);
    if (deadline <= now + 2 * g_pass)
        return false;
    std::uint64_t k = (deadline - now) / g_pass - 1;  // the event still lands on a real pass
    const std::uint32_t slot = c.r[15] + 8;             // the loop's timeout (its r15 + 8)
    const std::uint32_t timeout = m.read32(slot);
    if (timeout <= 2)
        return false;
    if (k > timeout - 2)
        k = timeout - 2;
    if (k == 0)
        return false;
    c.cycles += k * g_pass;
    m.write32(slot, timeout - static_cast<std::uint32_t>(k));
    g_last = c.cycles;
    return false;  // this pass itself runs as written
}

// 0x8C0D06E4 (r4 = logic steps per drawn frame): `[0x8C28E080] = r4`, a leaf. Rez's logic
// (0x8C084260) takes one step per VBlank tick (counted at [0x8C28E088]) and draws once every
// [0x8C28E07C] steps, moved one a frame toward this target: 1 in the menus, 2 in the stages. At
// 60 fps the hook stores at most 1, so every step is drawn; at 30 the setter runs as written.
bool set_frame_steps(dream::sh4::Ctx& c, dream::Memory& m) {
    if (g_frame_rate != 60 || !g_sys)
        return false;
    if (c.cycles >= c.next_event) {
        c.pc = 0x8C0D06E4u;
        dream::sh4::deliver_irq(c, m);
    }
    const std::uint32_t want = c.r[4], use = want > 1 ? 1 : want;
    c.r[1] = 0x8C28E080u;
    c.cycles += dream::sh4::cost(3);
    m.write32(c.r[1], use);
    std::printf("rez: frame %llu: logic steps per drawn frame %u requested, %u used\n",
                static_cast<unsigned long long>(g_sys->spg.frames()), want, use);
    return true;
}

// syTmrGetCount (0x8C01432A), `return 0xFFFFFFFF - TCNT0`: the game's time base. The value comes
// from the timer model without a register access; r0, r2 and r3 are left as the original leaves
// them, with its entry poll, scheduler catch-up and 5 cycles.
bool sy_tmr_get_count(dream::sh4::Ctx& c, dream::Memory& m) {
    if (!g_sys)
        return false;
    if (c.cycles >= c.next_event) {
        c.pc = 0x8C01432Au;
        dream::sh4::deliver_irq(c, m);
    }
    if (g_sys->memory.on_device_access)
        g_sys->memory.on_device_access();
    const std::uint32_t tcnt = g_sys->tmu.tcnt(0);
    c.r[2] = 0xFFD8000Cu;
    c.r[3] = tcnt;
    c.r[0] = 0xFFFFFFFFu - tcnt;
    c.cycles += dream::sh4::cost(5);
    return true;
}

}  // namespace rez

DC_HOOK_ENTRY(sy_tmr_get_count) {
    return rez::sy_tmr_get_count(c, m);
}

DC_HOOK_ENTRY(set_frame_steps) {
    return rez::set_frame_steps(c, m);
}

DC_HOOK_ENTRY(km_dispatch) {
    return rez::vsync_wait_pass(c, m);
}
