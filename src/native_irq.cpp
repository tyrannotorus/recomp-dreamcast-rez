// Native interrupt plumbing: Shinobi's interrupt dispatcher, its chain registration and the
// acknowledgements, without Holly's interrupt registers. Pending events and enable masks live in
// holly::Intc, used only as the event store; nothing reads or writes them as registers.
// An interrupt can come at any poll and its entry saves every register, so each hook keeps the
// original's polls, registers and T, stack and RAM writes, and cycles block by block.
#include "native_irq.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"
#include "dreamcomp/hook.h"
#include "native_gfx.h"

namespace dream::gen {
void fn_8c0328d0(dream::sh4::Ctx& c, dream::Memory& m);  // syChain's walker: RAM only, stays recompiled
}

namespace rez {

using dream::Memory;
using dream::sh4::Ctx;
using u32 = std::uint32_t;
using s32 = std::int32_t;

namespace {

dream::System* g_sys = nullptr;

void poll(Ctx& c, Memory& m, u32 pc) {
    if (c.cycles >= c.next_event) {
        c.pc = pc;
        dream::sh4::deliver_irq(c, m);
    }
}

void push(Ctx& c, Memory& m, u32 v) {
    c.r[15] -= 4;
    m.write32(c.r[15], v);
}

u32 pop(Ctx& c, Memory& m) {
    const u32 v = m.read32(c.r[15]);
    c.r[15] += 4;
    return v;
}

// SH-4 shad: left for a non-negative count, arithmetic right for a negative one.
u32 shad(u32 v, u32 n) {
    const s32 k = static_cast<s32>(n);
    if (k >= 0)
        return v << (k & 31);
    if ((k & 31) == 0)
        return static_cast<s32>(v) < 0 ? 0xFFFFFFFFu : 0u;
    return static_cast<u32>(static_cast<s32>(v) >> ((-k) & 31));
}

// ISTNRM as the event store shows it: the normal events and the two summary bits.
u32 istnrm_view() {
    const dream::holly::Intc& h = g_sys->holly;
    u32 v = h.istnrm & 0x3FFFFFFFu;
    if (h.istext)
        v |= 1u << 30;
    if (h.isterr)
        v |= 1u << 31;
    return v;
}

// The registers the original's register accesses named, kept natively.
constexpr u32 kIstNrm = 0xA05F6900u, kIstErr = 0xA05F6908u, kIml4Nrm = 0xA05F6920u, kIml4Ext = 0xA05F6924u,
              kIml6Nrm = 0xA05F6930u, kIml6Err = 0xA05F6938u;

u32* mask_field(u32 reg) {
    dream::holly::Intc& h = g_sys->holly;
    switch (reg) {
        case kIml4Nrm: return &h.iml4nrm;
        case kIml4Ext: return &h.iml4ext;
        case kIml6Nrm: return &h.iml6nrm;
        case kIml6Err: return &h.iml6err;
        default: return nullptr;
    }
}

// A mask register's read-modify-write ("or"), natively: the new value.
u32 mask_or(u32 reg, u32 bits) {
    device_access();
    u32* f = mask_field(reg);
    *f |= bits;
    g_sys->holly.recompute();
    return *f;
}

std::uint64_t g_guard_hits = 0;

// syChain's walker for one event, called as the dispatcher's bsr calls it.
void walk(Ctx& c, Memory& m, u32 bsr, u32 ret) {
    c.pr = ret;
    c.pc = bsr;
    dream::gen::fn_8c0328d0(c, m);
}

}  // namespace

// 0x8C0328F6 (r4 = INTEVT): Holly's level-6 events (errors, then normal bits 0-11 and 19-21) or
// level-4 events (normal bits 12-18, else external bits 0-3), each through syChain's walker in
// that order.
bool irq_dispatch(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    const dream::holly::Intc& h = g_sys->holly;
    poll(c, m, 0x8C0328F6u);
    push(c, m, c.r[14]);
    c.r[0] = static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(c.r[4])));
    push(c, m, c.r[13]);
    c.r[3] = static_cast<u32>(-4);
    push(c, m, c.r[12]);
    c.r[0] = shad(c.r[0], c.r[3]);
    push(c, m, c.r[11]);
    c.t = c.r[0] == 50u;
    push(c, m, c.r[10]);
    c.r[11] = 0;
    push(c, m, c.r[9]);
    push(c, m, c.pr);
    c.cycles += dream::sh4::cost(14);
    c.r[14] = 1;
    if (c.t)
        goto level6;
    c.t = c.r[0] == 54u;
    c.cycles += dream::sh4::cost(2);
    if (c.t) {
        c.cycles += dream::sh4::cost(1);
        goto level4;
    }
    c.t = c.r[0] == 58u;
    c.cycles += dream::sh4::cost(2);
    if (!c.t)
        c.cycles += dream::sh4::cost(1);
    c.cycles += dream::sh4::cost(2);
    goto done;

