// The Dreamcast address space: main RAM with its mirrors, VRAM in both views, sound RAM, flash,
// boot ROM, on-chip RAM, the store queues with PREF flush through QACR, and an MMIO dispatch table
// the hardware cores register into.
// Unhandled accesses read 0 and drop writes, so a title keeps running.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "dream/runtime/memory.h"

namespace dream::mem {

// A device on the bus. Sizes are 1, 2 or 4; 64-bit accesses arrive as two 32-bit ones.
class MmioHandler {
public:
    virtual ~MmioHandler() = default;
    virtual std::uint32_t read(std::uint32_t addr, unsigned size) = 0;
    virtual void write(std::uint32_t addr, std::uint32_t value, unsigned size) = 0;
    // A store-queue flush: 32 bytes as eight words. Default splits into word writes.
    virtual void write_burst(std::uint32_t addr, const std::uint32_t* words) {
        for (unsigned i = 0; i < 8; ++i) write(addr + 4 * i, words[i], 4);
    }
};

// A block of registers with no behaviour yet: reads return what was written. Used for on-chip
// modules a booting title programs but the runtime does not model (DMAC, BSC, UBC, CPG/WDT).
class RegisterFile final : public MmioHandler {
public:
    RegisterFile(std::uint32_t base, std::uint32_t words) : base_(base), regs_(words, 0) {}
    std::uint32_t read(std::uint32_t addr, unsigned) override {
        const std::uint32_t i = (addr - base_) >> 2;
        return i < regs_.size() ? regs_[i] : 0;
    }
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override {
        const std::uint32_t i = (addr - base_) >> 2;
        if (i >= regs_.size())
            return;
        if (size == 4) {
            regs_[i] = value;
        } else {  // merge a byte or halfword into its word (little-endian)
            const unsigned shift = (addr & 3u) * 8;
            const std::uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
            regs_[i] = (regs_[i] & ~mask) | ((value << shift) & mask);
        }
    }
    std::uint32_t& word(std::uint32_t offset) { return regs_[offset >> 2]; }

private:
    std::uint32_t base_;
    std::vector<std::uint32_t> regs_;
};

// A region whose writes are dropped and whose reads return zero: the instruction and operand
// cache address and data arrays at 0xF0000000..0xF7FFFFFF, which cache-maintenance loops write
// and nothing needs to model.
class NullRegion final : public MmioHandler {
public:
    std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
    void write(std::uint32_t, std::uint32_t, unsigned) override {}
};

class DcMemory final : public ::dream::Memory {
public:
    static constexpr std::uint32_t kRamSize = 16u << 20;
    // Bytes allocated past RAM's end: an unaligned access at the last byte reads and writes them,
    // by the slow path and emitted code's fast path alike, so neither tests for the end.
    static constexpr std::uint32_t kRamPad = 8;
    static constexpr std::uint32_t kVramSize = 8u << 20;
    static constexpr std::uint32_t kAramSize = 2u << 20;
    static constexpr std::uint32_t kBiosSize = 2u << 20;
    static constexpr std::uint32_t kFlashSize = 128u << 10;
    static constexpr std::uint32_t kOcramSize = 8u << 10;

    DcMemory();

