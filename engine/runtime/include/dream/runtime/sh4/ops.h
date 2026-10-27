// Helpers for SH-4 instructions whose C++ lowering is more than a line. Emitted code and the dev
// interpreter call these.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "dream/runtime/fenv.h"
#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::sh4 {

inline std::uint32_t addc(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t tmp = a + b;
    const std::uint32_t res = tmp + c.t;
    c.t = (tmp < a) || (res < tmp);
    return res;
}

inline std::uint32_t subc(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t tmp = a - b;
    const std::uint32_t res = tmp - c.t;
    c.t = (a < tmp) || (tmp < res);
    return res;
}

// ADDV/SUBV: T = signed overflow.
inline std::uint32_t addv(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t res = a + b;
    c.t = (~(a ^ b) & (a ^ res)) >> 31;
    return res;
}

inline std::uint32_t subv(Ctx& c, std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t res = a - b;
    c.t = ((a ^ b) & (a ^ res)) >> 31;
    return res;
}

inline std::uint32_t negc(Ctx& c, std::uint32_t a) noexcept {
    const std::uint32_t tmp = 0u - a;
    const std::uint32_t res = tmp - c.t;
    c.t = (0u < tmp) || (tmp < res);
    return res;
}

// SHAD: positive count shifts left, negative shifts right arithmetically; count is the low 5 bits.
inline std::uint32_t shad(std::uint32_t v, std::uint32_t count) noexcept {
    const std::uint32_t s = count & 0x1F;
    if ((count & 0x80000000u) == 0)
        return v << s;
    if (s == 0)
        return (v & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> (32 - s));
}

inline std::uint32_t shld(std::uint32_t v, std::uint32_t count) noexcept {
    const std::uint32_t s = count & 0x1F;
    if ((count & 0x80000000u) == 0)
        return v << s;
    if (s == 0)
        return 0;
    return v >> (32 - s);
}

inline std::uint32_t rotcl(Ctx& c, std::uint32_t v) noexcept {
    const std::uint32_t t = v >> 31;
    const std::uint32_t res = (v << 1) | c.t;
    c.t = t;
    return res;
}

inline std::uint32_t rotcr(Ctx& c, std::uint32_t v) noexcept {
    const std::uint32_t t = v & 1;
    const std::uint32_t res = (v >> 1) | (c.t << 31);
    c.t = t;
    return res;
}

// DIV0S/DIV0U/DIV1 operate on the Q and M bits of SR and on T.
inline void div0u(Ctx& c) noexcept {
    c.sr &= ~(SR_Q | SR_M);
    c.t = 0;
}

inline void div0s(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    const std::uint32_t q = rn >> 31, m = rm >> 31;
    c.sr = (c.sr & ~(SR_Q | SR_M)) | (q ? SR_Q : 0) | (m ? SR_M : 0);
    c.t = q ^ m;
}

// One step of the non-restoring division; returns the new Rn. Transcribed from the SH-4 manual.
inline std::uint32_t div1(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    std::uint32_t q = (c.sr & SR_Q) ? 1u : 0u;
    const std::uint32_t m = (c.sr & SR_M) ? 1u : 0u;
    const std::uint32_t old_q = q;
    q = rn >> 31;
    rn = (rn << 1) | c.t;
    const std::uint32_t tmp0 = rn;
    if (old_q == 0) {
        if (m == 0) {
            rn -= rm;
            const std::uint32_t tmp1 = rn > tmp0;
            q = q ? (tmp1 == 0) : tmp1;
        } else {
            rn += rm;
            const std::uint32_t tmp1 = rn < tmp0;
            q = q ? tmp1 : (tmp1 == 0);
        }
    } else {
        if (m == 0) {
            rn += rm;
            const std::uint32_t tmp1 = rn < tmp0;
            q = q ? (tmp1 == 0) : tmp1;
        } else {
            rn -= rm;
            const std::uint32_t tmp1 = rn > tmp0;
            q = q ? tmp1 : (tmp1 == 0);
        }
    }
    c.sr = (c.sr & ~SR_Q) | (q ? SR_Q : 0);
    c.t = (q == m);
    return rn;
}

