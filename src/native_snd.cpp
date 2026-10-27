// Native sound-bus guards: Shinobi's G2 suspend, resume and FIFO waits around CPU access to sound
// RAM, and syG2's DMA into sound RAM. With no bus to suspend and no FIFO to drain, only the device
// accesses are dropped; stack traffic, the counters and flags written, registers, interrupt polls
// and guest cycles are kept, so guest state and timing are unchanged. syG2's four DMA channels keep
// their state here instead of in the G2 DMA registers.
#include "native_snd.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"
#include "dreamcomp/hook.h"
#include "native_gfx.h"
#include "native_irq.h"

namespace dream::gen {
void fn_8c0333f4(dream::sh4::Ctx& c, dream::Memory& m);  // syG2's status (hooked below)
void fn_8c0133be(dream::sh4::Ctx& c, dream::Memory& m);  // the cache write-back: the CPU's, recompiled
}

namespace rez {

using dream::Memory;
using dream::sh4::Ctx;

namespace {

constexpr std::uint32_t kG2Records = 0x8C115CF4u;  // -> pointer to the G2 library's state
constexpr std::uint32_t kSuspended = 0x8C13DC74u;  // 1 while the channels are suspended

void poll(Ctx& c, Memory& m, std::uint32_t pc) {
    if (c.cycles >= c.next_event) {
        c.pc = pc;
        dream::sh4::deliver_irq(c, m);
    }
}

void push(Ctx& c, Memory& m, std::uint32_t v) {
    c.r[15] -= 4;
    m.write32(c.r[15], v);
}

std::uint32_t pop(Ctx& c, Memory& m) {
    const std::uint32_t v = m.read32(c.r[15]);
    c.r[15] += 4;
    return v;
}

// The channel register address both setters compute: base + channel * 32, with the T bit the
// last shift leaves.
std::uint32_t channel_regs(Ctx& c, Memory& m) {
    std::uint32_t r0 = m.read32(c.r[4]) << 4;
    c.t = r0 >> 31;
    r0 <<= 1;
    return r0 + m.read32(m.read32(kG2Records));
}

// 0x8C075418: SB_ADSUSP = 0 for the channel record at r4.
void resume_one(Ctx& c, Memory& m) {
    poll(c, m, 0x8C075418u);
    c.r[2] = kG2Records;
    c.r[1] = 0;
    c.r[4] = c.r[0] = channel_regs(c, m);
    c.r[3] = m.read32(m.read32(kG2Records));
    device_access();  // the SB_ADSUSP write
    c.cycles += dream::sh4::cost(13);
    c.r[0] = c.r[1];
}

// 0x8C07556C: the same with interrupts masked, SB_ADSUSP = 1.
void suspend_one(Ctx& c, Memory& m) {
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    poll(c, m, 0x8C07556Cu);
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[5] = c.r[0] = (read_sr(c) >> 4) & 15u;
    write_sr(c, (read_sr(c) & c.r[3]) | 240u);
    c.r[1] = kG2Records;
    c.r[2] = m.read32(kG2Records);
    c.r[4] = c.r[0] = channel_regs(c, m);
    c.r[2] = 1;
    device_access();  // the SB_ADSUSP write
    c.r[1] = static_cast<std::uint32_t>(-241);
    c.r[3] = read_sr(c) & c.r[1];
    c.r[0] = ((c.r[5] & 15u) << 4) | c.r[3];
    write_sr(c, c.r[0]);
    c.cycles += dream::sh4::cost(32);
    c.r[0] = 0;
}

// 0x8C0519DC resume / 0x8C051A20 suspend: the setter for each of the 4 channel records at
// [0x8C115CF4] + 4, 52 bytes apart, then [0x8C13DC74] = 0 / 1 and r0 = 0.
bool all_channels(Ctx& c, Memory& m, std::uint32_t entry, bool suspend) {
    poll(c, m, entry);
    push(c, m, c.r[14]);
    c.r[14] = 0;
    push(c, m, c.r[13]);
    push(c, m, c.r[12]);
    push(c, m, c.r[11]);
    push(c, m, c.pr);
    c.r[13] = 208;
    c.r[12] = kG2Records;
    c.r[11] = suspend ? 0x8C07556Cu : 0x8C075418u;
    c.cycles += dream::sh4::cost(9);
    const std::uint32_t loop = entry + 0x12;  // the block that loads the record pointer
    for (;;) {
        c.r[4] = m.read32(c.r[12]) + 4;
        c.cycles += dream::sh4::cost(4);
        c.pr = entry + 0x1A;
        c.r[4] += c.r[14];
        c.pc = entry + 0x16;
        if (suspend)
            suspend_one(c, m);
        else
            resume_one(c, m);
        c.r[14] += 52;
        c.t = c.r[14] >= c.r[13];
        c.cycles += dream::sh4::cost(3);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, loop);
    }
    c.r[3] = kSuspended;
    c.r[2] = suspend ? 1u : 0u;
    c.r[0] = 0;
    m.write32(c.r[3], c.r[2]);
    c.pr = pop(c, m);
    c.r[11] = pop(c, m);
    c.r[12] = pop(c, m);
    c.r[13] = pop(c, m);
    c.cycles += dream::sh4::cost(10);
    c.r[14] = pop(c, m);
    return true;
}

// 0x8C051A64 / 0x8C051914: call the check 0x8C075690 / 0x8C0751F4 (local = SB_FFST bit 4 / bit 0
// clear) until the local flag is 1, counting passes at [[0x8C115CF4] + 268 / 272]. The FIFO is
// always empty here, so the first check passes.
bool fifo_wait(Ctx& c, Memory& m, std::uint32_t entry, std::uint32_t check, std::uint32_t counter) {
    poll(c, m, entry);
    push(c, m, c.r[14]);
    push(c, m, c.r[13]);
    push(c, m, c.pr);
    c.r[15] -= 4;
    c.r[14] = check;
    c.r[13] = kG2Records;
    c.cycles += dream::sh4::cost(6);
    c.cycles += dream::sh4::cost(2);
    c.pr = entry + 0x10;
    c.r[4] = c.r[15];
    c.pc = entry + 0x0C;
    // The check.
    poll(c, m, check);
    c.r[2] = 0xA05F688Cu;
    device_access();  // the SB_FFST read
    c.r[0] = 0;
    c.t = 1;
    c.r[3] = 1;
    m.write32(c.r[4], c.r[3]);
    c.cycles += dream::sh4::cost(7);
    c.r[0] = 0;
    // Back in the wait: the flag is 1.
    c.r[0] = m.read32(c.r[15]);
    c.t = c.r[0] == 1u;
    c.cycles += dream::sh4::cost(3);
    c.r[0] = counter;
    c.r[2] = m.read32(c.r[13]);
    c.r[3] = m.read32(c.r[2] + c.r[0]) + 1;
    m.write32(c.r[2] + c.r[0], c.r[3]);
    c.r[0] = m.read32(c.r[15]);
    c.t = c.r[0] == 1u;
    c.cycles += dream::sh4::cost(8);
    c.r[0] = 0;
    c.r[15] += 4;
    c.pr = pop(c, m);
    c.r[13] = pop(c, m);
    c.cycles += dream::sh4::cost(6);
    c.r[14] = pop(c, m);
    return true;
}

// ---- syG2's DMA to the sound chip ----
// The four channels as syG2 programs them: the copy into or out of sound RAM happens at the start;
// the end follows after the bus time (4 cycles a byte; at once for 512 or less) with the channel's
// end event.
struct G2Channel {
    std::uint32_t stag = 0, star = 0, len = 0, dir = 0, tsel = 0, en = 0, st = 0, susp = 0x10;
    int event = -1;
};
std::array<G2Channel, 4> g_g2;
dream::System* g_sys = nullptr;

void g2_finish(unsigned ch) {
    static constexpr dream::holly::Irq kDone[4] = {dream::holly::Irq::AicaDma, dream::holly::Irq::Ext1Dma,
                                                   dream::holly::Irq::Ext2Dma, dream::holly::Irq::DevDma};
    G2Channel& r = g_g2[ch];
    const std::uint32_t len = r.len & 0x7FFFFFFFu;
    if (r.len & 0x80000000u)
        r.en = 0;
    r.star += len;
    r.stag += len;
    r.len = 0;
    r.st = 0;
    r.susp |= 0x10u;
    g_sys->holly.raise(kDone[ch]);
}

void g2_transfer(unsigned ch) {
    G2Channel& r = g_g2[ch];
    if (r.en != 1u)
        return;
    std::uint32_t src = r.star, dst = r.stag;
    const std::uint32_t len = r.len & 0x7FFFFFFFu;
    const bool src_ok = ((src >> 26) & 7u) == 3u || ((src >> 26) & 7u) == 1u;
    const bool dst_ok = ((dst >> 26) & 7u) == 0u || ((dst >> 26) & 7u) == 3u;
    if (!src_ok || !dst_ok || len == 0) {
        r.st = 0;
        r.en = 0;
        g_sys->holly.raise(dream::holly::Irq::ErrAicaIllegalAddr);
        return;
    }
    if (r.dir == 1u)
        std::swap(src, dst);
    // One block copy where the ranges allow it; the same bytes as word by word.
    if (!g_sys->memory.copy32(dst, src, len))
        for (std::uint32_t i = 0; i < len; i += 4) g_sys->memory.write32(dst + i, g_sys->memory.read32(src + i));
    r.st = 1u;
    r.susp &= ~0x10u;
    const std::uint64_t cycles = len * 4;
    if (cycles <= 512)
        g2_finish(ch);
    else
        g_sys->sched.request(r.event, cycles);
}

// The channel a register address names: syG2's register base (its state's first word) + 32 a
// channel.
unsigned g2_channel(Memory& m, std::uint32_t reg) {
    return ((reg - m.read32(m.read32(kG2Records))) >> 5) & 3u;
}

}  // namespace

