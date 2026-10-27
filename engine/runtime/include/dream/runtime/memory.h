// Guest memory interface used by emitted code.
//
// Emitted functions receive a Memory& and call these accessors with guest virtual addresses.
// The full Dreamcast map (RAM mirrors, VRAM views, MMIO dispatch, store queues) is mem::DcMemory;
// the BareMemory implementation below is a RAM-only substitute that traps on anything else.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "dream/runtime/rare_call.h"

namespace dream {

class Memory {
public:
    virtual ~Memory() = default;

    // RAM fast path. Emitted code calls these inline accessors; main RAM (area 3,
    // every P0-P3 alias and the three mirrors) is a mask and an index, and everything else --
    // MMIO, VRAM views, store queues, P4, faults -- goes to the virtual *_slow path. `fast_ram` is
    // null for implementations that want every access (BareMemory). `fast_stores` is cleared by
    // the implementation while the write hash must see every store.
    // fast_ram must have 8 bytes past its 16 MB that accesses may use (DcMemory::kRamPad): emitted
    // code's fast path (ops.h, MemView) does not test for the end.
    std::uint8_t* fast_ram = nullptr;
    bool fast_stores = false;
    // Store-queue fast path: the 2 x 8 words of the SQ buffers, word (a >> 2) & 15 for
    // a store to 0xE0000000-0xE3FFFFFF. Set and cleared with `fast_stores`. Games build their TA
    // display lists through the store queues, millions of stores a second, which would each
    // otherwise take the virtual slow path and the general address decoder to copy 4 bytes.
    std::uint32_t* fast_sq = nullptr;
    static constexpr bool is_sq(std::uint32_t a) noexcept { return (a & 0xFC000000u) == 0xE0000000u; }
    static constexpr bool is_ram(std::uint32_t a) noexcept {
        // Area 3 through P0-P3 only: P4 (0xE0000000 up) is never RAM, whatever its bits say.
        return (a & 0x1C000000u) == 0x0C000000u && a < 0xE0000000u;
    }

    std::uint8_t read8(std::uint32_t a) {
        if (fast_ram && is_ram(a))
            return fast_ram[a & 0x00FFFFFFu];
        return read8_slow(a);
    }
    std::uint16_t read16(std::uint32_t a) { return fast_load<std::uint16_t>(a, &Memory::read16_slow); }
    std::uint32_t read32(std::uint32_t a) { return fast_load<std::uint32_t>(a, &Memory::read32_slow); }
    std::uint64_t read64(std::uint32_t a) { return fast_load<std::uint64_t>(a, &Memory::read64_slow); }
    void write8(std::uint32_t a, std::uint8_t v) {
        if (fast_stores && is_ram(a))
            fast_ram[a & 0x00FFFFFFu] = v;
        else
            write8_slow(a, v);
    }
    void write16(std::uint32_t a, std::uint16_t v) { fast_store(a, v, &Memory::write16_slow); }
    void write32(std::uint32_t a, std::uint32_t v) { fast_store(a, v, &Memory::write32_slow); }
    void write64(std::uint32_t a, std::uint64_t v) { fast_store(a, v, &Memory::write64_slow); }

