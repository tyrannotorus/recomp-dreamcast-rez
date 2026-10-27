#include "dream/runtime/mem/dc_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace dream::mem {

// ---- DcMemory ----------------------------------------------------------------------------------

DcMemory::DcMemory()
    : ram_(new std::uint8_t[kRamSize + kRamPad]()),
      vram_(new std::uint8_t[kVramSize]()),
      aram_(new std::uint8_t[kAramSize]()),
      bios_(new std::uint8_t[kBiosSize]()),
      flash_(new std::uint8_t[kFlashSize]()),
      ocram_(new std::uint8_t[kOcramSize]()) {
    refresh_fast_path();
}

void DcMemory::map_mmio(std::uint32_t lo, std::uint32_t hi, MmioHandler* h) {
    phys_mmio_.push_back({lo, hi, h});
}

unsigned DcMemory::unmap_mmio(std::uint32_t lo, std::uint32_t hi) {
    const auto before = phys_mmio_.size();
    // remove_if, not C++20's erase_if: GCC 8's library (NextUI's tg5040 sysroot) has none.
    phys_mmio_.erase(std::remove_if(phys_mmio_.begin(), phys_mmio_.end(),
                                    [&](const Range& r) { return r.lo >= lo && r.hi <= hi; }),
                     phys_mmio_.end());
    return static_cast<unsigned>(before - phys_mmio_.size());
}

void DcMemory::map_p4(std::uint32_t lo, std::uint32_t hi, MmioHandler* h) {
    p4_mmio_.push_back({lo, hi, h});
}

MmioHandler* DcMemory::find(const std::vector<Range>& t, std::uint32_t a) const noexcept {
    for (const auto& r : t)
        if (a >= r.lo && a < r.hi)
            return r.h;
    return nullptr;
}

// Same interleave as the hardware's 64-bit bus and Flycast's pvr_map32 (GPL-2.0): bank 0
// words go to even 64-bit slots, bank 1 words to odd ones.
std::uint32_t DcMemory::vram_map32(std::uint32_t offset32) noexcept {
    constexpr std::uint32_t kBankBit = 0x400000u;
    constexpr std::uint32_t kStaticBits = (kVramSize - 1) - (kBankBit * 2 - 1) + 3;
    constexpr std::uint32_t kOffsetBits = (kBankBit - 1) & ~3u;
    const std::uint32_t bank = (offset32 & kBankBit) / kBankBit;
    std::uint32_t rv = offset32 & kStaticBits;
    rv |= (offset32 & kOffsetBits) * 2;
    rv |= bank * 4;
    return rv;
}

DcMemory::Target DcMemory::resolve(std::uint32_t a) {
    Target t;
    // P4: store queues, then the on-chip module registers.
    if ((a & 0xE0000000u) == 0xE0000000u) {
        if (a < 0xE4000000u) {
            t.kind = Target::kBytes;
            t.bytes = reinterpret_cast<std::uint8_t*>(&sq_[(a >> 5) & 1][0]) + (a & 0x1Cu);
            return t;
        }
        if (MmioHandler* h = find(p4_mmio_, a)) {
            t.kind = Target::kMmio;
            if (on_device_access)
                on_device_access();
            t.mmio = h;
            t.mmio_addr = a;
            return t;
        }
        return t;
    }
    // Operand cache as RAM (CCR.ORA): 8 KB in two 4 KB halves selected by bit 13, mirrored over
    // 0x7C000000-0x7FFFFFFF (SH7750 hardware manual, cache chapter).
    if ((a & 0xFC000000u) == 0x7C000000u) {
        t.kind = Target::kBytes;
        t.bytes = ocram_.get() + (((a >> 13) & 1u) << 12) + (a & 0xFFFu);
        return t;
    }
    const std::uint32_t phys = a & 0x1FFFFFFFu;
    switch (phys >> 26) {
        case 0: {  // area 0: ROM, flash, registers, sound RAM
            const std::uint32_t off = phys & 0x03FFFFFFu;
            if (off < 0x00200000u) {
                t.kind = Target::kBytes;
                t.bytes = bios_.get() + off;
                return t;
            }
            if (off < 0x00400000u) {
                t.kind = Target::kBytes;
                t.bytes = flash_.get() + (off & (kFlashSize - 1));
                return t;
            }
            if (off >= 0x00800000u && off < 0x01000000u) {
                t.kind = Target::kBytes;
                t.bytes = aram_.get() + (off & (kAramSize - 1));
                if (on_aram_access)
                    on_aram_access();
                return t;
            }
            if ((off >= 0x00600000u && off < 0x00600800u) || off >= 0x01000000u) {
                // Optional hardware that is absent: the modem (0x00600000) and the G2 expansion
                // area (0x01000000 up, e.g. the broadband adapter), which games probe at boot.
                // Reads give 0 and writes are dropped.
                return t;
            }
            break;  // registers: table below
        }
        case 1: {  // area 1: VRAM, 64-bit view at +0x0000000, 32-bit view at +0x1000000, mirrors
            const std::uint32_t off = phys & 0x007FFFFFu;
            t.kind = Target::kBytes;
            t.bytes = vram_.get() + ((phys & 0x01000000u) ? vram_map32(off) : off);
            return t;
        }
        case 3: {  // area 3: main RAM and its three mirrors
            t.kind = Target::kBytes;
            t.bytes = ram_.get() + (phys & (kRamSize - 1));
            return t;
        }
        default:
            break;
    }
    if (MmioHandler* h = find(phys_mmio_, phys)) {
        t.kind = Target::kMmio;
        if (on_device_access)
            on_device_access();
        t.mmio = h;
        t.mmio_addr = phys;
        return t;
    }
    return t;
}