// 0x8C0755B4 (r4 = the channel's record, r5 = mode): syG2's DMA start. Interrupts masked; busy
// (the status call) returns 0x00090180; else the source written back from the cache (the CPU's
// operation, still guest code), the channel programmed (enable off, sound-RAM and SH-4 addresses,
// length, direction, trigger, enable on), the record marked running and the start counted, and
// the start itself, which copies.
bool g2_start(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    poll(c, m, 0x8C0755B4u);
    push(c, m, c.r[14]);
    push(c, m, c.r[13]);
    push(c, m, c.r[12]);
    push(c, m, c.pr);
    c.r[15] -= 12;
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[12] = 0;
    m.write32(c.r[15] + 4, c.r[5]);
    c.r[0] = (read_sr(c) >> 4) & 15u;
    m.write32(c.r[15] + 8, c.r[0]);
    c.r[0] = (read_sr(c) & c.r[3]) | 240u;
    write_sr(c, c.r[0]);
    c.r[3] = kG2Records;
    c.r[14] = c.r[4];
    c.r[2] = m.read32(c.r[14]) << 4;
    c.r[13] = m.read32(m.read32(c.r[3]));
    c.t = c.r[2] >> 31;
    c.r[2] <<= 1;
    c.r[13] += c.r[2];
    c.r[2] = 0x8C0333F4u;
    c.cycles += dream::sh4::cost(29);
    c.pr = 0x8C0755EEu;
    c.r[5] = c.r[15];
    c.pc = 0x8C0755EAu;
    dream::gen::fn_8c0333f4(c, m);
    c.r[0] = m.read32(c.r[15]);
    c.t = c.r[0] == 1u;
    c.cycles += dream::sh4::cost(3);
    if (c.t) {
        c.r[12] = 0x00090180u;
        c.cycles += dream::sh4::cost(3);
    } else {
        c.cycles += dream::sh4::cost(1);
        c.r[3] = 0x1FFFFFFFu;
        c.r[2] = 0xA0000000u;
        c.r[4] = (m.read32(c.r[14] + 8) & c.r[3]) | c.r[2];
        c.r[5] = m.read32(c.r[14] + 12);
        c.r[3] = 0x8C0133BEu;
        c.r[1] = 0x7FFFFFE0u;
        c.cycles += dream::sh4::cost(10);
        c.pr = 0x8C07560Eu;
        c.r[5] &= c.r[1];
        c.pc = 0x8C07560Au;
        dream::gen::fn_8c0133be(c, m);
        G2Channel& g = g_g2[g2_channel(m, c.r[13])];
        device_access();  // the channel's registers
        c.r[2] = 0;
        g.en = 0;
        c.r[3] = m.read32(c.r[14] + 4);
        g.stag = c.r[3] & 0x1FFFFFE0u;
        c.r[2] = m.read32(c.r[14] + 8);
        g.star = c.r[2] & 0x1FFFFFE0u;
        c.r[3] = m.read32(c.r[14] + 12);
        g.len = c.r[3] & 0x9FFFFFFFu;
        c.r[2] = m.read32(c.r[14] + 16);
        g.dir = c.r[2] & 1u;
        c.r[1] = m.read32(c.r[15] + 4);
        c.t = c.r[1] == 0;
        c.cycles += dream::sh4::cost(13);
        if (!c.t) {
            c.cycles += dream::sh4::cost(1);
            c.r[3] = 2;
            c.cycles += dream::sh4::cost(1);
        } else {
            c.cycles += dream::sh4::cost(2);
            c.r[3] = 0;
        }
        device_access();
        c.r[1] = m.read32(c.r[14] + 20);
        c.r[3] |= c.r[1];
        g.tsel = c.r[3] & 1u;
        c.r[4] = 1;
        g.en = 1;
        c.r[3] = kG2Records;
        c.r[1] = 220;
        m.write32(c.r[14] + 44, c.r[4]);
        c.r[2] = m.read32(c.r[14]);
        c.r[0] = m.read32(c.r[3]);
        c.r[2] <<= 2;
        c.r[1] += c.r[0];
        c.r[2] += c.r[1];
        c.r[0] = m.read32(c.r[2]) + 1u;
        m.write32(c.r[2], c.r[0]);
        if (!(g.st & 1u))  // the start: the copy
            g2_transfer(static_cast<unsigned>(&g - g_g2.data()));
        c.cycles += dream::sh4::cost(17);
    }
    c.r[0] = m.read32(c.r[15] + 8);
    c.r[3] = read_sr(c);
    c.r[2] = static_cast<std::uint32_t>(-241);
    c.r[0] = ((c.r[0] & 15u) << 4) | (c.r[3] & c.r[2]);
    c.r[3] &= c.r[2];
    write_sr(c, c.r[0]);
    c.r[0] = c.r[12];
    c.r[15] += 12;
    c.pr = pop(c, m);
    c.r[12] = pop(c, m);
    c.r[13] = pop(c, m);
    c.cycles += dream::sh4::cost(16);
    c.r[14] = pop(c, m);
    return true;
}