level6:
    c.r[0] = (read_sr(c) & static_cast<u32>(-241)) | 96u;
    write_sr(c, c.r[0]);
    c.r[0] = 0xA05F6908u;
    c.r[1] = 0xA05F6938u;
    device_access();
    c.r[10] = h.isterr;
    c.r[3] = h.iml6err;
    c.r[10] &= c.r[3];
    c.r[2] = static_cast<u32>(-241);
    c.t = c.r[10] == 0;
    c.cycles += dream::sh4::cost(12);
    if (c.t) {
        c.cycles += dream::sh4::cost(1);
        goto normal6;
    }
    c.r[9] = 3072;
    c.r[13] = c.r[11];
    c.r[12] = 8;
    c.cycles += dream::sh4::cost(3);
    for (;;) {
        c.r[3] = shad(c.r[14], c.r[13]);
        c.t = (c.r[3] & c.r[10]) == 0;
        c.cycles += dream::sh4::cost(4);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
        } else {
            c.r[4] = c.r[13] << 4;
            c.cycles += dream::sh4::cost(5);
            c.r[4] += c.r[9];
            walk(c, m, 0x8C032960u, 0x8C032964u);
        }
        c.r[13] += 1;
        c.t = c.r[13] >= c.r[12];
        c.cycles += dream::sh4::cost(3);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, 0x8C032952u);
    }

normal6:
    c.r[1] = 0xA05F6900u;
    c.r[2] = 0xA05F6930u;
    device_access();
    c.r[10] = istnrm_view();
    c.r[3] = h.iml6nrm;
    c.r[10] &= c.r[3];
    c.t = c.r[10] == 0;
    c.cycles += dream::sh4::cost(7);
    if (c.t) {
        c.cycles += dream::sh4::cost(1);
        goto done;
    }
    c.r[13] = 2304;
    c.r[12] = c.r[11];
    c.r[9] = 12;
    c.cycles += dream::sh4::cost(3);
    for (;;) {
        c.r[3] = shad(c.r[14], c.r[12]);
        c.t = (c.r[3] & c.r[10]) == 0;
        c.cycles += dream::sh4::cost(4);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
        } else {
            c.r[4] = c.r[12] << 4;
            c.cycles += dream::sh4::cost(5);
            c.r[4] += c.r[13];
            walk(c, m, 0x8C03298Cu, 0x8C032990u);
        }
        c.r[12] += 1;
        c.t = c.r[12] >= c.r[9];
        c.cycles += dream::sh4::cost(3);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, 0x8C03297Eu);
    }
    c.r[12] = 19;
    c.r[11] = 22;
    c.cycles += dream::sh4::cost(2);
    for (;;) {
        c.r[3] = shad(c.r[14], c.r[12]);
        c.t = (c.r[3] & c.r[10]) == 0;
        c.cycles += dream::sh4::cost(4);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
        } else {
            c.r[4] = (c.r[12] - 7u) << 4;
            c.cycles += dream::sh4::cost(6);
            c.r[4] += c.r[13];
            walk(c, m, 0x8C0329AAu, 0x8C0329AEu);
        }
        c.r[12] += 1;
        c.t = c.r[12] >= c.r[11];
        c.cycles += dream::sh4::cost(3);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, 0x8C03299Au);
    }
    c.cycles += dream::sh4::cost(2);
    goto done;