// `count` pairs of `ROTCL Ra; DIV1 Rm,Rn`, the compilers' unrolled division: rotcl and div1 above,
// step by step, with T, Q and M in locals and SR and T written once at the end.
inline void rotcl_div1_run(Ctx& c, std::uint32_t& ra, std::uint32_t& rn, std::uint32_t rm,
                           unsigned count) noexcept {
    std::uint32_t t = c.t, q = (c.sr & SR_Q) ? 1u : 0u;
    const std::uint32_t m = (c.sr & SR_M) ? 1u : 0u;
    for (unsigned i = 0; i < count; ++i) {
        const std::uint32_t top = ra >> 31;  // ROTCL Ra
        ra = (ra << 1) | t;
        t = top;
        const std::uint32_t old_q = q;  // DIV1 Rm,Rn
        q = rn >> 31;
        rn = (rn << 1) | t;
        const std::uint32_t tmp0 = rn;
        if (old_q == m) {
            rn -= rm;
            const std::uint32_t tmp1 = rn > tmp0;
            q = (old_q == 0) ? (q ? (tmp1 == 0) : tmp1) : (q ? tmp1 : (tmp1 == 0));
        } else {
            rn += rm;
            const std::uint32_t tmp1 = rn < tmp0;
            q = (old_q == 0) ? (q ? tmp1 : (tmp1 == 0)) : (q ? (tmp1 == 0) : tmp1);
        }
        t = (q == m);
    }
    c.sr = (c.sr & ~SR_Q) | (q ? SR_Q : 0);
    c.t = t;
}

inline void dmuls(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(rn)) *
                           static_cast<std::int64_t>(static_cast<std::int32_t>(rm));
    c.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
    c.macl = static_cast<std::uint32_t>(p);
}

inline void dmulu(Ctx& c, std::uint32_t rn, std::uint32_t rm) noexcept {
    const std::uint64_t p = static_cast<std::uint64_t>(rn) * rm;
    c.mach = static_cast<std::uint32_t>(p >> 32);
    c.macl = static_cast<std::uint32_t>(p);
}

// MAC.L: MAC += Rm*Rn (signed 32x32->64); with SR.S the result saturates to 48 bits.
inline void mac_l(Ctx& c, std::int32_t a, std::int32_t b) noexcept {
    const std::int64_t prod = static_cast<std::int64_t>(a) * b;
    std::int64_t mac =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(c.mach) << 32) | c.macl);
    if (c.sr & SR_S) {
        mac = static_cast<std::int64_t>(static_cast<std::uint64_t>(mac) << 16) >>
              16;  // 48-bit sign-extend
        std::int64_t sum = mac + prod;
        const std::int64_t lo = -(static_cast<std::int64_t>(1) << 47),
                           hi = (static_cast<std::int64_t>(1) << 47) - 1;
        if (sum > hi)
            sum = hi;
        if (sum < lo)
            sum = lo;
        mac = sum;
    } else {
        mac += prod;
    }
    c.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(mac) >> 32);
    c.macl = static_cast<std::uint32_t>(mac);
}

// MAC.W: MAC += Rm*Rn (signed 16x16->32); with SR.S the low word saturates to 32 bits and MACH
// records overflow; without it the product is added to the 64-bit accumulator.
inline void mac_w(Ctx& c, std::int16_t a, std::int16_t b) noexcept {
    const std::int32_t prod = static_cast<std::int32_t>(a) * b;
    if (c.sr & SR_S) {
        const std::int64_t sum =
            static_cast<std::int64_t>(static_cast<std::int32_t>(c.macl)) + prod;
        if (sum > 0x7FFFFFFFLL) {
            c.macl = 0x7FFFFFFFu;
            c.mach |= 1;
        } else if (sum < -0x80000000LL) {
            c.macl = 0x80000000u;
            c.mach |= 1;
        } else {
            c.macl = static_cast<std::uint32_t>(sum);
        }
    } else {
        std::int64_t mac =
            static_cast<std::int64_t>((static_cast<std::uint64_t>(c.mach) << 32) | c.macl);
        mac += prod;
        c.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(mac) >> 32);
        c.macl = static_cast<std::uint32_t>(mac);
    }
}