// 0x8C0333F4 (r4 = the channel's record, r5 = out): whether its DMA is still running.
bool g2_status(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    poll(c, m, 0x8C0333F4u);
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[0] = (read_sr(c) >> 4) & 15u;
    c.r[6] = c.r[0];
    c.r[0] = (read_sr(c) & c.r[3]) | 240u;
    write_sr(c, c.r[0]);
    c.r[3] = kG2Records;
    c.r[4] = m.read32(c.r[4]) << 4;
    c.r[2] = m.read32(c.r[3]);
    c.r[1] = m.read32(c.r[2]);
    c.t = c.r[4] >> 31;
    c.r[4] <<= 1;
    c.r[2] = c.r[1];
    c.r[4] += c.r[2];
    device_access();  // the channel's start bit
    c.r[0] = g_g2[g2_channel(m, c.r[4])].st;
    c.t = (c.r[0] & 1u) == 0;
    c.cycles += dream::sh4::cost(22);
    if (c.t) {
        c.cycles += dream::sh4::cost(1);
        c.r[1] = 0;
        c.cycles += dream::sh4::cost(1);
    } else {
        c.cycles += dream::sh4::cost(2);
        c.r[1] = 1;
    }
    c.r[0] = c.r[6];
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[2] = read_sr(c);
    m.write32(c.r[5], c.r[1]);
    c.r[0] = ((c.r[0] & 15u) << 4) | (c.r[2] & c.r[3]);
    c.r[2] &= c.r[3];
    write_sr(c, c.r[0]);
    c.cycles += dream::sh4::cost(12);
    c.r[0] = 0;
    return true;
}