level4:
    c.r[0] = (read_sr(c) & static_cast<u32>(-241)) | 64u;
    c.r[2] = static_cast<u32>(-241);
    write_sr(c, c.r[0]);
    c.r[0] = 0xA05F6900u;
    c.r[1] = 0xA05F6920u;
    device_access();
    c.r[12] = istnrm_view();
    c.r[3] = h.iml4nrm;
    c.r[12] &= c.r[3];
    c.r[12] >>= 12;
    c.t = c.r[12] == 0;
    c.cycles += dream::sh4::cost(15);
    if (c.t) {
        c.cycles += dream::sh4::cost(1);
        goto external4;
    }
    c.r[10] = 128;
    c.r[9] = 2560;
    c.cycles += dream::sh4::cost(4);
    c.r[13] = c.r[11];
    for (;;) {
        // the loop test at 0x8C0329F4
        c.r[2] = shad(c.r[14], c.r[13]);
        c.t = c.r[2] == c.r[10];
        c.cycles += dream::sh4::cost(4);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, 0x8C0329DEu);
        c.r[3] = shad(c.r[14], c.r[13]);
        c.t = (c.r[3] & c.r[12]) == 0;
        c.cycles += dream::sh4::cost(4);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
        } else {
            c.r[4] = c.r[13] << 4;
            c.t = c.r[4] >> 31;
            c.r[4] <<= 1;
            c.cycles += dream::sh4::cost(6);
            c.r[4] += c.r[9];
            walk(c, m, 0x8C0329EEu, 0x8C0329F2u);
        }
        c.r[13] += 1;
        c.cycles += dream::sh4::cost(1);
    }
    c.cycles += dream::sh4::cost(2);
    goto done;

external4:
    c.r[1] = 0xA05F6904u;
    c.r[2] = 0xA05F6924u;
    device_access();
    c.r[10] = h.istext;
    c.r[3] = h.iml4ext;
    c.r[12] = 2816;
    c.r[10] &= c.r[3];
    c.cycles += dream::sh4::cost(8);
    c.r[13] = c.r[11];
    for (;;) {
        // the loop test at 0x8C032A4E
        c.r[0] = shad(c.r[14], c.r[13]);
        c.t = c.r[0] == 16u;
        c.cycles += dream::sh4::cost(4);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, 0x8C032A38u);
        c.r[2] = shad(c.r[14], c.r[13]);
        c.t = (c.r[2] & c.r[10]) == 0;
        c.cycles += dream::sh4::cost(4);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
        } else {
            c.r[4] = c.r[13] << 4;
            c.t = c.r[4] >> 31;
            c.r[4] <<= 1;
            c.cycles += dream::sh4::cost(6);
            c.r[4] += c.r[12];
            walk(c, m, 0x8C032A48u, 0x8C032A4Cu);
        }
        c.r[13] += 1;
        c.cycles += dream::sh4::cost(1);
    }

done:
    c.pr = pop(c, m);
    c.r[0] = 0;
    c.r[9] = pop(c, m);
    c.r[10] = pop(c, m);
    c.r[11] = pop(c, m);
    c.r[12] = pop(c, m);
    c.r[13] = pop(c, m);
    c.cycles += dream::sh4::cost(9);
    c.r[14] = pop(c, m);
    return true;
}

// An acknowledgement: the written ones cleared from the normal or the error events.
void irq_ack(u32 reg, u32 bits) {
    device_access();
    dream::holly::Intc& h = g_sys->holly;
    if (reg == kIstNrm)
        h.istnrm &= ~bits;
    else if (reg == kIstErr)
        h.isterr &= ~bits;
    h.recompute();
}

void irq_mask(u32 reg, u32 value) {
    device_access();
    *mask_field(reg) = value;
    g_sys->holly.recompute();
}

u32 irq_mask_value(u32 reg) {
    return *mask_field(reg);
}

u32 irq_status_view() {
    device_access();
    return istnrm_view();
}

