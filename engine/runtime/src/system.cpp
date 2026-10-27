#include "dream/runtime/system.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/hle/bios.h"
#include "dream/runtime/sh4/ops.h"

namespace dream {

System::System() : tmu(sched, intc), holly(intc), spg(sched, holly) {
    memory.map_p4(sh4::Intc::kBase, sh4::Intc::kBase + 0x10, &intc);
    memory.map_p4(sh4::Tmu::kBase, sh4::Tmu::kEnd, &tmu);
    memory.map_mmio(holly::Intc::kBase, holly::Intc::kEnd, &holly);
    memory.map_mmio(pvr::Spg::kRegLo, pvr::Spg::kRegHi, &spg);
    memory.map_mmio(pvr::Spg::kStatus, pvr::Spg::kStatus + 4, &spg);
    reset();
}

System::~System() {
    if (sh4::hooks() == this)
        sh4::set_hooks(nullptr);
}

void System::reset() {
    ctx = sh4::Ctx{};
    ctx.sr = 0x700000F0u;
    sh4::write_fpscr(ctx, 0x00040001u);
    ctx.next_event = 0;
    tmu.reset();
    holly.reset();
    spg.reset();
    nesting = 0;
}

void System::install() {
    sh4::set_hooks(this);
    // Devices live in the present: bring the scheduler up to the guest clock before any register
    // access, so timers and drive status reflect the cycles emitted code has accumulated since the
    // last interrupt poll. Guarded because an event run here may itself touch a device.
    memory.on_device_access = [this] {
        if (advancing_for_device || sched.now() >= ctx.cycles)
            return;
        advancing_for_device = true;
        sched.advance_to(ctx.cycles);
        advancing_for_device = false;
    };
}

void System::arm_next_poll(sh4::Ctx& c) noexcept {
    // Poll again when the next event is due, or immediately if something is already deliverable,
    // and at the next line boundary at the latest: the SPG has events only on the lines that do
    // something (pvr/spg.h), but the guest still polls on every line, so an interrupt a device
    // raises between events is taken on the same line as with an event per line.
    if (intc.select(sh4::read_sr(c))) {
        c.next_event = c.cycles;
        return;
    }
    const std::uint64_t next = sched.next_deadline();
    std::uint64_t line = spg.next_line();
    // Lazy sound samples (aica::Aica::set_lazy) have no event each, but the guest still polls at
    // each sample's time, as for the lines: the first sample after the scheduler's present,
    // whether or not the samples up to it have been made yet.
    if (aica && aica->lazy()) {
        std::uint64_t t = aica->next_due();
        if (t <= sched.now())
            t += ((sched.now() - t) / aica::Aica::kSh4CyclesPerSample + 1) * aica::Aica::kSh4CyclesPerSample;
        if (t < line)
            line = t;
    }
    c.next_event = next < line ? next : line;
}

// SH-4 exception entry (SH7750 manual 5.x; Flycast Do_Interrupt): SSR/SPC/SGR saved, BL, MD and
// RB set (bank swap), then the handler at VBR + offset runs as a nested guest call until its RTE.
void System::enter_exception(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t vector_offset) {
    c.ssr = sh4::read_sr(c);
    c.spc = c.pc;
    c.sgr = c.r[15];
    sh4::write_sr(c, c.ssr | sh4::SR_BL | sh4::SR_MD | sh4::SR_RB);
    const std::uint32_t handler = c.vbr + vector_offset;
    c.pc = handler;
    ++nesting;
    frame_pcs.push_back(c.spc);
    frame_sps.push_back(c.sgr);
    // A non-local return inside the handler unwinds through here: keep the frame books straight.
    struct Frame {
        System& s;
        ~Frame() {
            s.frame_pcs.pop_back();
            s.frame_sps.pop_back();
            --s.nesting;
        }
    } frame{*this};
    sh4::call_indirect(c, m, handler);  // returns after the handler's RTE
}

unsigned System::deliver_pending(sh4::Ctx& c, ::dream::Memory& m) {
    unsigned delivered = 0;
    while (auto sel = intc.select(sh4::read_sr(c))) {
        memory.intevt = sel->intevt;
        ++delivered;
        enter_exception(c, m, 0x600);
        if (delivered > 64)  // a handler that never clears its source would loop forever
            break;
    }
    return delivered;
}

void System::on_poll(sh4::Ctx& c, ::dream::Memory& m) {
    if (c.cycles > sched.now())
        sched.advance_to(c.cycles);  // devices raise their interrupts here
    deliver_pending(c, m);
    arm_next_poll(c);
}

void System::on_trapa(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t imm) {
    memory.tra = (imm & 0xFFu) << 2;
    memory.expevt = 0x160;
    enter_exception(c, m, 0x100);
    arm_next_poll(c);
}

void System::on_rte(sh4::Ctx& c, ::dream::Memory& m) {
    const bool switched =
        !frame_pcs.empty() && (c.spc != frame_pcs.back() || c.r[15] != frame_sps.back());
    sh4::write_sr(c, c.ssr);
    c.pc = c.spc;
    if (switched) {
        // The handler resumes a different context (Katana's scheduler ran inside the VBlank
        // handler): the interrupted host frames belong to the suspended task. Drop them and let
        // run_guest re-enter the resumed task at its SPC. The delivery frame's bookkeeping unwinds
        // through enter_exception.
        sh4::nonlocal_return(c, m, c.spc);
    }
    // The emitted `return` after rte() unwinds to enter_exception's call site.
}

void System::on_untranslated(sh4::Ctx& c, ::dream::Memory&, std::uint32_t target) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "untranslated call target 0x%08x from pc 0x%08x", target, c.pc);
    throw UntranslatedCall(target, c.pc, buf);
}

void System::on_resume(sh4::Ctx& c, ::dream::Memory&, std::uint32_t pc) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "cannot resume at 0x%08x (non-local return from pc 0x%08x)", pc,
                  c.pc);
    throw UntranslatedCall(pc, c.pc, buf);
}

}  // namespace dream