// 0x8C051950 (r4 = the channel's record): syG2's end of a DMA, from the end event or by hand from
// the sd library. Interrupts masked; the channel's enable cleared; the record marked done, the end
// counted; the event acknowledged and the status read back twice into a stack word, as the
// original does; the record's callback called.
bool g2_end(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    poll(c, m, 0x8C051950u);
    push(c, m, c.r[14]);
    push(c, m, c.pr);
    c.r[15] -= 8;
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[0] = (read_sr(c) >> 4) & 15u;
    m.write32(c.r[15] + 4, c.r[0]);
    c.r[0] = (read_sr(c) & c.r[3]) | 240u;
    write_sr(c, c.r[0]);
    c.r[6] = kG2Records;
    c.r[14] = c.r[4];
    c.r[3] = m.read32(c.r[14]) << 4;
    c.r[5] = 0;
    c.r[4] = m.read32(m.read32(c.r[6]));
    c.t = c.r[3] >> 31;
    c.r[3] <<= 1;
    c.r[4] += c.r[3];
    device_access();  // the channel's enable
    g_g2[g2_channel(m, c.r[4])].en = 0;
    c.r[2] = 236;
    m.write32(c.r[14] + 44, c.r[5]);
    c.r[5] = 1;
    m.write32(c.r[14] + 48, c.r[5]);
    c.r[0] = m.read32(c.r[6]);
    c.r[3] = m.read32(c.r[14]);
    c.r[2] += c.r[0];
    c.r[4] = 0xA05F6900u;
    c.r[3] = (c.r[3] << 2) + c.r[2];
    c.r[1] = m.read32(c.r[3]) + 1u;
    m.write32(c.r[3], c.r[1]);
    c.r[3] = m.read32(c.r[14]) + 15u;
    c.r[5] = c.r[3] < 32 ? (1u << c.r[3]) : 0u;
    irq_ack(0xA05F6900u, c.r[5]);
    c.r[2] = irq_status_view();
    m.write32(c.r[15], c.r[2]);
    c.r[3] = irq_status_view();
    m.write32(c.r[15], c.r[3]);
    c.r[2] = m.read32(c.r[14] + 28);
    c.t = c.r[2] == 0;
    c.cycles += dream::sh4::cost(48);
    if (c.t) {
        c.cycles += dream::sh4::cost(1);
    } else {
        c.r[2] = m.read32(c.r[14] + 28);
        c.cycles += dream::sh4::cost(3);
        c.pr = 0x8C0519B6u;
        c.r[4] = m.read32(c.r[14] + 32);
        c.pc = 0x8C0519B2u;
        dream::sh4::call_indirect(c, m, c.r[2]);
    }
    c.r[0] = m.read32(c.r[15] + 4);
    c.r[3] = read_sr(c);
    c.r[2] = static_cast<std::uint32_t>(-241);
    c.r[0] = ((c.r[0] & 15u) << 4) | (c.r[3] & c.r[2]);
    c.r[3] &= c.r[2];
    write_sr(c, c.r[0]);
    c.r[15] += 8;
    c.pr = pop(c, m);
    c.cycles += dream::sh4::cost(13);
    c.r[14] = pop(c, m);
    return true;
}

