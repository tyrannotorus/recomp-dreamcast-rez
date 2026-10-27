// Native devices: the device registers Rez's libraries touch, answered by native code.
// All guest code runs on a NativeShim (src/shim_memory.h) that answers the register blocks listed
// in native_dev_start and passes everything else on. G2 DMA, the system block and the SH-4 DMAC are
// modelled here; the other blocks are plain stored registers or the runtime's own state. In the
// native build the blocks are unmapped, so an access the shim misses faults.
#include "native_dev.h"
#include "native_sound.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/aica/rtc.h"
#include "dream/runtime/holly/intc.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/system.h"
#include "native_gfx.h"
#include "shim_memory.h"

namespace rez {
namespace {

dream::System* g_sys = nullptr;

// G2 DMA, per channel 8 registers: STAG STAR LEN DIR TSEL EN ST SUSP; then ID, time-outs and
// protection from 0x80. A start copies the block and raises the end-of-DMA interrupt after the
// transfer time; the busy bit reads set until then.
class NativeG2 final : public dream::mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F7800u, kEnd = 0x005F7900u;

    void start(dream::System& sys) {
        sys_ = &sys;
        regs_.fill(0);
        regs_[0x80 / 4] = 0x12;
        for (unsigned ch = 0; ch < 4; ++ch) {
            regs_[ch * 8 + kSusp] = 0x10;  // idle: "DMA stopped"
            events_[ch] = sys.sched.add("native g2", [this, ch](std::uint64_t, std::uint64_t) { finish(ch); });
        }
    }

    std::uint32_t read(std::uint32_t p, unsigned) override { return regs_[((p - kBase) & 0xFCu) / 4]; }

    void write(std::uint32_t p, std::uint32_t v, unsigned) override {
        const std::uint32_t off = (p - kBase) & 0xFCu;
        if (off >= 0x80) {
            if (off != 0x80)
                regs_[off / 4] = v;
            return;
        }
        const unsigned ch = off / 0x20;
        std::uint32_t* r = &regs_[ch * 8];
        switch ((off & 0x1Cu) / 4) {
            case kSt:
                if ((v & 1u) && !(r[kSt] & 1u))
                    transfer(ch);
                return;
            case kSusp:
                r[kSusp] = (r[kSusp] & 0x10u) | (v & 1u);
                return;
            case kLen:
                r[kLen] = v & 0x9FFFFFFFu;
                return;
            case kEn:
            case kDir:
            case kTsel:
                r[(off & 0x1Cu) / 4] = v & 1u;
                return;
            default:  // STAG, STAR
                r[(off & 0x1Cu) / 4] = v & 0x1FFFFFE0u;
                return;
        }
    }

private:
    enum : unsigned { kStag, kStar, kLen, kDir, kTsel, kEn, kSt, kSusp };
    static constexpr dream::holly::Irq kDone[4] = {dream::holly::Irq::AicaDma, dream::holly::Irq::Ext1Dma,
                                                   dream::holly::Irq::Ext2Dma, dream::holly::Irq::DevDma};
    static constexpr std::uint64_t kCyclesPerByte = 4;  // 16 bits at 25 MHz, in 200 MHz cycles

    void transfer(unsigned ch) {
        std::uint32_t* r = &regs_[ch * 8];
        if (r[kEn] != 1u)
            return;
        std::uint32_t src = r[kStar], dst = r[kStag];
        const std::uint32_t len = r[kLen] & 0x7FFFFFFFu;
        const bool src_ok = ((src >> 26) & 7u) == 3u || ((src >> 26) & 7u) == 1u;
        const bool dst_ok = ((dst >> 26) & 7u) == 0u || ((dst >> 26) & 7u) == 3u;
        if (!src_ok || !dst_ok || len == 0) {
            r[kSt] = 0;
            r[kEn] = 0;
            sys_->holly.raise(dream::holly::Irq::ErrAicaIllegalAddr);
            return;
        }
        if (r[kDir] == 1u)
            std::swap(src, dst);
        for (std::uint32_t i = 0; i < len; i += 4) sys_->memory.write32(dst + i, sys_->memory.read32(src + i));
        r[kSt] = 1u;
        r[kSusp] &= ~0x10u;
        const std::uint64_t cycles = len * kCyclesPerByte;
        if (cycles <= 512)
            finish(ch);
        else
            sys_->sched.request(events_[ch], cycles);
    }

    void finish(unsigned ch) {
        std::uint32_t* r = &regs_[ch * 8];
        const std::uint32_t len = r[kLen] & 0x7FFFFFFFu;
        if (r[kLen] & 0x80000000u)
            r[kEn] = 0;
        r[kStar] += len;
        r[kStag] += len;
        r[kLen] = 0;
        r[kSt] = 0;
        r[kSusp] |= 0x10u;
        sys_->holly.raise(kDone[ch]);
    }