// 0x8C06DEC0 (r4 = Kamui's event index): Kamui's interrupt entry. Its current index saved and set,
// the event acknowledged through Kamui's table (register, bits) at 0x8C115DD4, the DMA channel 2
// control cleared for a ch2 end (an on-chip module: kept as the CPU's), Kamui's handler called.
bool irq_kamui(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    poll(c, m, 0x8C06DEC0u);
    push(c, m, c.r[14]);
    push(c, m, c.r[13]);
    push(c, m, c.pr);
    c.r[15] -= 4;
    c.r[14] = 0x8C13F370u;
    c.r[5] = 15;
    c.t = c.r[4] > c.r[5];
    c.r[3] = m.read32(c.r[14] + 4);
    m.write32(c.r[15], c.r[3]);
    c.cycles += dream::sh4::cost(11);
    m.write32(c.r[14] + 4, c.r[5]);
    if (c.t) {
        c.r[2] = 32;
        c.t = c.r[4] >= c.r[2];
        c.cycles += dream::sh4::cost(3);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
            c.t = c.r[4] > c.r[5];
            c.cycles += dream::sh4::cost(2);
            if (!c.t) {
                c.cycles += dream::sh4::cost(1);
            } else {
                for (;;) {  // an index past 31: the original stops here
                    c.cycles += dream::sh4::cost(2);
                    poll(c, m, 0x8C06DF20u);
                }
            }
        } else {
            c.r[5] = c.r[4] - 16u;
            c.r[3] = 0x8C115DD4u;
            c.r[5] <<= 2;
            c.r[0] = c.r[4];
            c.r[5] <<= 1;
            c.t = c.r[0] == 18u;
            c.r[5] += c.r[3];
            c.cycles += dream::sh4::cost(10);
            m.write32(c.r[14] + 4, c.r[4]);
            if (c.t) {
                c.r[7] = 0xFFA0002Cu;
                c.r[2] = static_cast<u32>(-4);
                c.r[6] = m.read32(c.r[7]) & c.r[2];
                m.write32(c.r[7], c.r[6]);
                c.cycles += dream::sh4::cost(5);
            }
            c.r[2] = m.read32(c.r[5] + 4);
            c.r[3] = m.read32(c.r[5]);
            irq_ack(c.r[3], c.r[2]);
            c.r[13] = (c.r[4] << 4) + c.r[14] + 48u;
            c.r[2] = c.r[14] + 48u;
            c.r[3] = 0x8C13F5C0u;
            m.write32(c.r[3], c.r[4]);
            c.r[5] = m.read32(c.r[13] + 8);
            c.r[1] = m.read32(c.r[13]);
            c.cycles += dream::sh4::cost(15);
            c.pr = 0x8C06DF18u;
            c.r[4] = c.r[3];
            c.pc = 0x8C06DF14u;
            dream::sh4::call_indirect(c, m, c.r[1]);
            c.cycles += dream::sh4::cost(2);
        }
    }
    c.r[3] = m.read32(c.r[15]);
    c.r[0] = 0;
    m.write32(c.r[14] + 4, c.r[3]);
    c.r[15] += 4;
    c.pr = pop(c, m);
    c.r[13] = pop(c, m);
    c.cycles += dream::sh4::cost(8);
    c.r[14] = pop(c, m);
    return true;
}