inline std::uint32_t cmp_str(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t x = a ^ b;
    return ((x & 0xFF000000u) == 0) || ((x & 0x00FF0000u) == 0) || ((x & 0x0000FF00u) == 0) ||
           ((x & 0x000000FFu) == 0);
}

// LDC Rm,SR: writes SR, swapping register banks if RB changes, and refreshes the cached T.
inline void write_sr(Ctx& c, std::uint32_t v) noexcept {
    const bool old_rb = (c.sr & SR_RB) != 0, new_rb = (v & SR_RB) != 0;
    if (old_rb != new_rb) {
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t tmp = c.r[i];
            c.r[i] = c.r_bank[i];
            c.r_bank[i] = tmp;
        }
    }
    // A change to BL or IMASK may unmask a pending interrupt: force a poll at the next safe point.
    if (((c.sr ^ v) & (SR_BL | SR_IMASK)) != 0)
        c.next_event = 0;
    c.sr = v & 0x700083F3u;  // architecturally defined bits only
    c.t = v & SR_T;
}

inline std::uint32_t read_sr(const Ctx& c) noexcept {
    return (c.sr & ~SR_T) | (c.t & 1u);
}

// FRCHG: swap the two FP banks.
inline void frchg(Ctx& c) noexcept {
    for (int i = 0; i < 16; ++i) {
        const float tmp = c.fr[i];
        c.fr[i] = c.xf[i];
        c.xf[i] = tmp;
    }
    c.fpscr ^= FPSCR_FR;
}

}  // namespace dream::sh4

// ---------------------------------------------------------------------------------------------
// FPU helpers. The register file is 32 singles; a double DRn occupies FRn (high 32 bits of
// the IEEE pattern) and FRn+1 (low 32 bits), which is the SH-4's own layout, so doubles are
// assembled and split here rather than stored separately. c.fr is always the front bank.
// ---------------------------------------------------------------------------------------------

