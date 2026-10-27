// Idle skip for frame-wait loops: the clock jumps over passes of a spin loop that change nothing
// but the clock, so the next interrupt arrives at the same check and cycle as when spinning.
//
// The Katana/Ninja vsync wait has this shape:
//
//     while (*flag) {                                // cleared by the vblank interrupt
//         (*callback)(arg);                          // an empty function: rts; nop
//         if (counter > start + limit + 1) break;    // frame-count timeout
//     }
//
// Hook the empty callback's entry (`[hooks] 0x8C...... = "idle_wait@entry"`) and forward to
// IdleWait::on_call from the port:
//
//     dreamcomp::IdleWait g_wait{.return_pc = 0x0C224800, .flag = 0x8C379E38, .pass_cycles = 22};
//     DC_HOOK_ENTRY(idle_wait) { g_wait.on_call(c, m); return false; }
//
// return_pc is the pr of the loop's call (the address after its delay slot); pass_cycles the
// guest cycles from one callback entry to the next, counted from the generated code (block cycle
// adds plus the taken-branch +1s) and confirmed at run time: the skip acts only when the previous
// call came exactly pass_cycles earlier, so a wrong value means no effect, never a wrong result.
// It skips the k whole passes that end before the next scheduled event; every interrupt check in
// them would have found nothing due.
#pragma once

#include <cstdint>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dreamcomp {

struct IdleWait {
    std::uint32_t return_pc = 0;    // pr inside the loop's call
    std::uint32_t flag = 0;         // the loop runs while this word is non-zero
    std::uint64_t pass_cycles = 0;  // one pass, callback entry to callback entry

    std::uint64_t last_call = 0;

    void on_call(dream::sh4::Ctx& c, dream::Memory& m) {
        const std::uint64_t now = c.cycles;
        const bool steady = now - last_call == pass_cycles;
        if (steady && c.pr == return_pc && m.read32(flag) != 0 &&
            c.next_event > now + pass_cycles && c.next_event - now < 200'000'000u) {
            const std::uint64_t k = (c.next_event - 1 - now) / pass_cycles;
            c.cycles = now + k * pass_cycles;
        }
        last_call = c.cycles;
    }
};

}  // namespace dreamcomp