// 0x8C0326B0 (r4 event, r5 handler, r6 priority, r7 argument) -> the node's index, or 0: syChain's
// registration. Public events are remapped and the node inserted before the first of equal or
// higher priority (newest first among equals; priority 16 always at the head). Priority 16 also
// sets the event's enable mask and puts the dispatcher in the CPU's interrupt slot for its level.
bool irq_chain_add(Ctx& c, Memory& m) {
    if (!g_sys)
        return false;
    using dream::sh4::read_sr;
    using dream::sh4::write_sr;
    auto exts_w = [](u32 v) { return static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(v))); };
    poll(c, m, 0x8C0326B0u);
    push(c, m, c.r[14]);
    push(c, m, c.r[13]);
    push(c, m, c.r[12]);
    push(c, m, c.r[11]);
    push(c, m, c.r[10]);
    push(c, m, c.r[9]);
    push(c, m, c.r[8]);
    c.r[15] -= 8;
    c.r[3] = static_cast<u32>(-241);
    c.r[12] = 1;
    c.r[8] = 0x8C13CAA8u;
    c.r[10] = 0x8C13BAA8u;
    c.r[0] = (read_sr(c) >> 4) & 15u;
    m.write16(c.r[15] + 4, static_cast<std::uint16_t>(c.r[0]));
    c.r[0] = (read_sr(c) & c.r[3]) | 240u;
    write_sr(c, c.r[0]);
    // The public events 0x900-0x950 and the internal ones swap.
    static constexpr u32 kFrom[6] = {2304, 2320, 2336, 2352, 2368, 2384};
    static constexpr u32 kTo[6] = {2352, 2368, 2384, 2304, 2320, 2336};
    c.r[0] = exts_w(c.r[4]);
    c.cycles += dream::sh4::cost(25);
    {
        unsigned k = 0;
        for (; k < 6; ++k) {
            c.r[1] = kFrom[k];
            c.t = c.r[0] == c.r[1];
            if (k > 0)
                c.cycles += dream::sh4::cost(3);
            if (c.t)
                break;
        }
        if (k < 6) {
            c.cycles += dream::sh4::cost(1);
            c.r[4] = kTo[k];
            c.cycles += dream::sh4::cost(k == 5 ? 1 : 3);
        } else {
            c.cycles += dream::sh4::cost(2);
        }
    }
    // The list for the event, walked while its priorities are below the new one.
    c.r[3] = static_cast<u32>(-4);
    c.r[0] = shad(exts_w(c.r[4]), c.r[3]) << 2;
    c.r[14] = m.read32(c.r[8] + c.r[0]);
    c.cycles += dream::sh4::cost(7);
    c.r[9] = c.r[14];
    for (;;) {
        c.t = c.r[14] == 0;
        c.cycles += dream::sh4::cost(2);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
            break;
        }
        c.r[0] = m.read8(c.r[14] + 12) & 0xFFu;
        c.t = c.r[0] >= c.r[6];
        c.cycles += dream::sh4::cost(4);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, 0x8C03274Cu);
        c.r[9] = c.r[14];
        c.r[14] = m.read32(c.r[14] + 4);
        c.cycles += dream::sh4::cost(2);
    }
    // A free node, from index 1.
    c.r[11] = 256;
    c.r[13] = c.r[12];
    c.cycles += dream::sh4::cost(2);
    for (;;) {
        c.r[2] = (c.r[13] << 4) + c.r[10];
        c.r[0] = static_cast<u32>(static_cast<s32>(static_cast<std::int8_t>(m.read8(c.r[2] + 13))));
        c.t = c.r[0] == 0;
        c.cycles += dream::sh4::cost(7);
        if (c.t) {
            c.cycles += dream::sh4::cost(1);
            break;
        }
        c.r[13] += 1;
        c.t = c.r[13] >= c.r[11];
        c.cycles += dream::sh4::cost(3);
        if (c.t)
            break;
        c.cycles += dream::sh4::cost(1);
        poll(c, m, 0x8C032760u);
    }
    c.t = c.r[13] >= c.r[11];
    c.cycles += dream::sh4::cost(2);
    enum { Head, After, None } where;
    if (c.t) {
        c.cycles += dream::sh4::cost(2);
        where = None;
    } else {
        c.cycles += dream::sh4::cost(1);
        c.t = c.r[14] == 0;
        c.cycles += dream::sh4::cost(2);
        bool enable_path;
        if (!c.t) {
            c.cycles += dream::sh4::cost(1);
            enable_path = false;  // 0x8C032784
        } else {
            c.t = c.r[9] == 0;
            c.cycles += dream::sh4::cost(2);
            if (c.t) {
                c.cycles += dream::sh4::cost(1);
                enable_path = true;  // 0x8C03278A
            } else {
                enable_path = false;  // 0x8C032784
            }
        }
        if (!enable_path) {
            c.r[0] = c.r[6];
            c.t = c.r[0] == 16u;
            c.cycles += dream::sh4::cost(3);
            if (!c.t) {
                c.cycles += dream::sh4::cost(1);
                where = After;  // 0x8C03285E
            } else {
                enable_path = true;
            }
        }
        if (enable_path) {
            c.r[0] = c.r[6];
            c.t = c.r[0] == 16u;
            c.cycles += dream::sh4::cost(3);
            if (!c.t) {
                c.cycles += dream::sh4::cost(1);
            } else {
                // Priority 16: the event's enable, and the dispatcher in the level's slot.
                c.r[3] = static_cast<u32>(-4);
                c.r[0] = exts_w(c.r[4]);
                c.r[11] = 15;
                c.r[0] = shad(c.r[0], c.r[3]);
                c.r[9] = c.r[4] & 0xFFu;
                c.r[11] &= c.r[0];
                c.r[0] = exts_w(c.r[4]);
                c.r[3] = static_cast<u32>(-8);
                c.r[9] >>= 4;
                c.r[0] = shad(c.r[0], c.r[3]);
                c.t = c.r[9] & 1u;
                c.r[9] >>= 1;
                c.t = c.r[0] == 9u;
                c.cycles += dream::sh4::cost(14);
                int path = 4;  // none
                if (c.t) {
                    c.cycles += dream::sh4::cost(1);
                    path = 0;
                } else {
                    c.t = c.r[0] == 12u;
                    c.cycles += dream::sh4::cost(2);
                    if (c.t) {
                        c.cycles += dream::sh4::cost(1);
                        path = 1;
                    } else {
                        c.t = c.r[0] == 10u;
                        c.cycles += dream::sh4::cost(2);
                        if (c.t) {
                            c.cycles += dream::sh4::cost(1);
                            path = 2;
                        } else {
                            c.t = c.r[0] == 11u;
                            c.cycles += dream::sh4::cost(2);
                            if (c.t) {
                                c.cycles += dream::sh4::cost(1);
                                path = 3;
                            } else {
                                c.cycles += dream::sh4::cost(2);
                            }
                        }
                    }
                }
                if (path == 0) {  // 0x9xx: level 6, normal (0x9C0 and up: bits 19-21)
                    c.r[3] = 2496;
                    c.r[2] = exts_w(c.r[4]);
                    c.t = static_cast<s32>(c.r[2]) >= static_cast<s32>(c.r[3]);
                    c.cycles += dream::sh4::cost(4);
                    if (c.t) {
                        c.r[2] = kIml6Nrm;
                        c.r[11] += 7;
                        c.r[1] = shad(c.r[12], c.r[11]);
                        c.r[0] = mask_or(kIml6Nrm, c.r[1]);
                        c.cycles += dream::sh4::cost(9);
                    } else {
                        c.cycles += dream::sh4::cost(1);
                        c.r[2] = kIml6Nrm;
                        c.r[1] = shad(c.r[12], c.r[11]);
                        c.r[0] = mask_or(kIml6Nrm, c.r[1]);
                        c.cycles += dream::sh4::cost(7);
                    }
                } else if (path == 1) {  // 0xCxx: level 6, errors
                    c.r[2] = kIml6Err;
                    c.r[3] = shad(c.r[12], c.r[11]);
                    c.r[1] = mask_or(kIml6Err, c.r[3]);
                    c.cycles += dream::sh4::cost(6);
                }
                if (path == 0 || path == 1) {
                    c.r[3] = c.vbr;
                    c.r[0] = 548;
                    c.r[2] = 0x8C0328F6u;
                    m.write32(c.r[3] + c.r[0], c.r[2]);
                    c.cycles += dream::sh4::cost(6);
                } else if (path == 2 || path == 3) {  // 0xAxx / 0xBxx: level 4
                    if (path == 2) {
                        c.r[3] = kIml4Nrm;
                        c.r[9] += 12;
                        c.r[2] = shad(c.r[12], c.r[9]);
                        c.r[1] = mask_or(kIml4Nrm, c.r[2]);
                        c.cycles += dream::sh4::cost(9);
                    } else {
                        c.r[2] = kIml4Ext;
                        c.r[3] = shad(c.r[12], c.r[9]);
                        c.r[1] = mask_or(kIml4Ext, c.r[3]);
                        c.cycles += dream::sh4::cost(6);
                    }
                    c.r[3] = c.vbr;
                    c.r[0] = 556;
                    c.r[2] = 0x8C0328F6u;
                    m.write32(c.r[3] + c.r[0], c.r[2]);
                    c.cycles += dream::sh4::cost(4);
                }
            }
            where = Head;  // 0x8C032838
        }
    }
    if (where == Head) {
        c.r[11] = (c.r[13] << 4) + c.r[10];
        c.r[0] = exts_w(c.r[4]);
        c.r[3] = static_cast<u32>(-4);
        c.r[0] = shad(c.r[0], c.r[3]) << 2;
        m.write32(c.r[8] + c.r[0], c.r[11]);
        c.r[0] = c.r[6];
        m.write32(c.r[11] + 4, c.r[14]);
        m.write32(c.r[11], c.r[5]);
        m.write32(c.r[11] + 8, c.r[7]);
        m.write8(c.r[11] + 12, static_cast<std::uint8_t>(c.r[0]));
        c.r[0] = c.r[12];
        m.write8(c.r[11] + 13, static_cast<std::uint8_t>(c.r[0]));
        c.r[0] = c.r[4];
        c.cycles += dream::sh4::cost(19);
        m.write16(c.r[11] + 14, static_cast<std::uint16_t>(c.r[0]));
    } else if (where == After) {
        c.r[2] = exts_w(c.r[4]);
        c.r[3] = static_cast<u32>(-4);
        m.write32(c.r[15], c.r[2]);
        c.r[2] = shad(c.r[2], c.r[3]);
        c.r[0] = c.r[2] << 2;
        c.r[3] = m.read32(c.r[8] + c.r[0]);
        c.t = c.r[14] == c.r[3];
        c.cycles += dream::sh4::cost(9);
        if (c.t) {  // before the list's first node: the new head
            c.r[11] = (c.r[13] << 4) + c.r[10];
            c.r[3] = static_cast<u32>(-4);
            m.write32(c.r[11] + 4, c.r[14]);
            c.r[0] = shad(m.read32(c.r[15]), c.r[3]) << 2;
            c.cycles += dream::sh4::cost(11);
            m.write32(c.r[8] + c.r[0], c.r[11]);
        } else {
            c.cycles += dream::sh4::cost(1);
            c.r[11] = (c.r[13] << 4) + c.r[10];
            m.write32(c.r[9] + 4, c.r[11]);
            m.write32(c.r[11] + 4, c.r[14]);
            c.cycles += dream::sh4::cost(6);
        }
        c.r[0] = c.r[6];
        m.write32(c.r[11], c.r[5]);
        m.write32(c.r[11] + 8, c.r[7]);
        m.write8(c.r[11] + 12, static_cast<std::uint8_t>(c.r[0]));
        c.r[0] = c.r[12];
        m.write8(c.r[11] + 13, static_cast<std::uint8_t>(c.r[0]));
        c.r[0] = c.r[4];
        m.write16(c.r[11] + 14, static_cast<std::uint16_t>(c.r[0]));
        c.cycles += dream::sh4::cost(8);
    }
    if (where == None) {
        c.r[4] = 0;
        c.cycles += dream::sh4::cost(1);
    } else {
        c.cycles += dream::sh4::cost(2);
        c.r[4] = c.r[13];
    }
    c.r[0] = m.read16(c.r[15] + 4) & 0xFFFFu;
    c.r[3] = read_sr(c);
    c.r[2] = static_cast<u32>(-241);
    c.r[0] = (c.r[0] & 15u) << 4;
    c.r[3] &= c.r[2];
    c.r[0] |= c.r[3];
    write_sr(c, c.r[0]);
    c.r[0] = c.r[4];
    c.r[15] += 8;
    c.r[8] = pop(c, m);
    c.r[9] = pop(c, m);
    c.r[10] = pop(c, m);
    c.r[11] = pop(c, m);
    c.r[12] = pop(c, m);
    c.r[13] = pop(c, m);
    c.cycles += dream::sh4::cost(20);
    c.r[14] = pop(c, m);
    return true;
}

// syChain's delete (0x8C032A68) and finish (0x8C032C54) never run in Rez (shutdown only): guarded,
// so a run that reaches one says so; the original then runs.
bool irq_guard(Ctx& c) {
    if (g_sys) {
        if (g_guard_hits++ < 4)
            std::fprintf(stderr, "native irq: guarded function 0x%08X reached (pr 0x%08X)\n", c.pc, c.pr);
    }
    return false;
}

void native_irq_start(dream::System& sys) {
    g_sys = &sys;
}

}  // namespace rez

DC_HOOK_ENTRY(irq_dispatch) {
    return rez::irq_dispatch(c, m);
}
DC_HOOK_ENTRY(irq_kamui) {
    return rez::irq_kamui(c, m);
}
DC_HOOK_ENTRY(irq_chain_add) {
    return rez::irq_chain_add(c, m);
}
DC_HOOK_ENTRY(irq_guard) {
    (void)m;
    return rez::irq_guard(c);
}