template <typename T>
T DcMemory::load(std::uint32_t a) {
    const Target t = resolve(a);
    if (t.kind == Target::kBytes) {
        T v;
        std::memcpy(&v, t.bytes, sizeof(T));
        return v;
    }
    if (t.kind == Target::kMmio)
        return static_cast<T>(t.mmio->read(t.mmio_addr, sizeof(T)));
    return T{};
}

bool DcMemory::copy32(std::uint32_t dst, std::uint32_t src, std::uint32_t len) {
    if (len < 8 || (len & 3u) || tracing_writes())
        return false;
    // Both ends of both ranges: one host span each, or the word loop (a range across regions or
    // around sound RAM's wrap). Resolving them makes the accesses' own calls (on_aram_access).
    const Target s0 = resolve(src), s1 = resolve(src + len - 4);
    const Target d0 = resolve(dst), d1 = resolve(dst + len - 4);
    if (s0.kind != Target::kBytes || s1.kind != Target::kBytes || d0.kind != Target::kBytes ||
        d1.kind != Target::kBytes || s1.bytes != s0.bytes + len - 4 || d1.bytes != d0.bytes + len - 4)
        return false;
    if (s0.bytes < d0.bytes + len && d0.bytes < s0.bytes + len)
        return false;  // overlapping: the word loop's order matters
    std::memcpy(d0.bytes, s0.bytes, len);
    mark_vram(d0.bytes, len);
    note_aram_write(d0.bytes, len);
    return true;
}

template <typename T>
void DcMemory::store(std::uint32_t a, T v) {
    const Target t = resolve(a);
    if (t.kind == Target::kBytes) {
        std::memcpy(t.bytes, &v, sizeof(T));
        mark_vram(t.bytes, sizeof(T));
        note_ram_write(a, static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(v)),
                       sizeof(T));
        note_aram_write(t.bytes, sizeof(T));
        return;
    }
    if (t.kind == Target::kMmio)
        t.mmio->write(t.mmio_addr, static_cast<std::uint32_t>(v), sizeof(T));
}