// 0x8C075320 (r4 = the channel's record): syG2's channel init, from its init 0x8C051AE8. The
// channel's slot in syG2's state (52 bytes a channel from state + 4) set to {channel, 0 ...,
// +24 = 3, ...}, the channel's DMA enable cleared. One block.
bool g2_chan_init(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    poll(c, m, 0x8C075320u);
    c.r[15] -= 4;
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[0] = (read_sr(c) >> 4) & 15u;
    m.write32(c.r[15], c.r[0]);
    c.r[0] = (read_sr(c) & c.r[3]) | 240u;
    write_sr(c, c.r[0]);
    c.r[7] = m.read32(c.r[4]);
    c.r[6] = 52;
    c.r[5] = kG2Records;
    c.macl = c.r[7] * c.r[6];
    c.r[2] = m.read32(c.r[5]) + 4u;
    c.r[3] = c.macl + c.r[2];
    m.write32(c.r[3], c.r[7]);
    c.r[7] = 0;
    // The slot's words, each address computed afresh as the original does: +40, +44, +48, +4 ...
    // +20 = 0, +24 = 3, +28, +32 = 0. The registers end as the last computation leaves them.
    static constexpr std::uint32_t kWords[] = {40, 44, 48, 4, 8, 12, 16, 20, 24, 28};
    for (std::uint32_t off : kWords) {
        c.macl = m.read32(c.r[4]) * c.r[6];
        c.r[2] = c.macl + m.read32(c.r[5]) + 4u;
        c.r[3] = m.read32(c.r[5]) + 4u;
        m.write32(c.r[2] + off, off == 24 ? 3u : c.r[7]);
    }
    c.r[1] = 3;
    c.r[3] = m.read32(c.r[4]);
    c.r[2] = m.read32(c.r[5]);
    c.macl = c.r[3] * c.r[6];
    c.r[2] += 4;
    c.r[6] = c.macl + c.r[2];
    m.write32(c.r[6] + 32, c.r[7]);
    c.r[1] = m.read32(c.r[4]) << 4;
    c.r[3] = m.read32(m.read32(c.r[5]));
    c.t = c.r[1] >> 31;
    c.r[1] = (c.r[1] << 1) + c.r[3];
    c.r[4] = c.r[1];
    device_access();  // the channel's DMA enable
    g_g2[g2_channel(m, c.r[4])].en = 0;
    c.r[0] = m.read32(c.r[15]);
    c.r[2] = read_sr(c);
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[0] = ((c.r[0] & 15u) << 4) | (c.r[2] & c.r[3]);
    c.r[2] &= c.r[3];
    write_sr(c, c.r[0]);
    c.r[0] = c.r[7];
    c.cycles += dream::sh4::cost(120);
    c.r[15] += 4;
    return true;
}