    dream::System* sys_ = nullptr;
    std::array<std::uint32_t, 0x100 / 4> regs_{};
    std::array<int, 4> events_{-1, -1, -1, -1};
};

// The system block (SB_*): stored registers with the hardware's reset values, read-only registers
// and write masks. The FIFO status reads empty and the TA FIFO has all eight slots free.
class NativeSb final : public dream::mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F6800u, kEnd = 0x005F6900u;
    enum : std::uint32_t {
        kC2dStat = 0x00, kC2dLen = 0x04, kC2dSt = 0x08, kSdStaw = 0x10, kSdBaaw = 0x14, kSdWlt = 0x18,
        kSdLas = 0x1C, kSdSt = 0x20, kDbReqM = 0x40, kBavlWc = 0x44, kC2dPryc = 0x48, kC2dMaxl = 0x4C,
        kSdDiv = 0x60, kTfRem = 0x80, kLmMode0 = 0x84, kLmMode1 = 0x88, kFfSt = 0x8C, kSfRes = 0x90,
        kSbRev = 0x9C, kRbSplt = 0xA0
    };

    NativeSb() {
        regs_[kC2dStat >> 2] = 0x10000000u;
        regs_[kSdStaw >> 2] = regs_[kSdBaaw >> 2] = 0x08000000u;
        regs_[kTfRem >> 2] = 8;
        regs_[kSbRev >> 2] = 0x0Bu;
    }

    std::uint32_t reg(std::uint32_t off) const { return regs_[off >> 2]; }

    std::uint32_t read(std::uint32_t p, unsigned) override {
        const std::uint32_t off = p - kBase;
        return off == kSfRes ? 0 : regs_[off >> 2];
    }

    void write(std::uint32_t p, std::uint32_t v, unsigned) override {
        const std::uint32_t off = p - kBase;
        std::uint32_t& r = regs_[off >> 2];
        switch (off) {
            case kC2dStat:
                r = (v & 0x03FFFFE0u) | 0x10000000u;
                break;
            case kC2dLen:
                r = v & 0x00FFFFE0u;
                break;
            case kC2dSt:
                if (v & 1u) {
                    // Kamui's buffer send is native, so a register start has no path.
                    static unsigned said = 0;
                    if (said++ < 4)
                        std::fprintf(stderr, "native devices: a channel-2 DMA start by register (C2DSTAT %08x, len %u) "
                                             "has no native path\n", regs_[kC2dStat >> 2], regs_[kC2dLen >> 2]);
                    g_sys->holly.raise(dream::holly::Irq::Ch2Dma);
                }
                break;
            case kSdStaw:
            case kSdBaaw:
                r = (v & 0x07FFFFE0u) | 0x08000000u;
                break;
            case kSdWlt:
            case kSdLas:
            case kDbReqM:
            case kLmMode0:
            case kLmMode1:
                r = v & 1u;
                break;
            case kSdSt:
                break;  // Sort-DMA: not used by Rez; the start bit reads back clear
            case kBavlWc:
                r = v & 0x1Fu;
                break;
            case kC2dPryc:
                r = v & 0xFu;
                break;
            case kC2dMaxl:
                r = v & 3u;
                break;
            case kRbSplt:
                r = v & 0x80000000u;
                break;
            case kSfRes:
            case kSdDiv:
            case kTfRem:
            case kFfSt:
            case kSbRev:
                break;  // read-only, or a soft reset Rez never asks for
            default:
                r = v;
                break;
        }
    }

private:
    std::array<std::uint32_t, 0x100 / 4> regs_{};
};

// The SH-4 DMAC: stored registers. CHCR's TE bit is cleared by writing 0 and unaffected by 1, and
// DMAOR's NMIF and AE only clear. Rez starts no transfer here (channel 2 is native, above).
class NativeDmac final : public dream::mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0xFFA00000u, kEnd = 0xFFA00044u;

    std::uint32_t read(std::uint32_t a, unsigned) override {
        const std::uint32_t off = a - kBase;
        if (off == 0x40)
            return dmaor_;
        return off < 0x40 ? ch_[off / 4] : 0;
    }

    void write(std::uint32_t a, std::uint32_t v, unsigned) override {
        const std::uint32_t off = a - kBase;
        if (off == 0x40) {
            dmaor_ = (v & 0xFFFF8201u) | (dmaor_ & v & 6u);
            return;
        }
        if (off >= 0x40)
            return;
        std::uint32_t& r = ch_[off / 4];
        switch (off % 0x10) {
            case 0x00:
            case 0x04:
                r = v;
                return;
            case 0x08:
                r = v & 0x00FFFFFFu;
                return;
            default: {
                const unsigned n = off / 0x10;
                const std::uint32_t mask = n < 2 ? 0xFF0FFFF7u : 0xFF0AFFF7u;
                r = (v & mask & ~2u) | (r & v & 2u);
                if ((r & 1u) && (dmaor_ & 1u) && ((r >> 8) & 0xFu) == 4) {
                    static unsigned said = 0;
                    if (said++ < 4)
                        std::fprintf(stderr, "native devices: DMAC channel %u auto-request transfer has no native path\n", n);
                }
                return;
            }
        }
    }