    virtual std::uint8_t read8_slow(std::uint32_t addr) = 0;
    virtual std::uint16_t read16_slow(std::uint32_t addr) = 0;
    virtual std::uint32_t read32_slow(std::uint32_t addr) = 0;
    virtual std::uint64_t read64_slow(std::uint32_t addr) = 0;
    virtual void write8_slow(std::uint32_t addr, std::uint8_t v) = 0;
    virtual void write16_slow(std::uint32_t addr, std::uint16_t v) = 0;
    virtual void write32_slow(std::uint32_t addr, std::uint32_t v) = 0;
    virtual void write64_slow(std::uint32_t addr, std::uint64_t v) = 0;
    // Store queue: writes to 0xE0000000..0xE3FFFFFF buffer in the SQ; PREF flushes 32 bytes.
    virtual void sq_write32(std::uint32_t addr, std::uint32_t v) = 0;
    virtual void sq_flush(std::uint32_t addr) = 0;
    // What emitted code calls for a PREF into the store queues: straight into RAM when the
    // implementation says that is where queue n's flush goes, sq_flush otherwise. The
    // implementation keeps the state current with its store fast path (DcMemory: QACR0/1, MMUCR,
    // fast_stores): sq_to_ram[n] is 0 for sq_flush, 1 for RAM, 2 for RAM by the MMU assumption;
    // sq_area[n] holds the destination's area bits and sq_ram main RAM
    // (its own pointer: a memory layered over the real one may turn off its fast_ram for reads).
    std::uint8_t sq_to_ram[2] = {0, 0};
    std::uint32_t sq_area[2] = {0, 0};
    std::uint8_t* sq_ram = nullptr;
    void sq_flush_fast(std::uint32_t a) {
        const unsigned n = (a >> 5) & 1u;
        if (sq_to_ram[n]) {
            const std::uint32_t dest = sq_area[n] | (a & 0x03FFFFE0u);
            std::memcpy(sq_ram + (dest & 0x00FFFFFFu), fast_sq + std::size_t{8} * n, 32);
            return;
        }
        sq_flush_rare(a);
    }
    // The other way, out of line (rare_call.h): emitted code inlines sq_flush_fast.
    [[gnu::cold, gnu::noinline]] DREAM_RARE_CALL void sq_flush_rare(std::uint32_t a) { sq_flush(a); }

private:
    // An access that would run past the end of RAM (an unaligned word at the last byte) takes the
    // slow path, which knows what the hardware does there.
    template <typename T>
    T fast_load(std::uint32_t a, T (Memory::*slow)(std::uint32_t)) {
        const std::uint32_t o = a & 0x00FFFFFFu;
        if (fast_ram && is_ram(a) && o <= 0x01000000u - sizeof(T)) {
            T v;
            std::memcpy(&v, fast_ram + o, sizeof(T));
            return v;
        }
        return (this->*slow)(a);
    }
    template <typename T>
    void fast_store(std::uint32_t a, T v, void (Memory::*slow)(std::uint32_t, T)) {
        const std::uint32_t o = a & 0x00FFFFFFu;
        if (fast_stores && is_ram(a) && o <= 0x01000000u - sizeof(T)) {
            std::memcpy(fast_ram + o, &v, sizeof(T));
            return;
        }
        if constexpr (sizeof(T) == 4 || sizeof(T) == 8) {
            // A word into its slot, a double word (8-byte aligned) into two, as the slow path does.
            if (fast_sq && is_sq(a) && (sizeof(T) == 4 || (a & 7u) == 0)) {
                std::memcpy(fast_sq + ((a >> 2) & 15u), &v, sizeof(T));
                return;
            }
        }
        (this->*slow)(a, v);
    }
};

// 16 MB of main RAM at every alias of area 3 (0x0C000000 physical), nothing else. Accesses outside
// RAM throw.
class BareMemory final : public Memory {
public:
    static constexpr std::uint32_t kRamSize = 16u * 1024 * 1024;

    BareMemory() : ram_(new std::uint8_t[kRamSize]()) {}

    std::uint8_t* ram() noexcept { return ram_.get(); }

    std::uint8_t read8_slow(std::uint32_t a) override { return ram_[idx(a)]; }
    std::uint16_t read16_slow(std::uint32_t a) override {
        std::uint16_t v;
        std::memcpy(&v, ram_.get() + idx(a), 2);
        return v;
    }
    std::uint32_t read32_slow(std::uint32_t a) override {
        std::uint32_t v;
        std::memcpy(&v, ram_.get() + idx(a), 4);
        return v;
    }
    std::uint64_t read64_slow(std::uint32_t a) override {
        std::uint64_t v;
        std::memcpy(&v, ram_.get() + idx(a), 8);
        return v;
    }
    void write8_slow(std::uint32_t a, std::uint8_t v) override { ram_[idx(a)] = v; }
    void write16_slow(std::uint32_t a, std::uint16_t v) override {
        std::memcpy(ram_.get() + idx(a), &v, 2);
    }
    void write32_slow(std::uint32_t a, std::uint32_t v) override {
        std::memcpy(ram_.get() + idx(a), &v, 4);
    }
    void write64_slow(std::uint32_t a, std::uint64_t v) override {
        std::memcpy(ram_.get() + idx(a), &v, 8);
    }
    void sq_write32(std::uint32_t a, std::uint32_t v) override { sq_[(a >> 2) & 15] = v; }
    void sq_flush(std::uint32_t) override {}  // no QACR mapping

private:
    std::uint32_t idx(std::uint32_t a) const {
        // Any P0/P1/P2/P3 alias of area 3 lands in RAM; RAM itself is mirrored across the 64 MB
        // area.
        if (((a & 0x1C000000u) != 0x0C000000u)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "BareMemory: access outside RAM at 0x%08x", a);
            throw std::out_of_range(buf);
        }
        return a & (kRamSize - 1);
    }

    std::unique_ptr<std::uint8_t[]> ram_;
    std::uint32_t sq_[16]{};
};

}  // namespace dream