std::uint8_t DcMemory::read8_slow(std::uint32_t a) {
    return load<std::uint8_t>(a);
}
std::uint16_t DcMemory::read16_slow(std::uint32_t a) {
    return load<std::uint16_t>(a);
}
std::uint32_t DcMemory::read32_slow(std::uint32_t a) {
    if ((a & 0xFFFFFFC0u) == 0xFF000000u) {  // CCN: MMU and cache control, exception registers
        switch (a & 0x3Cu) {
            case 0x20:
                return tra;
            case 0x24:
                return expevt;
            case 0x28:
                return intevt;
            case 0x38:
                return qacr_[0];
            case 0x3C:
                return qacr_[1];
            default:
                return ccn_[(a >> 2) & 15];
        }
    }
    return load<std::uint32_t>(a);
}
std::uint64_t DcMemory::read64_slow(std::uint32_t a) {
    const Target t = resolve(a);
    if (t.kind == Target::kBytes) {
        std::uint64_t v;
        std::memcpy(&v, t.bytes, 8);
        return v;
    }
    if (t.kind == Target::kMmio)
        return static_cast<std::uint64_t>(t.mmio->read(t.mmio_addr, 4)) |
               (static_cast<std::uint64_t>(t.mmio->read(t.mmio_addr + 4, 4)) << 32);
    return 0;
}
void DcMemory::write8_slow(std::uint32_t a, std::uint8_t v) {
    store<std::uint8_t>(a, v);
}
void DcMemory::write16_slow(std::uint32_t a, std::uint16_t v) {
    store<std::uint16_t>(a, v);
}
void DcMemory::write32_slow(std::uint32_t a, std::uint32_t v) {
    if ((a & 0xFFFFFFC0u) == 0xFF000000u) {
        switch (a & 0x3Cu) {
            case 0x20:
                tra = v & 0x3FCu;
                return;
            case 0x24:
                expevt = v & 0xFFFu;
                return;
            case 0x28:
                intevt = v & 0xFFFu;
                return;
            case 0x38:
                set_qacr(0, v);
                return;
            case 0x3C:
                set_qacr(1, v);
                return;
            default:
                ccn_[(a >> 2) & 15] = v;
                if (((a >> 2) & 15) == 4)
                    refresh_sq();  // MMUCR.AT decides where an unmapped queue flushes
                return;  // MMUCR, CCR, PTEH/PTEL/TTB/TEA/PTEA: stored, no effect
        }
    }
    store<std::uint32_t>(a, v);
}
void DcMemory::write64_slow(std::uint32_t a, std::uint64_t v) {
    const Target t = resolve(a);
    if (t.kind == Target::kBytes) {
        const std::uint32_t lo = static_cast<std::uint32_t>(v);
        const std::uint32_t hi = static_cast<std::uint32_t>(v >> 32);
        std::memcpy(t.bytes, &v, 8);
        // Two 4-byte reports, the same split the device path below uses, so a pair written here
        // hashes identically to the same pair written as two stores.
        note_ram_write(a, lo, 4);
        note_ram_write(a + 4, hi, 4);
        note_aram_write(t.bytes, 8);
        return;
    }
    if (t.kind == Target::kMmio) {
        t.mmio->write(t.mmio_addr, static_cast<std::uint32_t>(v), 4);
        t.mmio->write(t.mmio_addr + 4, static_cast<std::uint32_t>(v >> 32), 4);
    }
}

void DcMemory::sq_write32(std::uint32_t a, std::uint32_t v) {
    sq_[(a >> 5) & 1][(a >> 2) & 7] = v;
}

// PREF @Rn with Rn in the store-queue area: the 32 bytes of SQ0 or SQ1 (address bit 5) land at
// physical (QACRn.AREA << 26) | (Rn & 0x03FFFFE0); SH7750 manual 4.6 and Flycast storeq.cpp.
//
// With the MMU on (MMUCR.AT) the hardware ignores QACR and translates the SQ address through the
// UTLB. There is no TLB model here; Katana's syMmu leaves QACR at 0 and maps the SQ pages onto
// RAM, so MMU on with QACR unset is treated as area 3 (RAM).
void DcMemory::sq_flush(std::uint32_t a) {
    const unsigned n = (a >> 5) & 1;
    std::uint32_t area = qacr_[n];
    if (area == 0 && (ccn_[4] & 1u) != 0)  // ccn_[4] is MMUCR (0xFF000010); bit 0 is AT
        area = 3u << 2;
    const std::uint32_t dest = (area << 24) | (a & 0x03FFFFE0u);
    const std::uint32_t* words = sq_[n];
    // Into main RAM with the write hash off: straight in, as the stores' own fast path does.
    // fast_stores is off while the write hash must see the burst; RAM has no video-memory pages
    // to mark.
    if (fast_stores && is_ram(dest)) {
        std::memcpy(fast_ram + (dest & 0x00FFFFFFu), words, 32);
        return;
    }
    const Target t = resolve(dest);
    if (t.kind == Target::kBytes) {
        std::memcpy(t.bytes, words, 32);
        mark_vram(t.bytes, 32);
        // A burst into RAM is eight stores as far as the write hash is concerned.
        if (tracing_writes())
            for (unsigned i = 0; i < 8; ++i) note_ram_write(dest + 4 * i, words[i], 4);
        return;
    }
    if (t.kind == Target::kMmio)  // how a title hands the graphics hardware a display list
        t.mmio->write_burst(t.mmio_addr, words);
}

}  // namespace dream::mem