// 0x8C0754D0 (r4, r5 = sound-RAM bounds): syG2's protection word, 0x4659 << 16 with the two
// bounds' bits 20-26 as bytes; kept natively (nothing reads it back) and at 0x8C115EC0.
bool g2_protect(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    poll(c, m, 0x8C0754D0u);
    c.r[3] = static_cast<std::uint32_t>(-241);
    c.r[0] = (read_sr(c) >> 4) & 15u;
    c.r[6] = c.r[0];
    c.r[0] = (read_sr(c) & c.r[3]) | 240u;
    write_sr(c, c.r[0]);
    c.r[7] = 0x07F00000u;
    c.r[3] = 0x46590000u;
    c.r[4] = ((c.r[4] & c.r[7]) >> 20) << 8;
    c.r[2] = 0xA05F78BCu;
    c.r[7] = (c.r[5] & c.r[7]) >> 20;
    c.r[5] = c.r[4] | c.r[7] | c.r[3];
    device_access();
    c.r[0] = c.r[6] & 15u;
    c.r[3] = read_sr(c);
    c.r[2] = static_cast<std::uint32_t>(-241);
    c.r[0] <<= 4;
    c.r[1] = 0x8C115EC0u;
    c.r[3] &= c.r[2];
    c.r[0] |= c.r[3];
    m.write32(c.r[1], c.r[5]);
    write_sr(c, c.r[0]);
    c.cycles += dream::sh4::cost(40);
    c.r[0] = 0;
    return true;
}

void native_snd_start(dream::System& sys) {
    g_sys = &sys;
    for (unsigned ch = 0; ch < 4; ++ch) {
        g_g2[ch] = G2Channel{};
        g_g2[ch].event = sys.sched.add("native g2", [ch](std::uint64_t, std::uint64_t) { g2_finish(ch); });
    }
}

bool g2_resume(Ctx& c, Memory& m) {
    return all_channels(c, m, 0x8C0519DCu, false);
}

bool g2_suspend(Ctx& c, Memory& m) {
    return all_channels(c, m, 0x8C051A20u, true);
}

bool g2_fifo_wait(Ctx& c, Memory& m) {
    return fifo_wait(c, m, 0x8C051A64u, 0x8C075690u, 268);
}

bool g2_fifo_wait2(Ctx& c, Memory& m) {
    return fifo_wait(c, m, 0x8C051914u, 0x8C0751F4u, 272);
}

}  // namespace rez

DC_HOOK_ENTRY(g2_resume) {
    return rez::g2_resume(c, m);
}

DC_HOOK_ENTRY(g2_suspend) {
    return rez::g2_suspend(c, m);
}

DC_HOOK_ENTRY(g2_fifo_wait) {
    return rez::g2_fifo_wait(c, m);
}

DC_HOOK_ENTRY(g2_fifo_wait2) {
    return rez::g2_fifo_wait2(c, m);
}

DC_HOOK_ENTRY(g2_start) {
    return rez::g2_start(c, m);
}

DC_HOOK_ENTRY(g2_status) {
    return rez::g2_status(c, m);
}

DC_HOOK_ENTRY(g2_end) {
    return rez::g2_end(c, m);
}

DC_HOOK_ENTRY(g2_chan_init) {
    return rez::g2_chan_init(c, m);
}

DC_HOOK_ENTRY(g2_protect) {
    return rez::g2_protect(c, m);
}