namespace dream::sh4 {

inline std::uint32_t f2u(float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float u2f(std::uint32_t u) noexcept {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

inline double get_dr(const Ctx& c, unsigned n) noexcept {
    const std::uint64_t bits = (static_cast<std::uint64_t>(f2u(c.fr[n])) << 32) | f2u(c.fr[n + 1]);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
inline void set_dr(Ctx& c, unsigned n, double d) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    c.fr[n] = u2f(static_cast<std::uint32_t>(bits >> 32));
    c.fr[n + 1] = u2f(static_cast<std::uint32_t>(bits));
}
inline double get_xd(const Ctx& c, unsigned n) noexcept {
    const std::uint64_t bits = (static_cast<std::uint64_t>(f2u(c.xf[n])) << 32) | f2u(c.xf[n + 1]);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
inline void set_xd(Ctx& c, unsigned n, double d) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    c.xf[n] = u2f(static_cast<std::uint32_t>(bits >> 32));
    c.xf[n + 1] = u2f(static_cast<std::uint32_t>(bits));
}

// 64-bit pair moves (FPSCR.SZ = 1). The register field's low bit selects the back bank (XDn).
inline void fmov_pair_load(Ctx& c, unsigned field, std::uint64_t v) noexcept {
    float* bank = (field & 1) ? c.xf : c.fr;
    const unsigned n = field & ~1u;
    bank[n] = u2f(static_cast<std::uint32_t>(v >> 32));
    bank[n + 1] = u2f(static_cast<std::uint32_t>(v));
}
inline std::uint64_t fmov_pair_store(const Ctx& c, unsigned field) noexcept {
    const float* bank = (field & 1) ? c.xf : c.fr;
    const unsigned n = field & ~1u;
    return (static_cast<std::uint64_t>(f2u(bank[n])) << 32) | f2u(bank[n + 1]);
}
// Memory order of a pair: the word at the lower address is FRn (the high half).
// Guest memory through bases held in the emitted function's locals:
// main RAM for loads (`ram`, Memory::fast_ram), for stores (`st`, RAM while fast_stores holds) and
// the store queues (`sq`, fast_sq), with the same results as Memory's own accessors. Held in locals
// because a guest store may alias anything, so the compiler would re-read them after every store.
// They are taken at function entry (mem_bases); what changes them (the write hash switched on) does
// so only around calls, and stale fast bases still store to RAM.
//
// A view is RAM's base and a mask of which of the 64 regions (an address's top six bits) are RAM:
// area 3 under P0-P3, or none when the fast path is off. An access running past RAM's last byte
// (only an unaligned one could) lands in the 8 bytes DcMemory allocates past it, as its slow path
// does, so the end needs no test.
struct MemView {
    std::uint8_t* base;
    std::uint64_t regions;
};
constexpr std::uint64_t kRamRegions = 0x0008080808080808ull;  // top bits 0x0C, 0x2C, ..., 0xCC
inline void mem_bases(::dream::Memory& m, MemView& ram, MemView& st, std::uint32_t*& sq) noexcept {
    ram = {m.fast_ram, m.fast_ram ? kRamRegions : 0};
    st = {m.fast_ram, m.fast_ram && m.fast_stores ? kRamRegions : 0};
    sq = m.fast_sq;
}
template <typename T>
[[gnu::always_inline]] inline bool mem_fast(MemView v, std::uint32_t a) noexcept {
    return ((v.regions >> (a >> 26)) & 1u) != 0;
}
// A run of stack pushes or pops (emit.cpp, memcache::stack_runs): whether all of [lo, lo + bytes)
// is fast RAM in one 16 MB window, so each word of it can be accessed without its own test.
[[gnu::always_inline]] inline bool mem_run(MemView v, std::uint32_t lo, std::uint32_t bytes) noexcept {
    return ((v.regions >> (lo >> 26)) & 1u) != 0 && (lo >> 24) == ((lo + bytes - 1u) >> 24);
}
// PREF @Rn: to the store queues' area, a flush; anywhere else, a cache hint with no effect here.
// With the clock in a local (`cy`), only a flush that does not go to RAM can read the clock, so
// only that one writes it back.
[[gnu::always_inline]] inline void pref_sq(::dream::Memory& m, std::uint32_t a) {
    if ((a & 0xFC000000u) == 0xE0000000u)
        m.sq_flush_fast(a);
}
[[gnu::always_inline]] inline void pref_sq(Ctx& c, std::uint64_t& cy, ::dream::Memory& m, std::uint32_t a) {
    if ((a & 0xFC000000u) != 0xE0000000u)
        return;
    if (m.sq_to_ram[(a >> 5) & 1u]) {
        m.sq_flush_fast(a);
        return;
    }
    c.cycles = cy;
    m.sq_flush_rare(a);
    cy = c.cycles;
}
// The run's first byte in host memory, and a word at an offset from it.
[[gnu::always_inline]] inline std::uint8_t* mem_run_at(MemView v, std::uint32_t lo) noexcept {
    return v.base + (lo & 0x00FFFFFFu);
}
[[gnu::always_inline]] inline void st_at(std::uint8_t* p, unsigned off, std::uint32_t x) noexcept {
    std::memcpy(p + off, &x, 4);
}
[[gnu::always_inline]] inline std::uint32_t ld_at(const std::uint8_t* p, unsigned off) noexcept {
    std::uint32_t x;
    std::memcpy(&x, p + off, 4);
    return x;
}
template <typename T>
[[gnu::always_inline]] inline bool mem_sq(const std::uint32_t* sq, std::uint32_t a) noexcept {
    if constexpr (sizeof(T) == 4 || sizeof(T) == 8)
        return sq && ::dream::Memory::is_sq(a) && (sizeof(T) == 4 || (a & 7u) == 0);
    else
        return false;
}
// The slow paths, out of line and cold, so the hot code at each access site stays dense. With the
// clock: the context's clock is set first (`cy`, by value), and the caller takes it back after.
template <typename T>
[[gnu::cold, gnu::noinline]] DREAM_RARE_CALL T mem_slow_ld(::dream::Memory& m, std::uint32_t a) {
    if constexpr (sizeof(T) == 1)
        return m.read8_slow(a);
    else if constexpr (sizeof(T) == 2)
        return m.read16_slow(a);
    else if constexpr (sizeof(T) == 4)
        return m.read32_slow(a);
    else
        return m.read64_slow(a);
}
template <typename T>
[[gnu::cold, gnu::noinline]] DREAM_RARE_CALL void mem_slow_st(::dream::Memory& m, std::uint32_t a, T v) {
    if constexpr (sizeof(T) == 1)
        m.write8_slow(a, v);
    else if constexpr (sizeof(T) == 2)
        m.write16_slow(a, v);
    else if constexpr (sizeof(T) == 4)
        m.write32_slow(a, v);
    else
        m.write64_slow(a, v);
}
template <typename T>
[[gnu::cold, gnu::noinline]] DREAM_RARE_CALL T mem_slow_ld(Ctx& c, ::dream::Memory& m, std::uint32_t a, std::uint64_t cy) {
    c.cycles = cy;
    if constexpr (sizeof(T) == 1)
        return m.read8_slow(a);
    else if constexpr (sizeof(T) == 2)
        return m.read16_slow(a);
    else if constexpr (sizeof(T) == 4)
        return m.read32_slow(a);
    else
        return m.read64_slow(a);
}
template <typename T>
[[gnu::cold, gnu::noinline]] DREAM_RARE_CALL void mem_slow_st(Ctx& c, ::dream::Memory& m, std::uint32_t a, T v, std::uint64_t cy) {
    c.cycles = cy;
    if constexpr (sizeof(T) == 1)
        m.write8_slow(a, v);
    else if constexpr (sizeof(T) == 2)
        m.write16_slow(a, v);
    else if constexpr (sizeof(T) == 4)
        m.write32_slow(a, v);
    else
        m.write64_slow(a, v);
}
template <typename T>
[[gnu::always_inline]] inline T mem_ld(::dream::Memory& m, MemView ram, std::uint32_t a) {
    if (mem_fast<T>(ram, a)) {
        T v;
        std::memcpy(&v, ram.base + (a & 0x00FFFFFFu), sizeof(T));
        return v;
    }
    return mem_slow_ld<T>(m, a);
}
template <typename T>
[[gnu::always_inline]] inline void mem_st(::dream::Memory& m, MemView st, std::uint32_t* sq,
                                          std::uint32_t a, T v) {
    if (mem_fast<T>(st, a)) {
        std::memcpy(st.base + (a & 0x00FFFFFFu), &v, sizeof(T));
        return;
    }
    if (mem_sq<T>(sq, a)) {
        std::memcpy(sq + ((a >> 2) & 15u), &v, sizeof(T));
        return;
    }
    mem_slow_st<T>(m, a, v);
}
// The same with the guest clock in a local of the emitted function too (`cy`): a slow path may read the clock (a timer's count, the video line), so
// the clock goes back into the context first and is taken again after.
template <typename T>
[[gnu::always_inline]] inline T mem_ld(Ctx& c, std::uint64_t& cy, ::dream::Memory& m, MemView ram,
                                       std::uint32_t a) {
    if (mem_fast<T>(ram, a)) {
        T v;
        std::memcpy(&v, ram.base + (a & 0x00FFFFFFu), sizeof(T));
        return v;
    }
    const T v = mem_slow_ld<T>(c, m, a, cy);
    cy = c.cycles;
    return v;
}
template <typename T>
[[gnu::always_inline]] inline void mem_st(Ctx& c, std::uint64_t& cy, ::dream::Memory& m, MemView st,
                                          std::uint32_t* sq, std::uint32_t a, T v) {
    if (mem_fast<T>(st, a)) {
        std::memcpy(st.base + (a & 0x00FFFFFFu), &v, sizeof(T));
        return;
    }
    if (mem_sq<T>(sq, a)) {
        std::memcpy(sq + ((a >> 2) & 15u), &v, sizeof(T));
        return;
    }
    mem_slow_st<T>(c, m, a, v, cy);
    cy = c.cycles;
}
// The forms the pass writes: rd32(m, mr_, a) and wr32(m, ms_, mq_, a, v); with the clock,
// rd32(c, cy_, m, mr_, a) and wr32(c, cy_, m, ms_, mq_, a, v).
using MemoryRef = ::dream::Memory&;
using MemRam = MemView;
using MemStore = MemView;
using MemQueue = std::uint32_t*;
using MemClock = std::uint64_t&;
[[gnu::always_inline]] inline std::uint8_t rd8(MemoryRef m, MemRam r,std::uint32_t a) { return mem_ld<std::uint8_t>(m, r, a); }
[[gnu::always_inline]] inline std::uint16_t rd16(MemoryRef m, MemRam r,std::uint32_t a) { return mem_ld<std::uint16_t>(m, r, a); }
[[gnu::always_inline]] inline std::uint32_t rd32(MemoryRef m, MemRam r,std::uint32_t a) { return mem_ld<std::uint32_t>(m, r, a); }
[[gnu::always_inline]] inline void wr8(MemoryRef m, MemStore s, MemQueue q,std::uint32_t a, std::uint8_t v) { mem_st<std::uint8_t>(m, s, q, a, v); }
[[gnu::always_inline]] inline void wr16(MemoryRef m, MemStore s, MemQueue q,std::uint32_t a, std::uint16_t v) { mem_st<std::uint16_t>(m, s, q, a, v); }
[[gnu::always_inline]] inline void wr32(MemoryRef m, MemStore s, MemQueue q,std::uint32_t a, std::uint32_t v) { mem_st<std::uint32_t>(m, s, q, a, v); }
[[gnu::always_inline]] inline std::uint8_t rd8(Ctx& c, MemClock y,MemoryRef m, MemRam r,std::uint32_t a) { return mem_ld<std::uint8_t>(c, y, m, r, a); }
[[gnu::always_inline]] inline std::uint16_t rd16(Ctx& c, MemClock y,MemoryRef m, MemRam r,std::uint32_t a) { return mem_ld<std::uint16_t>(c, y, m, r, a); }
[[gnu::always_inline]] inline std::uint32_t rd32(Ctx& c, MemClock y,MemoryRef m, MemRam r,std::uint32_t a) { return mem_ld<std::uint32_t>(c, y, m, r, a); }
[[gnu::always_inline]] inline void wr8(Ctx& c, MemClock y,MemoryRef m, MemStore s, MemQueue q,std::uint32_t a, std::uint8_t v) { mem_st<std::uint8_t>(c, y, m, s, q, a, v); }
[[gnu::always_inline]] inline void wr16(Ctx& c, MemClock y,MemoryRef m, MemStore s, MemQueue q,std::uint32_t a, std::uint16_t v) { mem_st<std::uint16_t>(c, y, m, s, q, a, v); }
[[gnu::always_inline]] inline void wr32(Ctx& c, MemClock y,MemoryRef m, MemStore s, MemQueue q,std::uint32_t a, std::uint32_t v) { mem_st<std::uint32_t>(c, y, m, s, q, a, v); }
[[gnu::always_inline]] inline std::uint64_t rd_pair(MemoryRef m, MemRam r,std::uint32_t a) {
    return (static_cast<std::uint64_t>(rd32(m, r, a)) << 32) | rd32(m, r, a + 4);
}
[[gnu::always_inline]] inline void wr_pair(MemoryRef m, MemStore s, MemQueue q,std::uint32_t a, std::uint64_t v) {
    wr32(m, s, q, a, static_cast<std::uint32_t>(v >> 32));
    wr32(m, s, q, a + 4, static_cast<std::uint32_t>(v));
}
[[gnu::always_inline]] inline std::uint64_t rd_pair(Ctx& c, MemClock y,MemoryRef m, MemRam r,std::uint32_t a) {
    return (static_cast<std::uint64_t>(rd32(c, y, m, r, a)) << 32) | rd32(c, y, m, r, a + 4);
}
[[gnu::always_inline]] inline void wr_pair(Ctx& c, MemClock y,MemoryRef m, MemStore s, MemQueue q,std::uint32_t a,
                                           std::uint64_t v) {
    wr32(c, y, m, s, q, a, static_cast<std::uint32_t>(v >> 32));
    wr32(c, y, m, s, q, a + 4, static_cast<std::uint32_t>(v));
}

inline std::uint64_t read_pair(::dream::Memory& m, std::uint32_t addr) {
    return (static_cast<std::uint64_t>(m.read32(addr)) << 32) | m.read32(addr + 4);
}
inline void write_pair(::dream::Memory& m, std::uint32_t addr, std::uint64_t v) {
    m.write32(addr, static_cast<std::uint32_t>(v >> 32));
    m.write32(addr + 4, static_cast<std::uint32_t>(v));
}

// FTRC: truncate toward zero with saturation; NaN gives the negative-overflow pattern.
inline std::uint32_t ftrc(float f) noexcept {
    if (std::isnan(f))
        return 0x80000000u;
    if (f >= 2147483648.0f)
        return 0x7FFFFFFFu;
    if (f <= -2147483648.0f)
        return 0x80000000u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(f));
}
inline std::uint32_t ftrc(double d) noexcept {
    if (std::isnan(d))
        return 0x80000000u;
    if (d >= 2147483648.0)
        return 0x7FFFFFFFu;
    if (d <= -2147483648.0)
        return 0x80000000u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(d));
}

// FMAC: FRn = FR0 * FRm + FRn with a single rounding (the SH-4 FMAC is a fused unit). This is the
// one place the choice lives.
inline float fmac(float fr0, float frm, float frn) noexcept {
    return std::fmaf(fr0, frm, frn);
}

// FSCA: FPUL holds an angle as a 16.16 fixed fraction of a turn in its low 16 bits. The result is
// a table lookup (fsca.cpp) using the hardware-captured coefficients Flycast ships, not sin/cos:
// the real unit is a table too, and computing live under the guest's rounding mode gave
// sin(quarter turn) = 0x3F7FFFFF where hardware gives exactly 1.0.
namespace detail {
extern const std::uint32_t kFscaCoefs[0x8000];  // fsca.cpp: sin over the first half-turn
// The sine of angle i (a 16-bit fraction of a turn) as its bits: the second half-turn the first's
// negation. The cosine is the sine a quarter turn on.
inline std::uint32_t fsca_sin_bits(std::uint32_t i) noexcept {
    return kFscaCoefs[i & 0x7FFFu] ^ ((i & 0x8000u) << 16);
}
}  // namespace detail

inline void fsca(Ctx& c, unsigned n) noexcept {
    const std::uint32_t i = c.fpul & 0xFFFFu;
    c.fr[n] = u2f(detail::fsca_sin_bits(i));
    c.fr[n + 1] = u2f(detail::fsca_sin_bits(i + 0x4000u));
}

// FIPR FVm,FVn: FR[n+3] = dot(FV[m], FV[n]). Products are exact in double (24+24 significand
// bits), the sum is accumulated in double left to right and rounded to single once: this is the
// reference (Flycast) evaluation and it is contraction-proof, since an FMA of an exact product
// equals the separate multiply and add.
inline void fipr(Ctx& c, unsigned m, unsigned n) noexcept {
    const float* a = &c.fr[n];
    const float* b = &c.fr[m];
    double d = static_cast<double>(a[0]) * b[0];
    d += static_cast<double>(a[1]) * b[1];
    d += static_cast<double>(a[2]) * b[2];
    d += static_cast<double>(a[3]) * b[3];
    c.fr[n + 3] = static_cast<float>(d);
}

// FTRV XMTRX,FVn: FV[n] = XMTRX * FV[n], XMTRX being the back bank as a 4x4 column-major matrix.
// Same double accumulation as FIPR, one row at a time, left to right.
inline void ftrv(Ctx& c, unsigned n) noexcept {
    const float* v = &c.fr[n];
    const float x = v[0], y = v[1], z = v[2], w = v[3];
    const float* xm = c.xf;
    // Each row before any is written back (FV[n] is the input); no local array (see ftrv_x).
    const double r0 = static_cast<double>(xm[0]) * x + static_cast<double>(xm[4]) * y +
                      static_cast<double>(xm[8]) * z + static_cast<double>(xm[12]) * w;
    const double r1 = static_cast<double>(xm[1]) * x + static_cast<double>(xm[5]) * y +
                      static_cast<double>(xm[9]) * z + static_cast<double>(xm[13]) * w;
    const double r2 = static_cast<double>(xm[2]) * x + static_cast<double>(xm[6]) * y +
                      static_cast<double>(xm[10]) * z + static_cast<double>(xm[14]) * w;
    const double r3 = static_cast<double>(xm[3]) * x + static_cast<double>(xm[7]) * y +
                      static_cast<double>(xm[11]) * z + static_cast<double>(xm[15]) * w;
    c.fr[n] = static_cast<float>(r0);
    c.fr[n + 1] = static_cast<float>(r1);
    c.fr[n + 2] = static_cast<float>(r2);
    c.fr[n + 3] = static_cast<float>(r3);
}

// The same operations on front-bank registers held in locals: emitted code keeps FR0..FR15 in local
// variables within a block (the translator's register cache). Same arithmetic, in the same order.
inline double dr_of(float hi, float lo) noexcept {
    const std::uint64_t bits = (static_cast<std::uint64_t>(f2u(hi)) << 32) | f2u(lo);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
inline void dr_set(float& hi, float& lo, double d) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    hi = u2f(static_cast<std::uint32_t>(bits >> 32));
    lo = u2f(static_cast<std::uint32_t>(bits));
}
inline void pair_set(float& hi, float& lo, std::uint64_t v) noexcept {
    hi = u2f(static_cast<std::uint32_t>(v >> 32));
    lo = u2f(static_cast<std::uint32_t>(v));
}
inline std::uint64_t pair_of(float hi, float lo) noexcept {
    return (static_cast<std::uint64_t>(f2u(hi)) << 32) | f2u(lo);
}
inline float fipr_of(float a0, float a1, float a2, float a3, float b0, float b1, float b2,
                     float b3) noexcept {
    double d = static_cast<double>(a0) * b0;
    d += static_cast<double>(a1) * b1;
    d += static_cast<double>(a2) * b2;
    d += static_cast<double>(a3) * b3;
    return static_cast<float>(d);
}
// FTRV with XMTRX from locals too (x0..x15 are XF0..XF15), so the matrix can stay in registers
// across a loop of transforms. Written out row by row, not as a loop over arrays, which at low
// optimisation levels puts the matrix on the stack and brings in the stack protector.
inline void ftrv_x(float x0, float x1, float x2, float x3, float x4, float x5, float x6, float x7,
                   float x8, float x9, float x10, float x11, float x12, float x13, float x14,
                   float x15, float& v0, float& v1, float& v2, float& v3) noexcept {
    const float x = v0, y = v1, z = v2, w = v3;
    const double r0 = static_cast<double>(x0) * x + static_cast<double>(x4) * y +
                      static_cast<double>(x8) * z + static_cast<double>(x12) * w;
    const double r1 = static_cast<double>(x1) * x + static_cast<double>(x5) * y +
                      static_cast<double>(x9) * z + static_cast<double>(x13) * w;
    const double r2 = static_cast<double>(x2) * x + static_cast<double>(x6) * y +
                      static_cast<double>(x10) * z + static_cast<double>(x14) * w;
    const double r3 = static_cast<double>(x3) * x + static_cast<double>(x7) * y +
                      static_cast<double>(x11) * z + static_cast<double>(x15) * w;
    v0 = static_cast<float>(r0);
    v1 = static_cast<float>(r1);
    v2 = static_cast<float>(r2);
    v3 = static_cast<float>(r3);
}
inline void fsca_set(const Ctx& c, float& s, float& co) noexcept {
    const std::uint32_t i = c.fpul & 0xFFFFu;
    s = u2f(detail::fsca_sin_bits(i));
    co = u2f(detail::fsca_sin_bits(i + 0x4000u));
}

// LDS Rm,FPSCR: store, swap banks if FR changed, and program the host rounding/denormal mode.
inline void write_fpscr(Ctx& c, std::uint32_t v) noexcept {
    v &= 0x003FFFFFu;
    if ((v ^ c.fpscr) & FPSCR_FR) {
        for (int i = 0; i < 16; ++i) {
            const float tmp = c.fr[i];
            c.fr[i] = c.xf[i];
            c.xf[i] = tmp;
        }
    }
    c.fpscr = v;
    ::dream::fenv::apply(::dream::fenv::from_fpscr(v));
}

}  // namespace dream::sh4
