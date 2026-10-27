// Device shims: guest memory as it is, except the device registers native code answers. Guest code
// runs on a ShimMemory in place of the memory map (System::guest_memory, or a hook running one
// translated function on it): accesses the shim claims are answered by native code, everything else
// goes to the real memory. The code's timing, polls and stores stay the original's, and the device
// model never sees the access.
#pragma once

#include <cstdint>

#include "dream/runtime/memory.h"

namespace rez {

// RAM loads keep the inline fast path, and so do RAM and store-queue stores while the real memory's
// is on: the shim follows it (DcMemory::add_follower), so `fast_stores` and `fast_sq` change with
// it. While the write hash, a watch or a replay journal is armed, every store comes through here
// and on to the real memory, which records it.
class ShimMemory : public dream::Memory {
public:
    explicit ShimMemory(dream::Memory& real) : real_(real) {
        fast_ram = real.fast_ram;
        fast_stores = false;  // until the real memory makes it a follower
        fast_sq = nullptr;
    }

    std::uint8_t read8_slow(std::uint32_t a) override {
        std::uint32_t v;
        return shim_read(phys(a), 1, v) ? static_cast<std::uint8_t>(v) : real_.read8(a);
    }
    std::uint16_t read16_slow(std::uint32_t a) override {
        std::uint32_t v;
        return shim_read(phys(a), 2, v) ? static_cast<std::uint16_t>(v) : real_.read16(a);
    }
    std::uint32_t read32_slow(std::uint32_t a) override {
        std::uint32_t v;
        return shim_read(phys(a), 4, v) ? v : real_.read32(a);
    }
    std::uint64_t read64_slow(std::uint32_t a) override { return real_.read64(a); }
    void write8_slow(std::uint32_t a, std::uint8_t v) override {
        if (plain(a) || !shim_write(phys(a), 1, v))
            real_.write8(a, v);
    }
    void write16_slow(std::uint32_t a, std::uint16_t v) override {
        if (plain(a) || !shim_write(phys(a), 2, v))
            real_.write16(a, v);
    }
    void write32_slow(std::uint32_t a, std::uint32_t v) override {
        if (plain(a) || !shim_write(phys(a), 4, v))
            real_.write32(a, v);
    }
    void write64_slow(std::uint32_t a, std::uint64_t v) override { real_.write64(a, v); }
    void sq_write32(std::uint32_t a, std::uint32_t v) override { real_.sq_write32(a, v); }
    void sq_flush(std::uint32_t a) override { real_.sq_flush(a); }

protected:
    // Every store comes here: RAM and the store queues go straight on.
    static constexpr bool plain(std::uint32_t a) noexcept { return is_ram(a) || is_sq(a); }

    // Areas 0-7 by physical address; P4 (store queues, on-chip modules) as it is, so that a store
    // queue address never aliases a register below it.
    static constexpr std::uint32_t phys(std::uint32_t a) noexcept {
        return a >= 0xE0000000u ? a : (a & 0x1FFFFFFFu);
    }

    // An access the shim answers (`p` as phys() gives it): true when handled. Every
    // claimed access is a device access in the original, so implementations call device_access()
    // first to keep the scheduler where the original's register access would have brought it.
    virtual bool shim_read(std::uint32_t p, unsigned size, std::uint32_t& v) = 0;
    virtual bool shim_write(std::uint32_t p, unsigned size, std::uint32_t v) = 0;

    dream::Memory& real_;
};

}  // namespace rez