    std::uint8_t* ram() noexcept { return ram_.get(); }
    std::uint8_t* vram() noexcept { return vram_.get(); }
    // Video memory written by the guest since the last take: 4 KB pages, offsets in
    // the 64-bit view (the texture cache's addresses). Every guest path into VRAM -- CPU stores,
    // store-queue bursts, the TA texture path, the YUV converter, DMA -- goes through store() or
    // sq_flush(), which mark it. `f(begin, end)` is called once per run of dirty pages.
    template <typename F>
    void take_vram_dirty(F&& f) {
        if (!vram_any_dirty_)
            return;
        vram_any_dirty_ = false;
        constexpr std::uint32_t kPages = kVramSize >> kVramPageBits;
        std::uint32_t run = kPages;
        for (std::uint32_t p = 0; p <= kPages; ++p) {
            const bool dirty =
                p < kPages && ((vram_dirty_[p >> 6] >> (p & 63)) & 1u) != 0;
            if (dirty && run == kPages)
                run = p;
            if (!dirty && run != kPages) {
                f(run << kVramPageBits, p << kVramPageBits);
                run = kPages;
            }
        }
        vram_dirty_.fill(0);
    }
    std::uint8_t* aram() noexcept { return aram_.get(); }
    std::uint8_t* bios() noexcept { return bios_.get(); }
    std::uint8_t* flash() noexcept { return flash_.get(); }
    // Called before every device (MMIO) access: the runtime advances the virtual clock to the
    // guest's cycle count here, so a timer or drive status read sees the present rather than the
    // last interrupt poll (Katana's syTmrGetCount busy-waits on TMU0 between polls).
    std::function<void()> on_device_access;
    // Every SH-4 (or DMA, through this object) access to sound RAM, before it is made, so lazy
    // sound samples can be brought up to the present first (aica::Aica::catch_up). Unset costs a
    // test.
    std::function<void()> on_aram_access;
    // Every store into sound RAM through this object, after it lands, with its offset in sound
    // RAM and size. Unset costs a test.
    std::function<void(std::uint32_t offset, unsigned size)> on_aram_write;
    // A DMA's copy, `len` bytes from `src` to `dst`: what
    // `for (i < len; i += 4) write32(dst + i, read32(src + i))` leaves, as one copy when both
    // ranges are plain memory, contiguous, apart, and the write hash is off; then the VRAM pages
    // are marked and on_aram_write is told once, for the whole range.
    // False otherwise, having done nothing: the caller copies word by word.
    bool copy32(std::uint32_t dst, std::uint32_t src, std::uint32_t len);
    // A rolling hash of every guest store (--write-hash). Two runs that agree on it did the same
    // thing, so comparing it frame by frame checks that a change leaves the game's behaviour as it
    // was. Off by default, and one predictable branch when on.
    bool hash_writes = false;
    std::uint64_t write_hash = 0, writes_hashed = 0;
    void note_write(std::uint32_t addr, std::uint64_t value, unsigned size) noexcept {
        constexpr std::uint64_t kPrime = 0x100000001B3ull;
        write_hash = (write_hash ^ (addr & 0x1FFFFFFFu)) * kPrime;
        write_hash = (write_hash ^ value) * kPrime;
        write_hash = (write_hash ^ size) * kPrime;
        ++writes_hashed;
    }
    // True when the write hash is armed, so the cold paths can skip the per-word loop entirely.
    bool tracing_writes() const noexcept { return hash_writes; }
    // Every path that writes guest RAM reports through here, not just DcMemory::store: anything
    // that memcpys into `t.bytes` must call this.
    void note_ram_write(std::uint32_t addr, std::uint64_t value, unsigned size) noexcept {
        if (hash_writes)
            note_write(addr, value, size);
    }

    // Devices. Physical ranges are [lo, hi) within the 512 MB physical space (area 0 registers,
    // area 4 TA FIFO); P4 ranges are virtual addresses at 0xE4000000 and above (on-chip modules).
    void map_mmio(std::uint32_t phys_lo, std::uint32_t phys_hi, MmioHandler* h);
    // Removes every physical mapping that lies inside [phys_lo, phys_hi) and returns how many:
    // native code that takes over a device unmaps its registers.
    unsigned unmap_mmio(std::uint32_t phys_lo, std::uint32_t phys_hi);
    void map_p4(std::uint32_t lo, std::uint32_t hi, MmioHandler* h);

    // CCN exception registers the interrupt model writes and handlers read back through memory:
    // TRA 0xFF000020, EXPEVT 0xFF000024, INTEVT 0xFF000028.
    std::uint32_t tra = 0, expevt = 0, intevt = 0;

    // Store-queue area control registers (CCN QACR0/1 at 0xFF000038/0xFF00003C); bits 4:2 select
    // the 64 MB area the flushed data lands in.
    void set_qacr(unsigned n, std::uint32_t v) noexcept {
        qacr_[n & 1] = v & 0x1Cu;
        refresh_sq();
    }
    std::uint32_t qacr(unsigned n) const noexcept { return qacr_[n & 1]; }