private:
    std::array<std::uint32_t, 16> ch_{};
    std::uint32_t dmaor_ = 0;
};

// Maple's registers (0x005F6C00): plain registers, counted. The native peripherals replace the
// Maple stack; only Shinobi's hardware init (0x8C051AE8) resets seven of them at boot.
class MapleRegs final : public dream::mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F6C00u, kEnd = 0x005F6D00u;

    std::uint32_t read(std::uint32_t a, unsigned size) override { return regs_.read(a, size); }
    void write(std::uint32_t a, std::uint32_t v, unsigned size) override { regs_.write(a, v, size); }

private:
    dream::mem::RegisterFile regs_{kBase, 0x100 / 4};
};

NativeG2 g_g2;
MapleRegs g_maple_regs;
NativeSb g_sb;
NativeDmac g_dmac;
dream::mem::RegisterFile g_g1(0x005F7400u, 0x100 / 4), g_pvrdma(0x005F7C00u, 0x100 / 4),
    g_dma_triggers(0x005F6940u, 0x20 / 4), g_scif(0xFFE80000u, 0x28 / 4);

struct Block {
    std::uint32_t lo, hi;
    dream::mem::MmioHandler* h;
};
std::vector<Block> g_blocks;

class NativeShim final : public ShimMemory {
public:
    using ShimMemory::ShimMemory;

protected:
    bool shim_read(std::uint32_t p, unsigned size, std::uint32_t& v) override {
        for (const Block& b : g_blocks)
            if (p >= b.lo && p < b.hi) {
                device_access();
                v = b.h->read(p, size);
                return true;
            }
        return false;
    }
    bool shim_write(std::uint32_t p, unsigned size, std::uint32_t v) override {
        for (const Block& b : g_blocks)
            if (p >= b.lo && p < b.hi) {
                device_access();
                b.h->write(p, v, size);
                return true;
            }
        return false;
    }
};

}  // namespace

void native_dev_start(dream::System& sys) {
    g_sys = &sys;
    if (!sys.pvr_core || !sys.rtc || !sys.aica)
        return;
    g_g2.start(sys);
    g_blocks = {
        {NativeG2::kBase, NativeG2::kEnd, &g_g2},
        {NativeSb::kBase, NativeSb::kEnd, &g_sb},
        {dream::holly::Intc::kBase, dream::holly::Intc::kEnd, &sys.holly},
        {0x005F6940u, 0x005F6960u, &g_dma_triggers},
        {0x005F7400u, 0x005F7500u, &g_g1},
        {0x005F7C00u, 0x005F7D00u, &g_pvrdma},
        // The video timing registers sit inside the PVR's block; they come first.
        {dream::pvr::Spg::kRegLo, dream::pvr::Spg::kRegHi, &sys.spg},
        {dream::pvr::Spg::kStatus, dream::pvr::Spg::kStatus + 4, &sys.spg},
        // The PowerVR's registers: the native graphics' frame settings.
        {dream::pvr::Core::kRegBase, dream::pvr::Core::kRegEnd, native_gfx_regs()},
        {dream::aica::Rtc::kBase, dream::aica::Rtc::kEnd, sys.rtc},
        // The sound chip's registers: the native sound's (the game only starts and stops the
        // driver there).
        {dream::aica::Aica::kRegBase, dream::aica::Aica::kRegEnd, native_sound_regs()},
        {NativeDmac::kBase, NativeDmac::kEnd, &g_dmac},
        {dream::sh4::Tmu::kBase, dream::sh4::Tmu::kEnd, &sys.tmu},
        {dream::sh4::Intc::kBase, dream::sh4::Intc::kBase + 0x10, &sys.intc},
        {0xFFE80000u, 0xFFE80028u, &g_scif},
    };
    // Maple: the native peripherals send no Maple frame.
    g_blocks.push_back({MapleRegs::kBase, MapleRegs::kEnd, &g_maple_regs});
    static NativeShim shim(sys.memory);
    sys.guest_memory = &shim;
    // RAM and store-queue stores stay on the real memory's fast path; only device addresses come
    // to the shim.
    sys.memory.add_follower(&shim);
    // The engine's models of these blocks are unmapped. The SH-4's on-chip blocks on P4 stay
    // mapped; the shim answers the game's accesses to them all the same.
    for (const Block& b : g_blocks)
        if (b.lo < 0xE0000000u)
            sys.memory.unmap_mmio(b.lo, b.hi);
    // The TA's polygon FIFO, YUV converter and texture path: Kamui's buffer send is native.
    sys.memory.unmap_mmio(dream::pvr::Core::kFifoBase, dream::pvr::Core::kFifoEnd);
}

std::uint32_t native_sb_reg(std::uint32_t offset) {
    return g_sb.reg(offset);
}

bool native_dev_on() {
    return !g_blocks.empty();
}

}  // namespace rez