    // Memory interface (the inline RAM fast path is in the base class)
    std::uint8_t read8_slow(std::uint32_t a) override;
    std::uint16_t read16_slow(std::uint32_t a) override;
    std::uint32_t read32_slow(std::uint32_t a) override;
    std::uint64_t read64_slow(std::uint32_t a) override;
    void write8_slow(std::uint32_t a, std::uint8_t v) override;
    void write16_slow(std::uint32_t a, std::uint16_t v) override;
    void write32_slow(std::uint32_t a, std::uint32_t v) override;
    void write64_slow(std::uint32_t a, std::uint64_t v) override;
    // Re-derives `fast_stores`: on unless the write hash must see every store. Call after arming
    // or disarming it.
    void refresh_fast_path() noexcept {
        fast_ram = ram_.get();
        fast_stores = fast_ram && !hash_writes;
        fast_sq = fast_stores ? &sq_[0][0] : nullptr;
        for (Memory* m : followers_) {
            m->fast_stores = fast_stores;
            m->fast_sq = fast_sq;
        }
        refresh_sq();
    }
    // Where each store queue's flush goes, for Memory::sq_flush_fast: the RAM path of sq_flush,
    // taken when the destination area is main RAM (area 3, from QACR or by the MMU assumption)
    // and the write hash is off. Called whenever QACR0/1, MMUCR or the fast path change.
    void refresh_sq() noexcept {
        for (unsigned n = 0; n < 2; ++n) {
            std::uint32_t area = qacr_[n];
            const bool assumed = area == 0 && (ccn_[4] & 1u) != 0;  // MMUCR.AT, as in sq_flush
            if (assumed)
                area = 3u << 2;
            sq_to_ram[n] = fast_stores && area == (3u << 2) ? (assumed ? 2 : 1) : 0;
            sq_area[n] = area << 24;
        }
        sq_ram = fast_ram;
        for (Memory* m : followers_) {
            m->sq_ram = sq_ram;
            for (unsigned n = 0; n < 2; ++n) {
                m->sq_to_ram[n] = sq_to_ram[n];
                m->sq_area[n] = sq_area[n];
            }
        }
    }
    // A memory layered over this one (a device shim that passes RAM and store-queue stores
    // through) whose store fast path must follow this one's: its fast-path fields are set now and
    // at every refresh, so they are never stale.
    void add_follower(Memory* m) {
        followers_.push_back(m);
        m->fast_stores = fast_stores;
        m->fast_sq = fast_sq;
        refresh_sq();
    }
    void sq_write32(std::uint32_t a, std::uint32_t v) override;
    void sq_flush(std::uint32_t a) override;

    // The 32-bit VRAM view interleaves the two 4 MB banks word by word onto the 64-bit view.
    static std::uint32_t vram_map32(std::uint32_t offset32) noexcept;

private:
    static constexpr std::uint32_t kVramPageBits = 12;
    std::array<std::uint64_t, (8u << 20 >> 12) / 64> vram_dirty_{};
    bool vram_any_dirty_ = false;
    std::vector<Memory*> followers_;  // add_follower()
    void note_aram_write(const std::uint8_t* p, unsigned bytes) {
        if (!on_aram_write)
            return;
        const std::uint8_t* base = aram_.get();
        if (p >= base && p < base + kAramSize)
            on_aram_write(static_cast<std::uint32_t>(p - base), bytes);
    }
    void mark_vram(const std::uint8_t* p, std::uint32_t bytes) noexcept {
        const std::uint8_t* base = vram_.get();
        if (p < base || p >= base + kVramSize)
            return;
        const auto first = static_cast<std::uint32_t>(p - base) >> kVramPageBits;
        const auto last = static_cast<std::uint32_t>(p - base + bytes - 1) >> kVramPageBits;
        for (std::uint32_t q = first; q <= last && q < (kVramSize >> kVramPageBits); ++q)
            vram_dirty_[q >> 6] |= 1ull << (q & 63);
        vram_any_dirty_ = true;
    }
    struct Target {
        enum Kind { kNone, kBytes, kMmio } kind = kNone;
        std::uint8_t* bytes = nullptr;  // kBytes: host pointer to the first byte
        MmioHandler* mmio = nullptr;    // kMmio
        std::uint32_t mmio_addr = 0;    // address handed to the handler
    };
    Target resolve(std::uint32_t a);
    template <typename T>
    T load(std::uint32_t a);
    template <typename T>
    void store(std::uint32_t a, T v);

    struct Range {
        std::uint32_t lo, hi;
        MmioHandler* h;
    };
    MmioHandler* find(const std::vector<Range>& t, std::uint32_t a) const noexcept;

    std::unique_ptr<std::uint8_t[]> ram_, vram_, aram_, bios_, flash_, ocram_;
    std::vector<Range> phys_mmio_, p4_mmio_;
    std::uint32_t sq_[2][8]{};
    std::uint32_t qacr_[2]{};
    std::uint32_t ccn_[16]{};  // PTEH..QACR1 word registers at 0xFF000000 + 4n (CCR, TRA, EXPEVT,
                               // INTEVT, QACR aliased)
};

}  // namespace dream::mem
