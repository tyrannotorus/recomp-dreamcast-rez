// Ninja2 (2.01.011) chunk-model drawing and Rez's wireframe lines, replaced natively.
// njCnkDrawModel (0x8C01AF76) transforms a model's vertex chunks into 32-byte records in Ninja's
// work buffer, and its strip writers turn those into PowerVR vertex parameters in Kamui's lists.
// Each native function leaves what the original leaves: the same guest memory (stack slots and
// globals with their final values), registers and guest cycles, using the runtime's FTRV, FIPR and
// FMAC models in the original's order. It polls for interrupts only at entry.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/sh4/ops.h"
#include "dreamcomp/hook.h"
#include "native_dev.h"

namespace {

using dream::Memory;
using dream::sh4::Ctx;
using dream::sh4::cost;
using dream::sh4::f2u;
using dream::sh4::fmac;
using dream::sh4::u2f;

// ---- guest memory as the native functions read it -------------------------------------------
// A pointer to `len` bytes of main RAM at `a`, checked once per call; null when any of it is not
// RAM (the translated code then runs).
const std::uint8_t* ram_span(const Memory& m, std::uint32_t a, std::uint32_t len) {
    if (!m.fast_ram || !Memory::is_ram(a))
        return nullptr;
    const std::uint32_t o = a & 0x00FFFFFFu;
    if (len > 0x01000000u - o)
        return nullptr;
    return m.fast_ram + o;
}
std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
float rdf(const std::uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}
std::uint32_t rd16s(const std::uint8_t* p) {
    std::int16_t v;
    std::memcpy(&v, p, 2);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
}
// FNEG: the sign bit flipped, whatever the value.
float neg(float f) {
    return u2f(f2u(f) ^ 0x80000000u);
}

// A guest register file: what a native function reads at entry and leaves at return.
struct Regs {
    std::uint32_t r[16];
    float f[16];
    std::uint32_t t;
};

// One 32-byte record.
struct Record {
    std::uint32_t w[8];
};
// A native function's stores and the guest cycles it spends.
class Out {
public:
    explicit Out(Memory& m) : m_(m), direct_(m.fast_stores && m.fast_sq && m.sq_ram) {}

    // A word into guest memory: a stack slot or a global, with the value the original leaves.
    void st32(std::uint32_t a, std::uint32_t v) { m_.write32(a, v); }
    void st16(std::uint32_t a, std::uint32_t v) { m_.write16(a, static_cast<std::uint16_t>(v)); }
    std::uint32_t ld32(std::uint32_t a) { return m_.read32(a); }
    float ldf(std::uint32_t a) { return u2f(ld32(a)); }
    std::uint32_t ld16s(std::uint32_t a) {
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(m_.read16(a))));
    }
    // A 32-byte record at `a` and the `pref` after it. With the queues mapped to RAM, a queue
    // address is written straight to the queue's destination; any other is staged and stored by
    // end(). Words are written in place through put8: a record built aside and copied stalls on
    // the copy's loads.
    [[gnu::always_inline]] std::uint8_t* begin(std::uint32_t a) {
        if (direct_ && Memory::is_sq(a)) {
            const unsigned n = (a >> 5) & 1u;
            if (m_.sq_to_ram[n]) {
                const std::uint32_t dest = (m_.sq_area[n] | (a & 0x03FFFFE0u)) & 0x00FFFFFFu;
                last_[n] = dest;
                wrote_[n] = true;
                return m_.sq_ram + dest;
            }
        }
        staged_ = true;
        return stage_;
    }
    [[gnu::always_inline]] void end(std::uint32_t a) {
        if (staged_) [[unlikely]]
            end_staged(a);
    }
    // Out of line and cold, so that `end` inlines into the vertex loops without costing them the
    // matrix in registers.
    [[gnu::noinline, gnu::cold]] void end_staged(std::uint32_t a) {
        staged_ = false;
        Record r;
        std::memcpy(r.w, stage_, 32);
        for (unsigned i = 0; i < 8; ++i) m_.write32(a + 4 * i, r.w[i]);
        if (Memory::is_sq(a))
            m_.sq_flush_fast(a);
    }
    // The words of a record in `mask` and the `pref` after them (Kamui's shorter global
    // parameters): the flush takes the rest from the last record the queue was given.
    void partial(std::uint32_t a, const std::uint32_t (&w)[8], std::uint32_t mask) {
        if (direct_ && Memory::is_sq(a) && m_.sq_to_ram[(a >> 5) & 1u]) {
            const unsigned n = (a >> 5) & 1u;
            std::uint32_t q[8];
            std::memcpy(q, wrote_[n] ? static_cast<const void*>(m_.sq_ram + last_[n])
                                     : static_cast<const void*>(m_.fast_sq + 8 * n),
                        32);
            for (unsigned k = 0; k < 8; ++k)
                if (mask >> k & 1u)
                    q[k] = w[k];
            std::memcpy(begin(a), q, 32);
            end(a);
            return;
        }
        for (unsigned k = 0; k < 8; ++k)
            if (mask >> k & 1u)
                m_.write32(a + 4 * k, w[k]);
        if (Memory::is_sq(a))
            m_.sq_flush_fast(a);
    }
    // Leaves each queue holding the last record it was given, as the original's stores do. Called
    // before guest code runs and at the end.
    void finish() {
        for (unsigned n = 0; n < 2; ++n)
            if (wrote_[n]) {
                std::memcpy(m_.fast_sq + 8 * n, m_.sq_ram + last_[n], 32);
                wrote_[n] = false;
            }
    }

    std::uint64_t cycles = 0;

private:
    Memory& m_;
    bool direct_, staged_ = false;
    std::uint32_t last_[2] = {0, 0};
    bool wrote_[2] = {false, false};
    alignas(16) std::uint8_t stage_[32];
};

// One record of eight words. Forced inline: GCC otherwise emits a call per vertex.
[[gnu::always_inline]] inline void put8(Out& o, std::uint32_t a, std::uint32_t w0, std::uint32_t w1,
                                        std::uint32_t w2, std::uint32_t w3, std::uint32_t w4,
                                        std::uint32_t w5, std::uint32_t w6, std::uint32_t w7) {
    std::uint8_t* d = o.begin(a);
    std::memcpy(d, &w0, 4);
    std::memcpy(d + 4, &w1, 4);
    std::memcpy(d + 8, &w2, 4);
    std::memcpy(d + 12, &w3, 4);
    std::memcpy(d + 16, &w4, 4);
    std::memcpy(d + 20, &w5, 4);
    std::memcpy(d + 24, &w6, 4);
    std::memcpy(d + 28, &w7, 4);
    o.end(a);
}

// XMTRX widened to double once per call; apply() computes FTRV as the runtime's ftrv_x (ops.h).
struct Mat {
    double m[16];
    explicit Mat(const Ctx& c) {
        for (unsigned i = 0; i < 16; ++i) m[i] = static_cast<double>(c.xf[i]);
    }
    // Forced inline: the Brick's build otherwise calls it out of line in vertex loops.
    [[gnu::always_inline]] void apply(float& v0, float& v1, float& v2, float& v3) const {
        const double x = v0, y = v1, z = v2, w = v3;
        const double r0 = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
        const double r1 = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
        const double r2 = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
        const double r3 = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
        v0 = static_cast<float>(r0);
        v1 = static_cast<float>(r1);
        v2 = static_cast<float>(r2);
        v3 = static_cast<float>(r3);
    }
};

// The three rows of XMTRX a vertex loop uses, summed in apply's order, for a point (w = 1:
// m12..m14 added as they are) and a direction (w = 0: m12..m14 times zero, computed once). The
// caller computes the fourth row where it needs it. Spelled out because the Brick's build compiles
// floating point strictly and would not drop apply's unused row or its products with 1 and 0.
struct Rows {
    double m0, m1, m2, m4, m5, m6, m8, m9, m10, m12, m13, m14, z12, z13, z14;
    explicit Rows(const Mat& M)
        : m0(M.m[0]), m1(M.m[1]), m2(M.m[2]), m4(M.m[4]), m5(M.m[5]), m6(M.m[6]), m8(M.m[8]),
          m9(M.m[9]), m10(M.m[10]), m12(M.m[12]), m13(M.m[13]), m14(M.m[14]), z12(M.m[12] * 0.0),
          z13(M.m[13] * 0.0), z14(M.m[14] * 0.0) {}
    void point(float& v0, float& v1, float& v2) const {
        const double x = v0, y = v1, z = v2;
        v0 = static_cast<float>(m0 * x + m4 * y + m8 * z + m12);
        v1 = static_cast<float>(m1 * x + m5 * y + m9 * z + m13);
        v2 = static_cast<float>(m2 * x + m6 * y + m10 * z + m14);
    }
    void direction(float& v0, float& v1, float& v2) const {
        const double x = v0, y = v1, z = v2;
        v0 = static_cast<float>(m0 * x + m4 * y + m8 * z + z12);
        v1 = static_cast<float>(m1 * x + m5 * y + m9 * z + z13);
        v2 = static_cast<float>(m2 * x + m6 * y + m10 * z + z14);
    }
};

// The original's entry poll: every emitted function starts with one, and a hook runs before it.
void entry_poll(Ctx& c, Memory& m, std::uint32_t entry) {
    if (c.cycles >= c.next_event) {
        c.pc = entry;
        dream::sh4::deliver_irq(c, m);
    }
}

// ---- running a native function -----------------------------------------------------------------
// `fn(c, x, o)` computes from the registers in `x` and guest memory, leaves the return registers in
// `x`, stores through `o` and counts its cycles there; false (before any store) leaves the call to
// the translated code. The entry hook applies the result and skips the original.
// `abi`: the original is compiled C, so only what its callers see is compared (r8-r15, fr12-fr15
// and memory outside its own frames). It may call guest code on rare paths (guest_call).
template <typename F>
bool run_native(Ctx& c, Memory& m, F& fn) {
    Regs x;
    std::memcpy(x.r, c.r, sizeof x.r);
    std::memcpy(x.f, c.fr, sizeof x.f);
    x.t = c.t;
    const std::uint32_t pr = c.pr;
    Out o(m);
    const std::uint64_t start = c.cycles;
    const bool done = fn(c, x, o);
    if (c.unwinding) {
        c.cycles += o.cycles;  // a guest call's non-local return: it carries on from there
        return true;
    }
    if (!done) {
        c.cycles = start;
        return false;
    }
    o.finish();
    std::memcpy(c.r, x.r, sizeof x.r);
    std::memcpy(c.fr, x.f, sizeof x.f);
    c.t = x.t;
    c.pr = pr;
    c.cycles += o.cycles;
    return true;
}

template <typename F>
bool hook_entry(Ctx& c, Memory& m, std::uint32_t entry, F&& fn) {
    entry_poll(c, m, entry);
    if (c.unwinding)
        return true;
    if (c.fpscr & (dream::sh4::FPSCR_SZ | dream::sh4::FPSCR_PR))
        return false;  // written for the mode Ninja calls them in
    return run_native(c, m, fn);
}

// A call from native code into guest code, as the original's jsr makes it: the arguments, the
// stack pointer and the return address the original would have, and the clock up to date first.
// False when the call unwinds (c.unwinding).
struct GuestArgs {
    std::uint32_t r4 = 0, r5 = 0, r6 = 0, r7 = 0;
    float f4 = 0.0f, f5 = 0.0f;
};
bool guest_call(Ctx& c, Memory& m, Out& o, std::uint32_t target, std::uint32_t ret,
                std::uint32_t sp, const GuestArgs& a, std::uint32_t& r0) {
    c.cycles += o.cycles;
    o.cycles = 0;
    o.finish();  // the queues as the guest code expects them
    c.r[4] = a.r4;
    c.r[5] = a.r5;
    c.r[6] = a.r6;
    c.r[7] = a.r7;
    c.fr[4] = a.f4;
    c.fr[5] = a.f5;
    c.r[15] = sp;
    c.pr = ret;
    // The native caller polls only at its entry; the guest code it calls must not take an
    // interrupt either (the next poll after the caller takes it), or the timeline would depend on
    // which parts are native.
    const std::uint64_t next = c.next_event;
    c.next_event = ~std::uint64_t{0};
    dream::sh4::call_indirect_in(c, m, target);
    c.next_event = next < c.next_event ? next : c.next_event;
    if (c.unwinding)
        return false;
    r0 = c.r[0];
    return true;
}

// ---- vertex chunk type 41: position and normal (0x8C05E240) ----------------------------------
// r4: Ninja's chunk descriptor. +0..+8 the light vector, +12 its scale, +20 the vertex count, +24
// the source vertices (position, then normal: 24 bytes), +28 the work buffer, +32 its store-queue
// address. The screen scale and centre are at the address in the literal at 0x8C05E2E4. A record:
// -z, screen x, screen y, 1/-z, the diffuse dot, its 17th power, the normal's x and -y. r0 counts
// the vertices nearer than z = -1.
bool vtx41(const Ctx& c, Regs& x, Out& o, Memory& m) {
    const std::uint32_t d = x.r[4];
    const std::uint8_t* dp = ram_span(m, d, 36);
    if (!dp)
        return false;
    const std::uint32_t n = rd32(dp + 20);
    const std::uint32_t src0 = rd32(dp + 24);
    const std::uint32_t r7 = rd32(dp + 28), sq0 = rd32(dp + 32);
    const std::uint32_t scr = m.read32(0x8C05E2E4u);
    const std::uint8_t* sp = ram_span(m, src0, 24 * n);
    const std::uint8_t* cp = ram_span(m, scr, 16);
    if (n == 0 || n > 0x10000u || !sp || !cp)
        return false;  // the translated code then runs (zero would loop 2^32 times there)
    const float s = rdf(dp + 12);
    const float lx = rdf(dp) * s, ly = rdf(dp + 4) * s, lz = rdf(dp + 8) * s, lw = 0.0f;
    const float ax = rdf(cp), ay = rdf(cp + 4), cx = rdf(cp + 8), cy = rdf(cp + 12);
    std::uint32_t near = 0;
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0, f6 = 0, f7 = 0;
    const Mat M(c);
    const Rows R(M);
    // FIPR's fourth product is the normal's fourth row times lw = 0: a signed zero, or NaN when
    // the row is not finite. A zero added to a nonzero sum (even NaN or infinite) changes nothing,
    // so the row is computed only when the sum is zero or the row could overflow (|m| <= 2^40 and
    // |n| <= 2^80 keep it under 2^122).
    const bool row3_small = std::fabs(M.m[3]) <= 0x1p40 && std::fabs(M.m[7]) <= 0x1p40 &&
                            std::fabs(M.m[11]) <= 0x1p40 && std::fabs(M.m[15]) <= 0x1p40;
    const auto small = [](std::uint32_t bits) { return (bits & 0x7FFFFFFFu) <= 0x67800000u; };
    const double dlx = lx, dly = ly, dlz = lz, dlw = lw;
    for (std::uint32_t i = 0; i < n; ++i, sp += 24) {
        f4 = rdf(sp);
        f5 = rdf(sp + 4);
        f6 = rdf(sp + 8);
        R.point(f4, f5, f6);  // FTRV's fourth result (fr7) is overwritten unread below
        const std::uint32_t nx = rd32(sp + 12), ny = rd32(sp + 16), nz = rd32(sp + 20);
        f0 = u2f(nx);
        f1 = u2f(ny);
        f2 = u2f(nz);
        R.direction(f0, f1, f2);
        std::uint32_t w0, w1, w2, w3, w4, w5, w6, w7;
        f6 = neg(f6);
        w0 = f2u(f6);
        // FIPR of the normal (fr0-fr3, fr3 its fourth row) with the light vector.
        double dot = static_cast<double>(f0) * dlx;
        dot += static_cast<double>(f1) * dly;
        dot += static_cast<double>(f2) * dlz;
        if (dot == 0.0 || !row3_small || !small(nx) || !small(ny) || !small(nz)) [[unlikely]] {
            const double vx = u2f(nx), vy = u2f(ny), vz = u2f(nz), vw = 0.0f;
            f3 = static_cast<float>(M.m[3] * vx + M.m[7] * vy + M.m[11] * vz + M.m[15] * vw);
            dot += static_cast<double>(f3) * dlw;
        }
        f3 = static_cast<float>(dot);
        f1 = neg(f1);
        w7 = f2u(f1);
        w6 = f2u(f0);
        f0 = 1.0f;
        f0 /= f6;
        f1 = cx;
        f2 = cy;
        f7 = 1.0f;
        f5 *= ay;
        const bool t = f7 > f6;
        f7 = f3;
        f4 *= ax;
        w4 = f2u(f3);
        f7 *= f7;
        near += t ? 1u : 0u;
        f7 *= f7;
        f2 = fmac(f0, f5, f2);
        w3 = f2u(f0);
        f7 *= f7;
        w2 = f2u(f2);
        f1 = fmac(f0, f4, f1);
        w1 = f2u(f1);
        f7 *= f7;
        f7 *= f3;
        w5 = f2u(f7);
        put8(o, sq0 + 32 * i, w0, w1, w2, w3, w4, w5, w6, w7);
    }
    // The four saved FR12..FR15 below the stack pointer, as the original leaves them.
    const std::uint32_t r15 = x.r[15];
    o.st32(r15 - 4, f2u(x.f[15]));
    o.st32(r15 - 8, f2u(x.f[14]));
    o.st32(r15 - 12, f2u(x.f[13]));
    o.st32(r15 - 16, f2u(x.f[12]));
    x.r[0] = near;
    x.r[1] = 0;
    x.r[2] = sq0 + 32 * n;
    x.r[3] = src0 + 24 * n;
    x.r[4] = scr + 16;
    x.r[5] = src0 + 24 + 24 * n;
    x.r[6] = 0;
    x.r[7] = r7 + 32 * n;
    const float fr[12] = {f0, f1, f2, f3, f4, f5, f6, f7, ax, ay, cx, cy};
    std::memcpy(x.f, fr, sizeof fr);
    x.t = 1;  // the loop's last `dt`
    o.cycles += cost(28) + std::uint64_t{n} * cost(49) + cost(5);
    return true;
}

// ---- vertex chunk types 34 (position) and 35 (position and colour): 0x8C05E2EC, 0x8C05E07C ----
// The same descriptor from +20. The screen scale and centre are at the literal's address (34:
// 0x8C05E358, 35: 0x8C05E0E8). A record: -z, screen x, screen y, 1/-z, 1.0 (34) or the vertex's
// colour (35), 0.0, the transformed x and y. r0 counts the vertices not beyond z = -1.
bool vtx_plain(const Ctx& c, Regs& x, Out& o, Memory& m, bool colour) {
    const std::uint32_t d = x.r[4];
    const std::uint8_t* dp = ram_span(m, d, 36);
    if (!dp)
        return false;
    const std::uint32_t n = rd32(dp + 20);
    const std::uint32_t stride = colour ? 16 : 12;
    const std::uint32_t src0 = rd32(dp + 24);
    const std::uint32_t r7 = rd32(dp + 28), sq0 = rd32(dp + 32);
    const std::uint32_t scr = m.read32(colour ? 0x8C05E0E8u : 0x8C05E358u);
    const std::uint8_t* sp = ram_span(m, src0, stride * n);
    const std::uint8_t* cp = ram_span(m, scr, 16);
    if (n == 0 || n > 0x10000u || !sp || !cp)
        return false;
    const float f0 = rdf(cp), f1 = rdf(cp + 4), f2 = rdf(cp + 8), f3 = rdf(cp + 12);
    const float f9 = 0.0f, f10 = 1.0f;
    std::uint32_t near = 0, word = 0;
    float f4 = 0, f5 = 0, f6 = 0, f7 = 0, f8 = 0;
    std::uint64_t cycles = cost(17) + cost(2);
    const Mat M(c);
    const Rows R(M);
    for (std::uint32_t i = 0; i < n; ++i, sp += stride) {
        f4 = rdf(sp);
        f5 = rdf(sp + 4);
        f6 = rdf(sp + 8);
        R.point(f4, f5, f6);  // FTRV's fourth result (fr7): only the last is seen, after the loop
        if (colour)
            word = rd32(sp + 12);
        f8 = 1.0f;
        f6 = neg(f6);
        f8 /= f6;
        std::uint32_t w0, w1, w2, w3, w4, w5, w6, w7;
        w7 = f2u(f5);
        f5 *= f1;
        w6 = f2u(f4);
        f4 *= f0;
        w5 = f2u(f9);
        const bool t = f6 > f10;
        w4 = colour ? word : f2u(f10);
        if (!t)
            ++near;
        f5 *= f8;
        w3 = f2u(f8);
        f4 *= f8;
        f5 += f3;
        w2 = f2u(f5);
        f4 += f2;
        w1 = f2u(f4);
        w0 = f2u(f6);
        put8(o, sq0 + 32 * i, w0, w1, w2, w3, w4, w5, w6, w7);
        // Per vertex: the block, then the near count's add (its own block) or the branch over it.
        cycles += (colour ? cost(20) : cost(19)) + (t ? 1u : cost(1)) + cost(14);
    }
    {
        const std::uint8_t* last = sp - stride;
        const double px = rdf(last), py = rdf(last + 4), pz = rdf(last + 8), pw = 1.0f;
        f7 = static_cast<float>(M.m[3] * px + M.m[7] * py + M.m[11] * pz + M.m[15] * pw);
    }
    x.r[0] = near;
    x.r[1] = 0;
    x.r[2] = sq0 + 32 * n;
    x.r[3] = src0 + stride * n;
    x.r[4] = colour ? word : scr + 16;
    x.r[5] = src0 + stride + stride * n;
    x.r[6] = near;
    x.r[7] = r7 + 32 * n;
    const float fr[11] = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10};
    std::memcpy(x.f, fr, sizeof fr);
    x.t = 1;
    o.cycles += cycles;
    return true;
}

// ---- strip writer A: 0x8C058E98, and 0x8C05921C which it continues into -----------------------
// Writes textured intensity vertex parameters (control word, x, y, 1/w, u, v, base and offset
// intensity) for Ninja strips whose vertices are work-buffer records. r4: the strip data (per
// strip a signed 16-bit length, negative for the other winding, then 16-bit record indices, each
// from the third on followed by 2 * r6 bits 14-15 bytes); r5: the work buffer; r6 bits 0-13: the
// strip count; r7: the destination (a store-queue address); fr4: the intensity base. Ninja's
// flags (0x8C120EF0) bit 17: u and v from the normal (an environment map) offset by 0x8C10EF04/08,
// else 0x8C05921C, the same with a fixed offset; bit 31 flips the winding.
//
// Each strip is split into runs by facing (the sign of each triangle's screen area, the winding
// alternating along the strip): front-facing runs lit by the normal (base = fr4 + dot, offset =
// the specular term), back-facing ones lit from behind (fr4 - dot, minus the specular). The vertex
// closing a run carries the end-of-strip bit. The totals at 0x8C120F08 (vertices written) and
// 0x8C120F04 (vertices read) grow.
constexpr std::uint32_t kFlags = 0x8C120EF0u, kWritten = 0x8C120F08u, kRead = 0x8C120F04u;
constexpr std::uint32_t kVertex = 0xE0000000u, kLast = 0xF0000000u;

// The strips' data and the work buffer, checked before anything is written.
struct Strips {
    const std::uint8_t* data;  // r4
    const std::uint8_t* wb;    // r5, as a pointer (the indices are signed)
    std::uint32_t skip;        // bytes after each index from the third on
};
// `uv`: each index is followed by a u and a v (strip writer B's data).
bool scan_strips(Memory& m, std::uint32_t r4, std::uint32_t r5, std::uint32_t r6, Strips& s,
                 bool uv = false) {
    const std::uint32_t per = uv ? 6 : 2;  // bytes per vertex
    const std::uint32_t count = r6 & 0x3FFFu;
    s.skip = ((r6 >> 14) & 3u) << 1;
    if (count == 0)
        return false;  // the original would loop 2^32 times
    const std::uint8_t* base = ram_span(m, r4, 2);
    if (!base)
        return false;
    // The data runs on until its strips end; it must stay in RAM.
    const std::uint32_t room = 0x01000000u - (r4 & 0x00FFFFFFu);
    std::uint32_t a = 0;
    std::int32_t lo = 0x7FFF, hi = -0x8000;
    auto index = [&](std::uint32_t at) {
        const std::int32_t i = static_cast<std::int32_t>(rd16s(base + at));
        lo = i < lo ? i : lo;
        hi = i > hi ? i : hi;
    };
    for (std::uint32_t k = 0; k < count; ++k) {
        if (room < a + 2 + 2 * per)
            return false;
        std::int32_t len = static_cast<std::int32_t>(rd16s(base + a));
        len = len < 0 ? -len : len;
        if (len < 3)
            return false;  // no triangle: the original's loop would run 2^32 times
        index(a + 2);
        index(a + 2 + per);
        a += 2 + 2 * per;
        const std::uint32_t tri = static_cast<std::uint32_t>(len - 2);
        if (room < a + tri * (per + s.skip))
            return false;
        for (std::uint32_t t = 0; t < tri; ++t) {
            index(a);
            a += per + s.skip;
        }
    }
    s.data = base;
    const std::uint32_t first = r5 + static_cast<std::uint32_t>(lo * 32);
    const std::uint8_t* wb = ram_span(m, first, static_cast<std::uint32_t>(hi - lo + 1) * 32);
    if (!wb)
        return false;
    s.wb = wb - static_cast<std::ptrdiff_t>(lo) * 32;
    return true;
}

// 0x8C05921C: the u and v from the normal with a fixed offset, kept in registers (FPSCR.SZ pair
// moves in the original). Entered with the caller's registers (0x8C058E98 restores them first).
// With `uv` it is strip writer B (0x8C05C094), the same but for the u and v: two signed 16-bit
// values after each index, times fr5.
template <bool uv>
bool strip_plain(Regs& x, Out& o, const Strips& s) {
    std::uint32_t r0, r1, r2, r3, r4 = x.r[4], r5 = x.r[5], r6, r7 = x.r[7], r8, r9, r10, r11, r12,
                                  r13, r14;
    float f0 = x.f[0], f1 = x.f[1], f2, f3, f4 = x.f[4], f5, f6 = x.f[6], f7 = x.f[7],
          f8 = x.f[8], f9, f10 = x.f[10], f11 = x.f[11], f12, f13, f14, f15;
    std::uint64_t cyc = 0;
    const std::uint32_t sp = x.r[15];
    // The saves, and the flags word at the new stack pointer.
    o.st32(sp - 4, x.r[8]);
    o.st32(sp - 8, x.r[9]);
    o.st32(sp - 12, x.r[10]);
    o.st32(sp - 16, x.r[11]);
    o.st32(sp - 20, x.r[12]);
    o.st32(sp - 24, x.r[13]);
    o.st32(sp - 28, x.r[14]);
    o.st32(sp - 32, f2u(x.f[12]));
    o.st32(sp - 36, f2u(x.f[13]));
    o.st32(sp - 40, f2u(x.f[14]));
    o.st32(sp - 44, f2u(x.f[15]));
    // PR at sp - 48 is stored by the callers, strip_a and strip_b.
    const std::uint32_t flags = o.ld32(kFlags);
    o.st32(sp - 52, flags);
    r1 = x.r[6] & 0x3FFFu;
    r6 = ((x.r[6] >> 14) & 3u) << 1;
    r2 = 0;
    r3 = 0;
    f9 = 1.0f;
    if constexpr (uv) {
        f5 = x.f[5];
        cyc += cost(31);
    } else {
        f5 = u2f(o.ld32(0x8C0594D4u));
        cyc += cost(33);
    }
    const std::uint8_t* wb = s.wb;
    const std::uint8_t* p = s.data + (r4 - x.r[4]);
    auto rec = [&](std::uint32_t idx) { return wb + static_cast<std::ptrdiff_t>(static_cast<std::int32_t>(idx)) * 32; };
    // One parameter: the record's x, y and 1/w, then u, v, base, offset.
    auto put = [&](std::uint32_t cw, float u, float v) {
        put8(o, r7, cw, f2u(f1), f2u(f2), f2u(f3), f2u(u), f2u(v), f2u(f6), f2u(f7));
        r7 += 32;
    };
    // The record's six words from +0 into fr0..fr3, fr6, fr7 (three pair loads).
    auto load = [&](const std::uint8_t* q) {
        f0 = rdf(q);
        f1 = rdf(q + 4);
        f2 = rdf(q + 8);
        f3 = rdf(q + 12);
        f6 = rdf(q + 16);
        f7 = rdf(q + 20);
    };
    const std::uint8_t *q9, *q10, *q11 = nullptr;
    do {
        // L_8c05925e: a strip.
        r8 = rd16s(p);
        r9 = rd16s(p + 2);
        p += 4;
        q9 = rec(r9);
        r9 = (r9 << 5) + r5;
        r0 = flags << 1;
        f9 = 1.0f;
        cyc += cost(8);
        if (flags >> 31) {
            f9 = neg(f9);
            cyc += cost(1);
        } else {
            cyc += 1;
        }
        cyc += cost(4);
        if (static_cast<std::int32_t>(r8) >= 0) {
            cyc += 1;
        } else {
            r8 = 0u - r8;
            f9 = neg(f9);
            cyc += cost(2);
        }
        r3 += r8;
        r8 -= 2;
        r12 = 0;
        r13 = 0;
        if constexpr (uv) {
            f10 = static_cast<float>(static_cast<std::int32_t>(rd16s(p))) * f5;
            f11 = static_cast<float>(static_cast<std::int32_t>(rd16s(p + 2))) * f5;
            p += 4;
        }
        r10 = rd16s(p);
        p += 2;
        q10 = rec(r10);
        r10 = (r10 << 5) + r5;
        if constexpr (uv) {
            f12 = static_cast<float>(static_cast<std::int32_t>(rd16s(p))) * f5;
            f13 = static_cast<float>(static_cast<std::int32_t>(rd16s(p + 2))) * f5;
            p += 4;
            cyc += cost(25);
        } else {
            f10 = rdf(q9 + 24) * f5;
            f11 = rdf(q9 + 28) * f5;
            f10 += f5;
            f11 += f5;
            f12 = rdf(q10 + 24) * f5;
            f13 = rdf(q10 + 28) * f5;
            f12 += f5;
            f13 += f5;
            cyc += cost(20);
        }
        for (;;) {
            // L_8c0592a4: a triangle, from the records at r9, r10 and r11.
            r11 = rd16s(p);
            p += 2;
            q11 = rec(r11);
            r11 = (r11 << 5) + r5;
            f0 = rdf(q9 + 4);
            f6 = rdf(q9 + 8);
            f1 = rdf(q10 + 4);
            f7 = rdf(q10 + 8);
            f0 -= f1;
            f6 -= f7;
            f2 = rdf(q11 + 4);
            f8 = rdf(q11 + 8);
            f2 -= f1;
            f8 -= f7;
            f8 *= f0;
            if constexpr (uv) {
                f14 = static_cast<float>(static_cast<std::int32_t>(rd16s(p))) * f5;
                f15 = static_cast<float>(static_cast<std::int32_t>(rd16s(p + 2))) * f5;
                p += 4;
            } else {
                f14 = rdf(q11 + 24);
                f15 = rdf(q11 + 28);
            }
            f6 *= f2;
            f7 = 0.0f;
            f6 -= f8;
            f6 *= f9;
            const bool front = f7 > f6;
            f9 = neg(f9);
            cyc += uv ? cost(42) : cost(33);
            if (front) {
                cyc += cost(2);
                if (r13 != 0) {  // L_8c0592ea: the back-facing run ends with the second vertex
                    load(q10);
                    r13 += 1;
                    f6 = neg(f6);
                    f6 += f4;
                    cyc += cost(14);
                    if (f4 > f6) {
                        f6 = f4;
                        cyc += cost(1);
                    } else {
                        cyc += 1;
                    }
                    f7 = neg(f7);
                    r14 = kLast;
                    r2 += r13;
                    r13 = 0;
                    put(r14, f12, f13);
                    cyc += cost(10);
                } else {
                    cyc += 1;
                }
                cyc += cost(2);
                if (r12 == 0) {  // L_8c059320: a front-facing run starts with the first vertex
                    load(q9);
                    r12 += 1;
                    f6 += f4;
                    cyc += cost(13);
                    if (f4 > f6) {
                        f6 = f4;
                        cyc += cost(1);
                    } else {
                        cyc += 1;
                    }
                    r14 = kVertex;
                    put(r14, f10, f11);
                    cyc += cost(7);
                } else {
                    cyc += 1;
                }
                // L_8c05934a: then the second.
                load(q10);
                r12 += 1;
                f6 += f4;
                cyc += cost(13);
                if (f4 > f6) {
                    f6 = f4;
                    cyc += cost(1);
                } else {
                    cyc += 1;
                }
                r14 = kVertex;
                put(r14, f12, f13);
                cyc += cost(8);
            } else {
                cyc += cost(2);
                if (r12 != 0) {  // L_8c05937a: the front-facing run ends
                    load(q10);
                    r12 += 1;
                    f6 += f4;
                    cyc += cost(13);
                    if (f4 > f6) {
                        f6 = f4;
                        cyc += cost(1);
                    } else {
                        cyc += 1;
                    }
                    r14 = kLast;
                    r2 += r12;
                    r12 = 0;
                    put(r14, f12, f13);
                    cyc += cost(9);
                } else {
                    cyc += 1;
                }
                cyc += cost(2);
                if (r13 == 0) {  // L_8c0593ac: a back-facing run starts
                    load(q9);
                    r13 += 1;
                    f6 = neg(f6);
                    f6 += f4;
                    cyc += cost(14);
                    if (f4 > f6) {
                        f6 = f4;
                        cyc += cost(1);
                    } else {
                        cyc += 1;
                    }
                    f7 = neg(f7);
                    r14 = kVertex;
                    put(r14, f10, f11);
                    cyc += cost(8);
                } else {
                    cyc += 1;
                }
                // L_8c0593da
                load(q10);
                r13 += 1;
                f6 = neg(f6);
                f6 += f4;
                cyc += cost(14);
                if (f4 > f6) {
                    f6 = f4;
                    cyc += cost(1);
                } else {
                    cyc += 1;
                }
                f7 = neg(f7);
                r14 = kVertex;
                put(r14, f12, f13);
                cyc += cost(8);
            }
            // L_8c059408 (B: L_8c05c298): the next triangle shares this one's last two vertices.
            if constexpr (!uv) {
                f14 *= f5;
                f15 *= f5;
                f14 += f5;
                f15 += f5;
            }
            p += r6;
            r9 = r10;
            q9 = q10;
            r8 -= 1;
            f10 = f12;
            f11 = f13;
            cyc += uv ? cost(6) : cost(10);
            f12 = f14;
            f13 = f15;
            if (r8 == 0)
                break;
            cyc += cost(2);
            r10 = r11;
            q10 = q11;
        }
        // L_8c059420: the open run ends with the strip's last vertex.
        cyc += cost(2);
        if (r12 != 0) {
            load(q11);
            r2 += r12;
            f6 += f4;
            cyc += cost(13);
            if (f4 > f6) {
                f6 = f4;
                cyc += cost(1);
            } else {
                cyc += 1;
            }
            r14 = kLast;
            r2 += 1;
            put(r14, f14, f15);
            cyc += cost(9);
        } else {
            cyc += 1;
            cyc += cost(2);
            if (r13 != 0) {
                load(q11);
                r2 += r13;
                f6 = neg(f6);
                f6 += f4;
                cyc += cost(14);
                if (f4 > f6) {
                    f6 = f4;
                    cyc += cost(1);
                } else {
                    cyc += 1;
                }
                f7 = neg(f7);
                r14 = kLast;
                r2 += 1;
                put(r14, f14, f15);
                cyc += cost(9);
            } else {
                cyc += 1;
            }
        }
        r1 -= 1;
        cyc += cost(2);
        if (r1 == 0)
            break;
        cyc += cost(2);
    } while (true);
    cyc += 1;
    // L_8c05948e: the totals, and the restores.
    const std::uint32_t written = o.ld32(kWritten) + r2, read = o.ld32(kRead) + r3;
    o.st32(kWritten, written);
    o.st32(kRead, read);
    cyc += cost(24);
    (void)r0;
    (void)r10;
    (void)r14;
    x.r[0] = r7;
    x.r[1] = r1;
    x.r[2] = r2;
    x.r[3] = r3;
    x.r[4] = kWritten;
    x.r[5] = kRead;
    x.r[6] = written;
    x.r[7] = read;
    const float fr[12] = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11};
    std::memcpy(x.f, fr, sizeof fr);
    x.t = 1;
    o.cycles += cyc;
    (void)q11;
    (void)r9;
    (void)r11;
    return true;
}

// Strip writer B (0x8C05C094): strips whose vertices carry their own u and v (Ninja's strip
// chunks 65 and 66, scaled by fr5), otherwise as 0x8C05921C. Its saves are the same, PR with them.
bool strip_b(std::uint32_t pr, Regs& x, Out& o, Memory& m) {
    Strips s;
    if (!scan_strips(m, x.r[4], x.r[5], x.r[6], s, true))
        return false;
    o.st32(x.r[15] - 48, pr);
    return strip_plain<true>(x, o, s);
}

bool strip_a(std::uint32_t pr, Regs& x, Out& o, Memory& m) {
    Strips s;
    if (!scan_strips(m, x.r[4], x.r[5], x.r[6], s))
        return false;
    const std::uint32_t flags = o.ld32(kFlags);
    const std::uint32_t sp = x.r[15];
    // The saves of 0x8C058E98's prologue (r14..r8, fr15..fr12).
    std::uint64_t cyc = cost(17);
    if (!(flags & 0x00020000u)) {
        // Restored again at once, and on to 0x8C05921C, whose saves land on the same words.
        cyc += 1 + cost(13);
        o.cycles += cyc;
        o.st32(sp - 48, pr);
        return strip_plain<false>(x, o, s);
    }
    std::uint32_t r0, r1, r2, r3, r4 = x.r[4], r5 = x.r[5], r6 = x.r[6], r7 = x.r[7], r8, r9, r10,
                                  r11, r12, r13, r14;
    float f0 = x.f[0], f1 = x.f[1], f2, f3, f4 = x.f[4], f5 = x.f[5], f6 = x.f[6], f7 = x.f[7],
          f8 = x.f[8], f9 = x.f[9], f10, f11, f12, f13, f14, f15;
    o.st32(sp - 4, x.r[14]);
    o.st32(sp - 8, x.r[13]);
    o.st32(sp - 12, x.r[12]);
    o.st32(sp - 16, x.r[11]);
    o.st32(sp - 20, x.r[10]);
    o.st32(sp - 24, x.r[9]);
    o.st32(sp - 28, x.r[8]);
    o.st32(sp - 32, f2u(x.f[15]));
    o.st32(sp - 36, f2u(x.f[14]));
    o.st32(sp - 40, f2u(x.f[13]));
    o.st32(sp - 44, f2u(x.f[12]));
    const std::uint32_t frame = sp - 72;  // seven words of state below the saves
    // L_8c058eba: the u and v offsets.
    r1 = 0x8C10EF04u;
    f3 = u2f(o.ld32(0x8C058EF4u));
    f2 = u2f(o.ld32(0x8C10EF04u));
    f2 += f3;
    const float s8 = f2;  // the u offset
    f12 = u2f(o.ld32(0x8C10EF08u));
    cyc += cost(11);
    f12 += f3;  // the v offset
    // L_8c058f00
    r14 = ((r6 >> 14) & 3u);
    r11 = 0;
    r12 = 0;
    f3 = 1.0f;
    r6 &= 0x3FFFu;
    cyc += cost(18);
    float s24 = 1.0f;  // the winding's sign
    if (flags & 0x80000000u) {
        f3 = neg(1.0f);
        s24 = f3;
        cyc += cost(4);
    }
    r14 <<= 1;
    const std::uint32_t s20 = r14;  // the bytes after each index
    cyc += cost(2);
    std::uint32_t s0 = 0, s4 = 0;  // the open runs' lengths: front-facing, back-facing
    float s12 = 0.0f, s16 = 0.0f;  // the third vertex's v and u
    const std::uint8_t* wb = s.wb;
    const std::uint8_t* p = s.data;
    auto rec = [&](std::uint32_t idx) { return wb + static_cast<std::ptrdiff_t>(static_cast<std::int32_t>(idx)) * 32; };
    const float half = u2f(o.ld32(0x8C059074u));
    auto put = [&](std::uint32_t cw, float a, float b, float z, float u, float v) {
        put8(o, r7, cw, f2u(a), f2u(b), f2u(z), f2u(u), f2u(v), f2u(f6), f2u(f5));
        r7 += 32;
    };
    const std::uint8_t *q9 = nullptr, *q10, *q13;
    do {
        // L_8c058f30: a strip.
        r8 = rd16s(p);
        r13 = rd16s(p + 2);
        p += 4;
        q13 = rec(r13);
        r13 = (r13 << 5) + r5;
        cyc += cost(11);
        f13 = s24;
        if (static_cast<std::int32_t>(r8) < 0) {
            f13 = neg(f13);
            r8 = 0u - r8;
            cyc += cost(2);
        }
        // L_8c058f4a
        f11 = f12;
        r2 = 0;
        r12 += r8;
        r3 = 0;
        s0 = 0;
        r8 -= 2;
        s4 = 0;
        f10 = s8;
        f0 = half;
        r10 = rd16s(p);
        p += 2;
        q10 = rec(r10);
        r10 = (r10 << 5) + r5;
        f10 = fmac(f0, rdf(q13 + 24), f10);
        f11 = fmac(f0, rdf(q13 + 28), f11);
        f14 = f12;
        f15 = s8;
        f15 = fmac(f0, rdf(q10 + 24), f15);
        f3 = rdf(q10 + 28);
        f14 = fmac(f0, f3, f14);
        cyc += cost(32);
        for (;;) {
            // L_8c058f8a: a triangle.
            r9 = rd16s(p);
            p += 2;
            q9 = rec(r9);
            r9 = (r9 << 5) + r5;
            f0 = half;
            f2 = rdf(q9 + 24);
            f3 = fmac(f0, f2, s8);
            s16 = f3;
            f2 = rdf(q9 + 28);
            f3 = fmac(f0, f2, f12);
            s12 = f3;
            f3 = rdf(q13 + 8);
            f6 = rdf(q10 + 8);
            f2 = rdf(q9 + 4);
            f5 = rdf(q10 + 4);
            f3 -= f6;
            f2 -= f5;
            f3 *= f2;
            f2 = rdf(q13 + 4);
            f1 = rdf(q9 + 8);
            f2 -= f5;
            f1 -= f6;
            f2 *= f1;
            f3 -= f2;
            f5 = f3;
            f5 *= f13;
            f3 = 0.0f;
            const bool front = f3 > f5;
            cyc += cost(42);
            if (front) {
                cyc += cost(3);
                if (s4 != 0) {  // L_8c058fe4: the back-facing run ends with the second vertex
                    f8 = rdf(q10 + 4);
                    f6 = f4;
                    f7 = rdf(q10 + 8);
                    f9 = rdf(q10 + 12);
                    f2 = rdf(q10 + 16);
                    f6 -= f2;
                    f5 = rdf(q10 + 20);
                    const bool t = f4 > f6;
                    cyc += cost(12);
                    f5 = neg(f5);
                    if (t) {
                        f6 = f4;
                        cyc += cost(1);
                    }
                    r3 = kLast;
                    put(r3, f8, f7, f9, f15, f14);
                    r2 = s4;
                    r3 = 0;
                    s4 = 0;
                    r2 += 1;
                    r11 += r2;
                    cyc += cost(17);
                } else {
                    r2 = s4;
                    cyc += 1;
                }
                r2 = s0;
                cyc += cost(3);
                if (s0 == 0) {  // L_8c059026: a front-facing run starts with the first vertex
                    f8 = rdf(q13 + 4);
                    f7 = rdf(q13 + 8);
                    f9 = rdf(q13 + 12);
                    f6 = rdf(q13 + 16);
                    f6 += f4;
                    const bool t = f4 > f6;
                    cyc += cost(10);
                    f5 = rdf(q13 + 20);
                    if (t) {
                        f6 = f4;
                        cyc += cost(1);
                    }
                    r3 = kVertex;
                    put(r3, f8, f7, f9, f10, f11);
                    r2 = s0 + 1;
                    s0 = r2;
                    cyc += cost(15);
                } else {
                    cyc += 1;
                }
                // L_8c05905a: then the second.
                f7 = rdf(q10 + 4);
                f8 = rdf(q10 + 8);
                f9 = rdf(q10 + 12);
                f6 = rdf(q10 + 16);
                f6 += f4;
                const bool t = f4 > f6;
                cyc += cost(10);
                f5 = rdf(q10 + 20);
                cyc += cost(2);
                if (t) {
                    f6 = f4;
                    cyc += cost(1);
                }
                r3 = kVertex;
                put(r3, f7, f8, f9, f15, f14);
                r2 = s0 + 1;
                cyc += cost(16);
                s0 = r2;
            } else {
                cyc += 1;
                r1 = s0;
                cyc += cost(3);
                if (s0 != 0) {  // L_8c0590ac: the front-facing run ends
                    f7 = rdf(q10 + 4);
                    f8 = rdf(q10 + 8);
                    f9 = rdf(q10 + 12);
                    f6 = rdf(q10 + 16);
                    f6 += f4;
                    const bool t = f4 > f6;
                    cyc += cost(10);
                    f5 = rdf(q10 + 20);
                    if (t) {
                        f6 = f4;
                        cyc += cost(1);
                    }
                    r3 = kLast;
                    put(r3, f7, f8, f9, f15, f14);
                    r2 = s0;
                    r3 = 0;
                    s0 = 0;
                    r2 += 1;
                    r11 += r2;
                    cyc += cost(17);
                } else {
                    cyc += 1;
                }
                r2 = s4;
                cyc += cost(3);
                if (s4 == 0) {  // L_8c0590ea: a back-facing run starts
                    f9 = rdf(q13 + 4);
                    f6 = f4;
                    f8 = rdf(q13 + 8);
                    f7 = rdf(q13 + 12);
                    f3 = rdf(q13 + 16);
                    f6 -= f3;
                    f5 = rdf(q13 + 20);
                    const bool t = f4 > f6;
                    cyc += cost(12);
                    f5 = neg(f5);
                    if (t) {
                        f6 = f4;
                        cyc += cost(1);
                    }
                    r3 = kVertex;
                    put(r3, f9, f8, f7, f10, f11);
                    r2 = s4 + 1;
                    s4 = r2;
                    cyc += cost(15);
                } else {
                    cyc += 1;
                }
                // L_8c059122
                f8 = rdf(q10 + 4);
                f6 = f4;
                f7 = rdf(q10 + 8);
                f9 = rdf(q10 + 12);
                f3 = rdf(q10 + 16);
                f6 -= f3;
                f5 = rdf(q10 + 20);
                const bool t = f4 > f6;
                cyc += cost(12);
                f5 = neg(f5);
                if (t) {
                    f6 = f4;
                    cyc += cost(1);
                }
                r3 = kVertex;
                put(r3, f8, f7, f9, f15, f14);
                r2 = s4 + 1;
                s4 = r2;
                cyc += cost(15);
            }
            // L_8c05915a: the next triangle shares this one's last two vertices.
            f10 = f15;
            r3 = s20;
            r13 = r10;
            q13 = q10;
            f15 = s16;
            r8 -= 1;
            r0 = 12;
            r10 = r9;
            q10 = q9;
            f13 = neg(f13);
            p += r3;
            f11 = f14;
            cyc += cost(13);
            f14 = s12;
            if (r8 == 0)
                break;
            cyc += cost(2);
        }
        // L_8c059178: the open run ends with the strip's last vertex.
        r3 = s0;
        cyc += cost(3);
        bool close = true;
        if (s0 != 0) {
            cyc += 1;
        } else {
            r3 = s4;
            cyc += cost(3);
            if (s4 == 0) {
                cyc += 1;
                close = false;
            }
        }
        if (close) {
            f8 = rdf(q9 + 4);
            r3 = s0;
            f7 = rdf(q9 + 8);
            cyc += cost(8);
            f9 = rdf(q9 + 12);
            if (s0 != 0) {
                f6 = rdf(q9 + 16);
                r3 = s0 + 1;
                f6 += f4;
                r11 += r3;
                cyc += cost(7);
                f5 = rdf(q9 + 20);
            } else {
                f3 = rdf(q9 + 16);
                f6 = f4;
                r3 = s4;
                f6 -= f3;
                f5 = rdf(q9 + 20);
                r3 += 1;
                r11 += r3;
                f5 = neg(f5);
                cyc += cost(8);
            }
            const bool t = f4 > f6;
            cyc += cost(3);
            if (t) {
                f6 = f4;
                cyc += cost(1);
            }
            r0 = 16;
            f3 = s16;
            r3 = kLast;
            put(r3, f8, f7, f9, s16, s12);
            cyc += cost(15);
        }
        // L_8c0591d8
        r6 -= 1;
        cyc += cost(2);
        if (r6 == 0)
            break;
        cyc += cost(2);
    } while (true);
    cyc += 1;
    // L_8c0591e0: the totals, the frame's last state, and the restores.
    r3 = o.ld32(kWritten) + r11;
    o.st32(kWritten, r3);
    r2 = o.ld32(kRead) + r12;
    o.st32(kRead, r2);
    o.st32(frame + 0, s0);
    o.st32(frame + 4, s4);
    o.st32(frame + 8, f2u(s8));
    o.st32(frame + 12, f2u(s12));
    o.st32(frame + 16, f2u(s16));
    o.st32(frame + 20, s20);
    o.st32(frame + 24, f2u(s24));
    cyc += cost(22);
    (void)r0;
    (void)r14;
    (void)q9;
    x.r[0] = r7;
    x.r[1] = r1;
    x.r[2] = r2;
    x.r[3] = r3;
    x.r[4] = kRead;
    x.r[6] = r6;
    x.r[7] = r7;
    const float fr[12] = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11};
    std::memcpy(x.f, fr, sizeof fr);
    x.t = 1;
    o.cycles += cyc;
    (void)r4;
    return true;
}



// Strip writer C (0x8C059930): one-sided strips without texture. Front-facing runs of triangles
// only (the back-facing ones are dropped), lit by the normal; a vertex parameter is the control
// word, x, y, 1/w, the base and offset intensity in its last two words, and its words 4 and 5 are
// not stored (the queue's earlier contents go with the flush, as in the original). The triangle's
// facing is the sign of (x1 - x0)(y2 - y1) - (y1 - y0)(x2 - x1), the winding as in A.
bool strip_c(std::uint32_t pr, Regs& x, Out& o, Memory& m) {
    Strips s;
    if (!scan_strips(m, x.r[4], x.r[5], x.r[6], s))
        return false;
    const std::uint32_t sp = x.r[15];
    o.st32(sp - 4, x.r[8]);
    o.st32(sp - 8, x.r[9]);
    o.st32(sp - 12, x.r[10]);
    o.st32(sp - 16, x.r[11]);
    o.st32(sp - 20, x.r[12]);
    o.st32(sp - 24, x.r[13]);
    o.st32(sp - 28, x.r[14]);
    o.st32(sp - 32, f2u(x.f[12]));
    o.st32(sp - 36, f2u(x.f[13]));
    o.st32(sp - 40, f2u(x.f[14]));
    o.st32(sp - 44, f2u(x.f[15]));
    o.st32(sp - 48, pr);
    const std::uint32_t flags = o.ld32(kFlags);
    o.st32(sp - 52, flags);
    std::uint32_t r1 = x.r[6] & 0x3FFFu, r2 = 0, r3 = 0, r7 = x.r[7], r8, r12;
    const std::uint32_t skip = ((x.r[6] >> 14) & 3u) << 1;
    float f0 = x.f[0], f1 = x.f[1], f2 = x.f[2], f3 = x.f[3], f5 = x.f[5], f6 = x.f[6], f7 = x.f[7],
          f8 = x.f[8], f9 = x.f[9], f10 = x.f[10], f11 = x.f[11];
    const float f4 = x.f[4];
    std::uint64_t cyc = cost(32);
    const std::uint8_t* p = s.data;
    auto rec = [&](std::uint32_t idx) { return s.wb + static_cast<std::ptrdiff_t>(static_cast<std::int32_t>(idx)) * 32; };
    // A vertex from its record: its words 0-3 and 4-5 into fr0..fr3, fr6, fr7; the base clamped.
    auto vertex = [&](const std::uint8_t* q, std::uint32_t cw) {
        f0 = rdf(q);
        f1 = rdf(q + 4);
        f2 = rdf(q + 8);
        f3 = rdf(q + 12);
        f6 = rdf(q + 16);
        f7 = rdf(q + 20);
        f6 += f4;
        cyc += cost(13);
        if (f4 > f6) {
            f6 = f4;
            cyc += cost(1);
        } else {
            cyc += 1;
        }
        const std::uint32_t w[8] = {cw, f2u(f1), f2u(f2), f2u(f3), 0, 0, f2u(f6), f2u(f7)};
        o.partial(r7, w, 0xCFu);
        r7 += 32;
    };
    const std::uint8_t *q9, *q10, *q11 = nullptr;
    do {
        // L_8c059970: a strip.
        r8 = rd16s(p);
        q9 = rec(rd16s(p + 2));
        p += 4;
        f5 = 1.0f;
        cyc += cost(8);
        if (flags >> 31) {
            f5 = neg(f5);
            cyc += cost(1);
        } else {
            cyc += 1;
        }
        cyc += cost(4);
        if (static_cast<std::int32_t>(r8) >= 0) {
            cyc += 1;
        } else {
            r8 = 0u - r8;
            f5 = neg(f5);
            cyc += cost(2);
        }
        q10 = rec(rd16s(p));
        p += 2;
        r3 += r8;
        r8 -= 2;
        r12 = 0;
        cyc += cost(8);
        for (;;) {
            // L_8c05999e: a triangle.
            q11 = rec(rd16s(p));
            p += 2;
            f0 = rdf(q9);
            f1 = rdf(q9 + 4);
            f2 = rdf(q9 + 8);
            f3 = rdf(q9 + 12);
            f6 = rdf(q10);
            f7 = rdf(q10 + 4);
            f8 = rdf(q10 + 8);
            f9 = rdf(q10 + 12);
            f10 = rdf(q11);
            f11 = rdf(q11 + 4);
            f11 -= f7;
            float f12 = rdf(q11 + 8);
            f7 -= f1;
            f12 -= f8;
            f8 -= f2;
            f7 *= f12;
            f8 *= f11;
            f6 = 0.0f;
            f7 -= f8;
            f7 *= f5;
            const bool front = f6 > f7;
            f5 = neg(f5);
            cyc += cost(26);
            if (front) {
                cyc += cost(1);
                if (r12 == 0) {
                    r12 += 1;
                    vertex(q9, kVertex);
                    cyc += cost(6);
                } else {
                    cyc += 1;
                }
                r12 += 1;
                vertex(q10, kVertex);
                cyc += cost(7);
            } else {
                cyc += cost(1);
                if (r12 != 0) {
                    r12 += 1;
                    r2 += r12;
                    vertex(q10, kLast);
                    r12 = 0;
                    cyc += cost(8);
                } else {
                    cyc += 1;
                }
            }
            // L_8c059a54
            r8 -= 1;
            q9 = q10;
            p += skip;
            cyc += cost(5);
            if (r8 == 0)
                break;
            q10 = q11;
        }
        cyc += cost(2);
        if (r12 != 0) {
            r2 += r12;
            r2 += 1;
            vertex(q11, kLast);
            cyc += cost(7);
        } else {
            cyc += 1;
        }
        r1 -= 1;
        cyc += cost(2);
        if (r1 == 0)
            break;
        cyc += cost(2);
    } while (true);
    cyc += 1;
    const std::uint32_t written = o.ld32(kWritten) + r2, read = o.ld32(kRead) + r3;
    o.st32(kWritten, written);
    o.st32(kRead, read);
    cyc += cost(24);
    x.r[0] = r7;
    x.r[1] = 0;
    x.r[2] = r2;
    x.r[3] = r3;
    x.r[4] = kWritten;
    x.r[5] = kRead;
    x.r[6] = written;
    x.r[7] = read;
    const float fr[12] = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11};
    std::memcpy(x.f, fr, sizeof fr);
    x.t = 1;
    o.cycles += cyc;
    return true;
}


// Strip writer D (0x8C0594E0, and 0x8C059744 which it continues into): A's two forms one-sided.
// Only front-facing runs are written, lit by the normal (base = fr4 + dot, the specular term as it
// is); the environment-mapped form takes u and v from the normal as A's does, the plain one too
// with a fixed offset. Called only by the native walker, so what it leaves beyond memory is its
// result (r0), the totals' registers and the fr4/fr5 the walker carries on.
bool strip_d(std::uint32_t pr, Regs& x, Out& o, Memory& m) {
    Strips s;
    if (!scan_strips(m, x.r[4], x.r[5], x.r[6], s))
        return false;
    const std::uint32_t sp = x.r[15];
    const std::uint32_t flags = o.ld32(kFlags);
    const float f4 = x.f[4];
    std::uint64_t cyc = cost(16);
    std::uint32_t r7 = x.r[7], emitted = 0, read = 0;
    float f5 = x.f[5];
    const std::uint8_t* p = s.data;
    auto rec = [&](std::uint32_t idx) { return s.wb + static_cast<std::ptrdiff_t>(static_cast<std::int32_t>(idx)) * 32; };
    std::uint32_t count = x.r[6] & 0x3FFFu;
    if (flags & 0x00020000u) {
        // 0x8C0594E0's own form: the u and v offsets from 0x8C10EF04/08, the winding from bit 31.
        o.st32(sp - 4, x.r[14]);
        o.st32(sp - 8, x.r[13]);
        o.st32(sp - 12, x.r[11]);
        o.st32(sp - 16, x.r[10]);
        o.st32(sp - 20, x.r[9]);
        o.st32(sp - 24, x.r[8]);
        o.st32(sp - 28, f2u(x.f[15]));
        o.st32(sp - 32, f2u(x.f[14]));
        o.st32(sp - 36, f2u(x.f[13]));
        o.st32(sp - 40, f2u(x.f[12]));
        const float off = o.ldf(0x8C059534u);
        const float cu = o.ldf(0x8C10EF04u) + off, cv = o.ldf(0x8C10EF08u) + off;
        cyc += cost(9) + cost(16);
        float sign = 1.0f;
        if (flags & 0x80000000u) {
            sign = neg(1.0f);
            cyc += cost(2);
        }
        o.st32(sp - 44, s.skip);
        cyc += cost(2);
        const float half = o.ldf(0x8C059654u), half2 = o.ldf(0x8C059734u);
        // A vertex: x, y, 1/w, the base, the specular term, u and v from the normal.
        auto vertex = [&](const std::uint8_t* q, std::uint32_t cw, float h) {
            float base = rdf(q + 16) + f4;
            const bool t = f4 > base;
            if (t)
                base = f4;
            const float v = fmac(h, rdf(q + 28), cv), u = fmac(h, rdf(q + 24), cu);
            f5 = v;
            put8(o, r7, cw, rd32(q + 4), rd32(q + 8), rd32(q + 12), f2u(u), f2u(v), f2u(base), rd32(q + 20));
            r7 += 32;
            return t;
        };
        do {
            std::uint32_t len = rd16s(p);
            const std::uint8_t* q0 = rec(rd16s(p + 2));
            p += 4;
            float f15 = sign;
            cyc += cost(10);
            if (static_cast<std::int32_t>(len) < 0) {
                f15 = neg(f15);
                len = 0u - len;
                cyc += cost(2);
            }
            const std::uint8_t* q1 = rec(rd16s(p));
            p += 2;
            read += len;
            std::uint32_t run = 0, tri = len - 2;
            cyc += cost(9);
            const std::uint8_t* q2 = nullptr;
            for (;;) {
                q2 = rec(rd16s(p));
                p += 2;
                float f3 = rdf(q0 + 8);
                const float y1 = rdf(q1 + 8);
                float f2 = rdf(q2 + 4);
                const float x1 = rdf(q1 + 4);
                f3 -= y1;
                f2 -= x1;
                f3 *= f2;
                f2 = rdf(q0 + 4);
                float f1 = rdf(q2 + 8);
                f2 -= x1;
                f1 -= y1;
                f2 *= f1;
                f3 -= f2;
                f5 = f3 * f15;
                const bool front = 0.0f > f5;
                cyc += cost(27);
                if (front) {
                    cyc += cost(2);
                    if (run == 0) {
                        cyc += cost(9) + (vertex(q0, kVertex, half) ? cost(1) : 0) + cost(21);
                        run += 1;
                    } else {
                        cyc += 1;
                    }
                    cyc += cost(10) + (vertex(q1, kVertex, half) ? cost(1) : 0) + cost(22);
                    run += 1;
                } else {
                    cyc += 1 + cost(2);
                    if (run != 0) {
                        cyc += cost(10) + (vertex(q1, kLast, half2) ? cost(1) : 0) + cost(23);
                        run += 1;
                        emitted += run;
                        run = 0;
                    } else {
                        cyc += 1;
                    }
                }
                p += s.skip;
                f15 = neg(f15);
                cyc += cost(10);
                if (--tri == 0)
                    break;
                cyc += cost(2);
                q0 = q1;
                q1 = q2;
            }
            cyc += cost(2);
            if (run != 0) {
                cyc += cost(10) + (vertex(q2, kLast, half2) ? cost(1) : 0) + cost(22);
                run += 1;
                emitted += run;
            } else {
                cyc += 1;
            }
            cyc += cost(2);
            if (--count == 0)
                break;
            cyc += cost(2);
        } while (true);
        cyc += 1;
        o.st32(kWritten, o.ld32(kWritten) + emitted);
        o.st32(kRead, o.ld32(kRead) + read);
        cyc += cost(21);
        x.r[0] = r7;
        x.r[2] = o.ld32(kRead);
        x.r[3] = o.ld32(kWritten);
        x.r[4] = kRead;
        x.r[6] = 0;
        x.r[7] = r7;
        x.f[5] = f5;
        o.cycles += cyc;
        return true;
    }
    // 0x8C059744: restored at once, then the u and v from the normal with the offset at 0x8C059924.
    cyc += 1 + cost(12);
    o.st32(sp - 4, x.r[8]);
    o.st32(sp - 8, x.r[9]);
    o.st32(sp - 12, x.r[10]);
    o.st32(sp - 16, x.r[11]);
    o.st32(sp - 20, x.r[12]);
    o.st32(sp - 24, x.r[13]);
    o.st32(sp - 28, x.r[14]);
    o.st32(sp - 32, f2u(x.f[12]));
    o.st32(sp - 36, f2u(x.f[13]));
    o.st32(sp - 40, f2u(x.f[14]));
    o.st32(sp - 44, f2u(x.f[15]));
    o.st32(sp - 48, pr);
    o.st32(sp - 52, flags);
    f5 = o.ldf(0x8C059924u);
    cyc += cost(35);
    auto vertex = [&](const std::uint8_t* q, std::uint32_t cw, float u, float v) {
        float base = rdf(q + 16) + f4;
        const bool t = f4 > base;
        if (t)
            base = f4;
        put8(o, r7, cw, rd32(q + 4), rd32(q + 8), rd32(q + 12), f2u(u), f2u(v), f2u(base), rd32(q + 20));
        r7 += 32;
        cyc += cost(13) + (t ? cost(1) : 1);
    };
    do {
        std::uint32_t len = rd16s(p);
        const std::uint8_t* q9 = rec(rd16s(p + 2));
        p += 4;
        float f9 = 1.0f;
        cyc += cost(8);
        if (flags >> 31) {
            f9 = neg(f9);
            cyc += cost(1);
        } else {
            cyc += 1;
        }
        cyc += cost(4);
        if (static_cast<std::int32_t>(len) >= 0) {
            cyc += 1;
        } else {
            len = 0u - len;
            f9 = neg(f9);
            cyc += cost(2);
        }
        read += len;
        std::uint32_t tri = len - 2, run = 0;
        const std::uint8_t* q10 = rec(rd16s(p));
        p += 2;
        float f10 = rdf(q9 + 24) * f5, f11 = rdf(q9 + 28) * f5;
        f10 += f5;
        f11 += f5;
        float f12 = rdf(q10 + 24) * f5, f13 = rdf(q10 + 28) * f5;
        f12 += f5;
        f13 += f5;
        cyc += cost(19);
        const std::uint8_t* q11 = nullptr;
        float f14 = 0.0f, f15 = 0.0f;
        for (;;) {
            q11 = rec(rd16s(p));
            p += 2;
            float f0 = rdf(q9 + 4), f6 = rdf(q9 + 8);
            const float f1 = rdf(q10 + 4), f7 = rdf(q10 + 8);
            f0 -= f1;
            f6 -= f7;
            float f2 = rdf(q11 + 4);
            f2 -= f1;
            float f8 = rdf(q11 + 8);
            f6 *= f2;
            f8 -= f7;
            f14 = rdf(q11 + 24);
            f15 = rdf(q11 + 28);
            f8 *= f0;
            f6 -= f8;
            f6 *= f9;
            const bool front = 0.0f > f6;
            f9 = neg(f9);
            cyc += cost(34);
            if (front) {
                cyc += cost(1);
                if (run == 0) {
                    run += 1;
                    vertex(q9, kVertex, f10, f11);
                    cyc += cost(6);
                } else {
                    cyc += 1;
                }
                run += 1;
                vertex(q10, kVertex, f12, f13);
                cyc += cost(7);
            } else {
                cyc += cost(1);
                if (run != 0) {
                    run += 1;
                    vertex(q10, kLast, f12, f13);
                    emitted += run;
                    run = 0;
                    cyc += cost(8);
                } else {
                    cyc += 1;
                }
            }
            f14 *= f5;
            f15 *= f5;
            f14 += f5;
            f15 += f5;
            p += s.skip;
            q9 = q10;
            q10 = q11;
            f10 = f12;
            f11 = f13;
            f12 = f14;
            f13 = f15;
            cyc += cost(11);
            if (--tri == 0)
                break;
        }
        cyc += cost(2);
        if (run != 0) {
            emitted += run;
            vertex(q11, kLast, f14, f15);
            emitted += 1;
            cyc += cost(7);
        } else {
            cyc += 1;
        }
        cyc += cost(2);
        if (--count == 0)
            break;
        cyc += cost(2);
    } while (true);
    cyc += 1;
    const std::uint32_t w2 = o.ld32(kWritten) + emitted, w3 = o.ld32(kRead) + read;
    o.st32(kWritten, w2);
    o.st32(kRead, w3);
    cyc += cost(24);
    x.r[0] = r7;
    x.r[1] = 0;
    x.r[2] = emitted;
    x.r[3] = read;
    x.r[4] = kWritten;
    x.r[5] = kRead;
    x.r[6] = w2;
    x.r[7] = w3;
    x.f[5] = f5;
    o.cycles += cyc;
    return true;
}

// ---- Kamui's strip context, as Ninja's chunk helpers change it -------------------------------
// Kamui keeps the polygon header it is building at the address in 0x8C12104C (the parameter
// control word at +0, ISP/TSP at +4, TSP at +8, the texture control word at +12, the last mode
// word at +24, the texture list at +40), and its list pointers at the address in 0x8C121050
// (+0 one list, +8 the other); 0x8C1212BC says which list pointer the next strip's end updates.
constexpr std::uint32_t kStripCtx = 0x8C12104Cu, kLists = 0x8C121050u, kListAt = 0x8C1212BCu;
constexpr std::uint32_t kDiffuse = 0x8C10EF78u, kSpecular = 0x8C10EF98u, kLight = 0x8C115F40u;

// 0x8C01754A: the list pointer after a strip, into the slot 0x8C1212BC names.
void nj_end_strip(Out& o, std::uint32_t p) {
    o.st32(o.ld32(kListAt), p);
    o.cycles += cost(4);
}

// 0x8C017594: a strip mode word (from Ninja's tables) into the header: the list-type bits of the
// control word, the TSP's bit 20, and the word itself at +24. Bit 7 of the control word (Gouraud)
// follows Ninja's flags.
void nj_strip_mode(Out& o, std::uint32_t r4) {
    const std::uint32_t ctx = o.ld32(kStripCtx);
    std::uint32_t w0 = (o.ld32(ctx) & 0xFDFFFF00u) | (r4 & 0x020000FFu);
    const std::uint32_t w8 = (o.ld32(ctx + 8) & 0xFFEFFFFFu) | (r4 & 0x00100000u);
    const std::uint32_t flags = o.ld32(kFlags);
    std::uint64_t cyc = cost(21);
    bool gouraud;
    if (flags & 1024u) {
        cyc += 1;
        gouraud = true;
    } else {
        cyc += cost(3);
        if (!(flags & 4096u)) {
            cyc += 1;
            gouraud = false;
        } else {
            cyc += cost(3);
            gouraud = !(r4 & 0x02100000u);
            if (!gouraud)
                cyc += 1;
        }
    }
    if (gouraud) {
        w0 |= 128u;
        cyc += cost(3);
    } else {
        w0 &= ~128u;
        cyc += cost(2);
    }
    o.st32(ctx, w0);
    o.st32(ctx + 8, w8);
    o.st32(ctx + 24, r4);
    o.cycles += cyc + cost(8);
}

// 0x8C01771C: the blend from a chunk header's flags (bits 8-13 the source and destination
// factors, 14-15 more) into the TSP's top byte, unless Ninja's flags force one (0x8C120F24).
void nj_blend(Out& o, std::uint32_t r4) {
    const std::uint32_t ctx = o.ld32(kStripCtx);
    const std::uint32_t flags = o.ld32(kFlags);
    std::uint32_t w8 = o.ld32(ctx + 8) & 0x00FFFFFFu;
    std::uint64_t cyc = cost(10);
    if (flags & 0x8000u) {
        r4 = o.ld32(0x8C120F24u);
        cyc += cost(2);
    } else {
        cyc += 1;
    }
    w8 |= ((r4 << 18) & 0xFC000000u) | ((r4 << 10) & 0x03000000u);
    o.st32(ctx + 8, w8);
    o.cycles += cyc + cost(14);
}

// 0x8C0176B4: a texture chunk (r4 its header, r5 the texture word: id in bits 0-12, flip and clamp
// bits above) into the header's TSP and texture control word. Returns the texture's flags (16 bits
// from its descriptor), whose low byte 4 means a UV scale follows.
std::uint32_t nj_texture(Out& o, std::uint32_t r4, std::uint32_t r5) {
    const std::uint32_t ctx = o.ld32(kStripCtx);
    const std::uint32_t tex = o.ld32(o.ld32(o.ld32(ctx + 40)) + (r5 & 8191u) * 12 + 8);
    std::uint32_t w8 = (o.ld32(ctx + 8) & ~63u) | (o.ld32(tex) & 63u);
    o.st32(ctx + 8, w8);
    o.st32(ctx + 12, o.ld32(tex + 4));
    w8 = (w8 & 0xFFF800FFu) | ((r4 & 0xF000u) << 3) | (r4 & 0x0F00u) | ((r5 & 0x2000u) >> 1) |
         ((r5 & 0xC000u) >> 1);
    o.st32(ctx + 8, w8);
    o.cycles += cost(52);
    return o.ld32(o.ld32(tex + 12) + 4) >> 16;
}

// 0x8C041BB0, then 0x8C01771C: a material chunk (r4 its header: bit 0, 1, 2 of type - 16 say a
// diffuse, ambient, specular colour follows, ARGB bytes) into float colours at `diffuse`,
// `ambient`, `specular` (null: skipped), each byte times 1/255 (0x8C041D30). Ninja's flags may
// replace (bit 4) or offset (bit 5) the diffuse colour with 0x8C120F14. fr4 is left as the scale.
void nj_material(Out& o, std::uint32_t r4, std::uint32_t p, std::uint32_t diffuse,
                 std::uint32_t ambient, std::uint32_t specular, float& f4) {
    const std::uint32_t kinds = (r4 & 0xFFu) - 16;
    f4 = o.ldf(0x8C041D30u);
    const float scale = f4;
    auto hi = [](std::uint32_t v) { return static_cast<float>(static_cast<std::int32_t>((v >> 8) & 0xFFu)); };
    auto lo = [](std::uint32_t v) { return static_cast<float>(static_cast<std::int32_t>(v & 0xFFu)); };
    auto put4 = [&](std::uint32_t at, float a, float b, float c, float d) {
        o.st32(at, f2u(a));
        o.st32(at + 4, f2u(b));
        o.st32(at + 8, f2u(c));
        o.st32(at + 12, f2u(d));
    };
    std::uint64_t cyc = cost(14);
    if (kinds & 1u) {
        const std::uint32_t gb = o.ld16s(p), ar = o.ld16s(p + 2);
        p += 4;
        const std::uint32_t flags = o.ld32(kFlags);
        cyc += cost(7);
        if (flags & 16u) {
            for (unsigned k = 0; k < 4; ++k) o.st32(diffuse + 4 * k, o.ld32(0x8C120F14u + 4 * k));
            cyc += cost(12) + cost(2);
        } else {
            cyc += cost(2);
            if (diffuse == 0) {
                cyc += 1;
            } else if (cyc += cost(3), flags & 32u) {
                put4(diffuse, fmac(scale, hi(ar), o.ldf(0x8C120F14u)), fmac(scale, lo(ar), o.ldf(0x8C120F18u)),
                     fmac(scale, hi(gb), o.ldf(0x8C120F1Cu)), fmac(scale, lo(gb), o.ldf(0x8C120F20u)));
                cyc += cost(37) + cost(2);
            } else {
                put4(diffuse, hi(ar) * scale, lo(ar) * scale, hi(gb) * scale, lo(gb) * scale);
                cyc += 1 + cost(32);
            }
        }
    }
    cyc += cost(3);
    if (kinds & 2u) {
        const std::uint32_t gb = o.ld16s(p), ar = o.ld16s(p + 2);
        p += 4;
        cyc += cost(4);
        if (ambient != 0) {
            put4(ambient, hi(ar) * scale, lo(ar) * scale, hi(gb) * scale, lo(gb) * scale);
            cyc += cost(32);
        }
    } else {
        cyc += 1;
    }
    cyc += cost(3);
    if (kinds & 4u) {
        const std::uint32_t gb = o.ld16s(p), ar = o.ld16s(p + 2);
        cyc += cost(4);
        if (specular != 0) {
            put4(specular, hi(ar) * scale, lo(ar) * scale, hi(gb) * scale, lo(gb) * scale);
            cyc += cost(29);
        }
    } else {
        cyc += 1;
    }
    o.cycles += cyc + cost(8);
    nj_blend(o, r4);
}

// Kamui's polygon headers for a strip (0x8C016FE4-0x8C01737C): eight words at the list pointer,
// from the strip context and, in most, the face colour `col` (and the specular one `spec`). The
// `second` list is the one at +8. Some store fewer words (their flush sends what the queue held
// for the rest); the long ones send a second block of colours. Return the list pointer after.
std::uint32_t nj_head(Out& o, bool second, bool fixed, unsigned words, bool colour, bool notex,
                      bool two, const float* col, const float* spec, unsigned cycles) {
    const std::uint32_t list = o.ld32(kLists) + (second ? 8u : 0u);
    const std::uint32_t p = o.ld32(list);
    const std::uint32_t ctx = o.ld32(kStripCtx);
    std::uint32_t w[8] = {};
    w[0] = o.ld32(ctx);
    w[1] = o.ld32(ctx + 4);
    w[2] = fixed ? ((o.ld32(ctx + 8) & 0x03EFFFFFu) | 0x20000000u) : o.ld32(ctx + 8);
    std::uint32_t mask = 7u;
    if (words >= 4 && !notex) {
        w[3] = o.ld32(ctx + 12);
        mask |= 8u;
    }
    if (colour) {
        for (unsigned k = 0; k < 4; ++k) w[4 + k] = f2u(col[k]);
        mask |= 0xF0u;
    }
    o.partial(p, w, mask);
    std::uint32_t end = p + 32;
    if (two) {
        std::uint32_t v[8];
        for (unsigned k = 0; k < 4; ++k) {
            v[k] = f2u(col[k]);
            v[4 + k] = f2u(spec[k]);
        }
        o.partial(end, v, 0xFFu);
        end += 32;
    }
    o.st32(kListAt, list);
    o.cycles += cost(cycles);
    return end;
}
std::uint32_t nj_head_at(Out& o, std::uint32_t fn, const float* col, const float* spec) {
    switch (fn) {
        case 0x8C017252u: return nj_head(o, false, true, 4, true, false, false, col, spec, 40);
        case 0x8C0172C4u: return nj_head(o, true, false, 4, true, false, false, col, spec, 37);
        case 0x8C01707Au: return nj_head(o, true, false, 4, false, false, false, col, spec, 25);
        case 0x8C017042u: return nj_head(o, false, true, 4, false, false, false, col, spec, 28);
        case 0x8C017016u: return nj_head(o, true, false, 3, false, false, false, col, spec, 22);
        case 0x8C016FE4u: return nj_head(o, false, true, 3, false, false, false, col, spec, 25);
        case 0x8C01720Cu: return nj_head(o, true, false, 3, true, true, false, col, spec, 35);
        case 0x8C0171C0u: return nj_head(o, false, true, 3, true, true, false, col, spec, 38);
        case 0x8C01737Cu: return nj_head(o, true, false, 4, false, false, true, col, spec, 52);
        case 0x8C01730Eu: return nj_head(o, false, true, 4, false, false, true, col, spec, 55);
        default: return 0;
    }
}

// ---- 0x8C01A6C0: Ninja's polygon chunk walker -------------------------------------------------
// njCnkDrawModel's second half: r4 the polygon chunks, r5 a limit (strip chunks whose own limit is
// not below it are skipped), r6 the vertex work buffer. Per chunk (a 16-bit header, type in the low
// byte, flags in the high one): 1 a blend, 4 and 5 a cached list (keep the pointer, draw it),
// 8-15 a texture (and a UV scale), 16-63 a material, 64-66 strips (strip mode, face colour, Kamui's
// polygon header, then a strip writer from Ninja's tables), 255 the end. Compiled C.
//
// Native strip writers are called directly, the others as guest code (guest_call). `f4`/`f5`
// carry fr4/fr5 between chunks as the original leaves them, since some writers read them.
struct Walk {
    Ctx& c;
    Memory& m;
    Out& o;
    float f4, f5;
};
bool nj_walk(Walk& w, std::uint32_t p, std::uint32_t limit, std::uint32_t wb, std::uint32_t sp);

// A strip writer: natively when it is one written here, else as guest code.
bool nj_writer(Walk& w, std::uint32_t fn, std::uint32_t ret, std::uint32_t sp, std::uint32_t r4,
               std::uint32_t r5, std::uint32_t r6, std::uint32_t r7, float f4, float f5,
               std::uint32_t& r0) {
    if (fn == 0x8C058E98u || fn == 0x8C05C094u || fn == 0x8C059930u || fn == 0x8C0594E0u) {
        Regs x{};
        x.r[4] = r4;
        x.r[5] = r5;
        x.r[6] = r6;
        x.r[7] = r7;
        x.r[15] = sp;
        x.f[4] = f4;
        x.f[5] = f5;
        const bool ok = fn == 0x8C058E98u   ? strip_a(ret, x, w.o, w.m)
                        : fn == 0x8C05C094u ? strip_b(ret, x, w.o, w.m)
                        : fn == 0x8C059930u ? strip_c(ret, x, w.o, w.m)
                                            : strip_d(ret, x, w.o, w.m);
        if (ok) {
            r0 = x.r[0];
            w.f4 = x.f[4];
            w.f5 = x.f[5];
            return true;
        }
        // (It declines before any store, when the strips' data are not in RAM.)
    }
    GuestArgs a;
    a.r4 = r4;
    a.r5 = r5;
    a.r6 = r6;
    a.r7 = r7;
    a.f4 = f4;
    a.f5 = f5;
    if (!guest_call(w.c, w.m, w.o, fn, ret, sp, a, r0))
        return false;
    w.f4 = w.c.fr[4];
    w.f5 = w.c.fr[5];
    return true;
}

bool nj_walk(Walk& w, std::uint32_t p, std::uint32_t limit, std::uint32_t wb, std::uint32_t sp) {
    Out& o = w.o;
    const std::uint32_t frame = sp - 120;  // the original's: 36 bytes of saves, 84 of locals
    float col[4] = {0, 0, 0, 0};           // its face colour at +68
    float f15 = 0.0f;
    o.cycles += cost(98);
    for (;;) {
        // L_8c01ae26: the next chunk.
        const std::uint32_t header = o.ld16s(p);
        p += 2;
        o.cycles += cost(9);
        if (header == 255u) {
            o.cycles += 1 + cost(11);
            return true;
        }
        o.cycles += cost(2);
        // L_8c01a784
        const std::uint32_t type = header & 0xFFu;
        o.cycles += cost(8);
        if (type < 8) {
            const std::uint32_t arg = (static_cast<std::uint32_t>(static_cast<std::int32_t>(header) >> 8)) & 0xFFu;
            o.cycles += cost(9);
            if (type == 1) {
                o.cycles += cost(3);
                nj_blend(o, header);
                o.cycles += cost(2);
                continue;
            }
            o.cycles += cost(2);
            if (type == 2) {
                o.cycles += cost(2);
                continue;
            }
            o.cycles += 1 + cost(2);
            if (type == 3) {
                o.cycles += cost(2);
                continue;
            }
            o.cycles += 1 + cost(2);
            if (type == 4) {
                // L_8c01a7cc: keep the list from here, and stop.
                const std::uint32_t slot = 0x8C151C4Cu + static_cast<std::uint32_t>(static_cast<std::int16_t>(arg)) * 4;
                o.st32(slot, p);
                o.st32(0x8C151C48u, o.ld32(0x8C151C48u) + 1);
                o.cycles += 1 + cost(8) + cost(11);
                return true;
            }
            o.cycles += cost(2);
            if (type == 5) {
                // L_8c01a7dc: draw a kept list.
                o.cycles += 1 + cost(6);
                const std::uint32_t at = o.ld32(0x8C151C4Cu + static_cast<std::uint32_t>(static_cast<std::int16_t>(arg)) * 4);
                if (!nj_walk(w, at, limit, wb, frame))
                    return false;
                o.st32(0x8C151C48u, o.ld32(0x8C151C48u) - 1);
                o.cycles += cost(4);
                continue;
            }
            o.cycles += cost(2);
            continue;
        }
        o.cycles += cost(5);
        if (type < 16) {
            // L_8c01a7fa: a texture.
            const std::uint32_t word = o.ld16s(p);
            p += 2;
            o.cycles += cost(7);
            const std::uint32_t r0 = nj_texture(o, header, word);
            o.st32(0x8C1214FCu, 0);
            o.cycles += cost(6);
            if ((r0 & 0xFFu) != 4) {
                o.cycles += cost(2);
                continue;
            }
            const std::uint32_t next = o.ld16s(p);
            o.cycles += cost(5);
            if (!(next & 24u)) {
                o.cycles += cost(2);
                continue;
            }
            // L_8c01a826: a UV scale, six signed values over 0x8C01A90C.
            o.cycles += 1;
            p += 4;
            w.f4 = o.ldf(0x8C01A90Cu);
            for (unsigned k = 0; k < 6; ++k) {
                const float v = static_cast<float>(static_cast<std::int32_t>(o.ld16s(p))) / w.f4;
                p += 2;
                o.st32(0x8C1213A4u + 4 * k, f2u(v));
            }
            o.st32(0x8C1214FCu, 1);
            o.cycles += cost(69);
            continue;
        }
        o.cycles += cost(4);
        if (type < 64) {
            // L_8c01a8b8: a material.
            const std::uint32_t size = o.ld16s(p);
            p += 2;
            o.cycles += cost(12);
            nj_material(o, header, p, kDiffuse, 0, kSpecular, w.f4);
            o.cycles += cost(2);
            p += size * 2;  // L_8c01ae1a
            o.cycles += cost(6);
            continue;
        }
        // L_8c01a918: strips.
        o.cycles += 1;
        const std::uint32_t size = o.ld16s(p) & 0xFFFFu;
        const std::uint32_t count = o.ld16s(p + 2);
        p += 4;
        o.cycles += cost(16);
        if (static_cast<std::int32_t>(limit) >= static_cast<std::int32_t>(o.ld32(0x8C151C44u))) {
            o.cycles += cost(2) + cost(1);
            p += (size - 1) * 2;
            o.cycles += cost(6);
            continue;
        }
        std::uint32_t r6 = header;
        const std::uint32_t flags = o.ld32(kFlags);
        o.cycles += cost(5);
        if (flags & 2048u) {
            r6 = (r6 & o.ld32(0x8C120F0Cu)) | o.ld32(0x8C120F10u);
            o.cycles += cost(6);
        } else {
            o.cycles += 1;
        }
        // L_8c01a952: the strip flags.
        const std::uint32_t sf = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(r6)) >> 8);
        o.cycles += cost(8);
        if (sf & 4u) {
            w.f4 = 0.0f;
            f15 = 0.0f;
            o.cycles += cost(3);
        } else {
            f15 = o.ldf(0x8C121364u + 16);
            o.cycles += 1 + cost(3);
        }
        w.f4 = 1.0f;
        o.cycles += cost(4);
        if (sf & 1u) {
            f15 = w.f4;
            o.cycles += cost(1);
        }
        o.st32(0x8C10EEE4u, 0);
        o.cycles += cost(8);
        if (sf & 32u) {
            o.st32(0x8C10EEE4u, 1);
            o.cycles += cost(2);
        }
        const std::uint32_t lit = o.ld32(0x8C1214F8u);  // the walker's +52
        const std::uint32_t row = sf << 5;  // Ninja's tables: 32 bytes per flags value
        o.cycles += cost(15);
        const std::uint32_t table = limit != 0 ? 0x8C11229Cu : 0x8C11129Cu;
        // The face colour: the material's diffuse, times Ninja's light colour when asked.
        auto colour = [&](bool plain) {
            col[0] = o.ldf(kDiffuse);
            if (plain) {
                col[1] = o.ldf(kDiffuse + 4);
                col[2] = o.ldf(kDiffuse + 8);
                col[3] = o.ldf(kDiffuse + 12);
            } else {
                col[1] = o.ldf(kDiffuse + 4) * o.ldf(kLight + 4);
                col[2] = o.ldf(kDiffuse + 8) * o.ldf(kLight + 8);
                col[3] = o.ldf(kDiffuse + 12) * o.ldf(kLight + 12);
            }
        };
        float spec[4];
        auto load_spec = [&] {
            for (unsigned k = 0; k < 4; ++k) spec[k] = o.ldf(kSpecular + 4 * k);
        };
        // The original's copy of the face colour at +68 is not stored: every header is native.
        (void)frame;
        std::uint32_t list = 0;  // r7: where the strip's vertices go
        std::uint32_t fn = 0, ret = 0;
        float wf4 = 0.0f, wf5 = 0.0f;
        if (type == 65 || type == 66) {
            o.cycles += type == 65 ? 1 : cost(2) + 1;
            const std::uint32_t scaled = o.ld32(0x8C1214FCu);
            o.cycles += cost(4);
            if (scaled != 0) {
                // L_8c01a9c2: the UV-scaled forms.
                o.cycles += cost(3);
                if (sf & 8u) {
                    o.cycles += cost(3);
                    nj_strip_mode(o, 0x0210000Cu);
                    o.st32(kLight, o.ld32(kDiffuse));
                    o.cycles += cost(5);
                    list = nj_head_at(o, 0x8C01707Au, col, spec);
                    o.cycles += cost(2);
                } else {
                    o.cycles += 1 + cost(3);
                    nj_strip_mode(o, 0x0008000Cu);
                    o.cycles += cost(3);
                    list = nj_head_at(o, 0x8C017042u, col, spec);
                }
                o.cycles += cost(4);
                if (type == 65) {
                    w.f5 = o.ldf(0x8C01AA34u);
                    o.cycles += cost(3);
                } else {
                    w.f5 = o.ldf(0x8C01AB58u);
                    o.cycles += 1 + cost(2);
                }
                o.cycles += cost(3);
                if (limit != 0) {
                    fn = o.ld32(0x8C1132A0u);
                    o.cycles += cost(8);
                    ret = 0x8C01AA52u;
                } else {
                    fn = o.ld32(0x8C11329Cu);
                    o.cycles += 1 + cost(7);
                    ret = 0x8C01AA64u;
                }
                wf4 = f15;
                wf5 = w.f5;
                std::uint32_t r0 = 0;
                if (!nj_writer(w, fn, ret, frame, p, wb, count, list, wf4, wf5, r0))
                    return false;
                if (ret == 0x8C01AA52u)
                    o.cycles += cost(2);
                o.cycles += cost(2);
                list = r0;
                goto strip_end;
            }
            o.cycles += 1 + cost(3);
            if (lit != 0) {
                o.cycles += 1;
                const std::uint32_t f = o.ld32(kFlags);
                o.cycles += cost(4);
                if (f & 512u) {
                    o.cycles += cost(3);
                    if (sf & 1u) {
                        col[0] = o.ldf(kDiffuse);
                        col[1] = col[2] = col[3] = w.f4;
                        o.cycles += cost(12);
                    } else {
                        col[0] = o.ldf(kDiffuse);
                        col[1] = o.ldf(kLight + 4);
                        col[2] = o.ldf(kLight + 8);
                        col[3] = o.ldf(kLight + 12);
                        o.cycles += 1 + cost(18);
                    }
                } else {
                    o.cycles += 1 + cost(3);
                    if (sf & 1u) {
                        colour(true);
                        o.cycles += cost(18);
                    } else {
                        colour(false);
                        o.cycles += 1 + cost(25);
                    }
                }
                // L_8c01ab18
                nj_strip_mode(o, o.ld32(0x8C11129Cu + row + 16));
                o.cycles += cost(5);
                o.cycles += cost(3);
                load_spec();
                if (sf & 8u) {
                    o.cycles += cost(2);
                    if (sf & 2u) {
                        o.cycles += cost(4);
                        list = nj_head_at(o, 0x8C0172C4u, col, spec);
                        o.cycles += cost(2);
                    } else {
                        o.cycles += 1 + cost(5);
                        list = nj_head_at(o, 0x8C01737Cu, col, spec);
                    }
                    o.cycles += cost(2);
                } else {
                    o.cycles += 1 + cost(2);
                    if (sf & 2u) {
                        o.cycles += cost(4);
                        list = nj_head_at(o, 0x8C017252u, col, spec);
                        o.cycles += cost(2);
                    } else {
                        o.cycles += 1 + cost(5);
                        list = nj_head_at(o, 0x8C01730Eu, col, spec);
                    }
                    o.cycles += cost(1);
                }
                // L_8c01ab7c: the UV scale for the writer.
                o.cycles += cost(3);
                if (type == 65) {
                    w.f5 = o.ldf(0x8C01AD8Cu);
                    o.cycles += cost(3);
                } else {
                    w.f5 = o.ldf(0x8C01AD90u);
                    o.cycles += 1 + cost(2);
                }
                o.cycles += cost(3);
                o.cycles += limit != 0 ? cost(5) : 1 + cost(3);
                fn = o.ld32(table + row + 24);
                o.cycles += cost(6);
                std::uint32_t r0 = 0;
                if (!nj_writer(w, fn, 0x8C01ABAEu, frame, p, wb, count, list, f15, w.f5, r0))
                    return false;
                o.cycles += cost(2);
                list = r0;
                goto strip_end;
            }
            // L_8c01abb2: unlit.
            o.cycles += cost(2);
            nj_strip_mode(o, o.ld32(0x8C11129Cu + row + 20));
            o.cycles += cost(5);
            o.cycles += cost(3);
            if (sf & 8u) {
                o.cycles += cost(3);
                list = nj_head_at(o, 0x8C01707Au, col, spec);
                o.cycles += cost(2);
            } else {
                o.cycles += 1 + cost(3);
                list = nj_head_at(o, 0x8C017042u, col, spec);
            }
            o.cycles += cost(4);
            if (type == 65) {
                w.f4 = o.ldf(0x8C01AD8Cu);
                o.cycles += cost(3);
            } else {
                w.f4 = o.ldf(0x8C01AD90u);
                o.cycles += 1 + cost(2);
            }
            o.cycles += cost(3);
            o.cycles += limit != 0 ? cost(5) : 1 + cost(3);
            fn = o.ld32(table + row + 28);
            o.cycles += cost(5);
            std::uint32_t r0 = 0;
            if (!nj_writer(w, fn, 0x8C01AC04u, frame, p, wb, count, list, w.f4, w.f5, r0))
                return false;
            o.cycles += cost(2);
            list = r0;
            goto strip_end;
        }
        o.cycles += cost(2) + cost(2);
        if (type != 64) {
            o.cycles += 1 + cost(2);
            list = 0;  // r4 (r10, 0): no strip
            goto strip_end;
        }
        o.cycles += cost(2);
        // L_8c01ac08: type 64.
        o.cycles += cost(2);
        if (sf & 64u) {
            o.cycles += cost(3);
            if (lit != 0) {
                o.cycles += cost(3);
                if (sf & 1u) {
                    colour(true);
                    o.cycles += cost(18);
                } else {
                    colour(false);
                    o.cycles += 1 + cost(25);
                }
                // L_8c01ac6e
                nj_strip_mode(o, o.ld32(0x8C11129Cu + row));
                o.cycles += cost(5);
                o.cycles += cost(4);
                load_spec();
                if (sf & 8u) {
                    o.cycles += cost(2);
                    if (sf & 2u) {
                        o.cycles += cost(4);
                        list = nj_head_at(o, 0x8C0172C4u, col, spec);
                        o.cycles += cost(2);
                    } else {
                        o.cycles += 1 + cost(5);
                        list = nj_head_at(o, 0x8C01737Cu, col, spec);
                    }
                    o.cycles += cost(2);
                } else {
                    o.cycles += 1 + cost(2);
                    if (sf & 2u) {
                        o.cycles += cost(4);
                        list = nj_head_at(o, 0x8C017252u, col, spec);
                        o.cycles += cost(2);
                    } else {
                        o.cycles += 1 + cost(5);
                        list = nj_head_at(o, 0x8C01730Eu, col, spec);
                    }
                    o.cycles += cost(1);
                }
                // L_8c01acba
                o.cycles += cost(3);
                o.cycles += limit != 0 ? 1 + cost(5) : cost(2) + cost(3);
                fn = o.ld32(table + row + 8);
                o.cycles += cost(6);
                std::uint32_t r0 = 0;
                if (!nj_writer(w, fn, 0x8C01ADCAu, frame, p, wb, count, list, f15, w.f5, r0))
                    return false;
                o.cycles += cost(2);
                list = r0;
                goto strip_end;
            }
            // L_8c01acc4: unlit.
            o.cycles += 1;
            nj_strip_mode(o, o.ld32(0x8C11129Cu + row + 4));
            o.cycles += cost(5);
            o.cycles += cost(3);
            if (sf & 8u) {
                o.cycles += cost(3);
                list = nj_head_at(o, 0x8C01707Au, col, spec);
                o.cycles += cost(2);
            } else {
                o.cycles += 1 + cost(3);
                list = nj_head_at(o, 0x8C017042u, col, spec);
            }
            o.cycles += cost(4);
            o.cycles += limit != 0 ? cost(2) + cost(5) : 1 + cost(2) + cost(3);
        } else {
            // L_8c01acf4
            o.cycles += 1 + cost(3);
            if (lit != 0) {
                o.cycles += cost(2);
                if (sf & 4u) {
                    colour(true);
                    o.cycles += cost(18);
                } else {
                    colour(false);
                    o.cycles += 1 + cost(25);
                }
                // L_8c01ad54
                nj_strip_mode(o, o.ld32(0x8C11129Cu + row));
                o.cycles += cost(5);
                o.cycles += cost(3);
                const std::uint32_t head = (sf & 8u) ? 0x8C01720Cu : 0x8C0171C0u;
                o.cycles += (sf & 8u) ? cost(3) : 1 + cost(1);
                o.cycles += cost(3);
                list = nj_head_at(o, head, col, spec);
                o.cycles += cost(4);
                o.cycles += limit != 0 ? cost(5) : 1 + cost(3);
                fn = o.ld32(table + row + 8);
                o.cycles += cost(6);
                std::uint32_t r0 = 0;
                if (!nj_writer(w, fn, 0x8C01ADCAu, frame, p, wb, count, list, f15, w.f5, r0))
                    return false;
                o.cycles += cost(2);
                list = r0;
                goto strip_end;
            }
            // L_8c01adce: unlit.
            o.cycles += 1;
            nj_strip_mode(o, o.ld32(0x8C11129Cu + row + 4));
            o.cycles += cost(5);
            o.cycles += cost(3);
            const std::uint32_t head = (sf & 8u) ? 0x8C017016u : 0x8C016FE4u;
            o.cycles += (sf & 8u) ? cost(3) : 1 + cost(1);
            o.cycles += cost(2);
            list = nj_head_at(o, head, col, spec);
            o.cycles += cost(4);
            o.cycles += limit != 0 ? cost(5) : 1 + cost(3);
        }
        // L_8c01ae02: the writer for unlit strips.
        {
            fn = o.ld32(table + row + 12);
            o.cycles += cost(5);
            std::uint32_t r0 = 0;
            if (!nj_writer(w, fn, 0x8C01AE0Cu, frame, p, wb, count, list, w.f4, w.f5, r0))
                return false;
            o.cycles += cost(1);
            list = r0;
        }
    strip_end:
        // L_8c01ae0e: the list pointer after the strips, and on past the chunk.
        o.cycles += cost(2);
        if (list != 0) {
            o.cycles += cost(3);
            nj_end_strip(o, list);
        } else {
            o.cycles += 1;
        }
        o.cycles += cost(1);
        p += (size - 1) * 2;
        o.cycles += cost(6);
    }
}

// ---- 0x8C01AE52: Ninja's vertex chunk walker, and njCnkDrawModel (0x8C01AF76) ------------------
constexpr std::uint32_t kVtxDesc = 0x8C121364u;  // the descriptor the vertex handlers take (r4)
constexpr std::uint32_t kNear = 0x8C151C40u, kCounted = 0x8C151C44u;

// 0x8C03337C: a work-buffer address's store-queue address (the high bits pick SQ area 0 or 1).
std::uint32_t nj_sq_of(Out& o, std::uint32_t a) {
    const bool high = ((a >> 24) & 31u) >= 13;
    o.cycles += cost(10) + (high ? cost(1) : cost(3)) + cost(3);
    return (high ? 0xE1000000u : 0xE0000000u) | (a & 0x00FFFFFFu);
}

// A vertex chunk handler: natively when it is one written here, else as guest code. r0: the
// vertices it found nearer than the near plane.
bool nj_vertex_handler(Walk& w, std::uint32_t fn, std::uint32_t ret, std::uint32_t sp, std::uint32_t r4,
                       std::uint32_t r5, std::uint32_t& r0) {
    if (fn == 0x8C05E240u || fn == 0x8C05E2ECu || fn == 0x8C05E07Cu) {
        Regs x{};
        x.r[4] = r4;
        x.r[5] = r5;
        x.r[15] = sp;
        x.f[4] = w.f4;
        x.f[5] = w.f5;
        const bool ok = fn == 0x8C05E240u ? vtx41(w.c, x, w.o, w.m) : vtx_plain(w.c, x, w.o, w.m, fn == 0x8C05E07Cu);
        if (ok) {
            r0 = x.r[0];
            w.f4 = x.f[4];
            w.f5 = x.f[5];
            return true;
        }
    }
    GuestArgs a;
    a.r4 = r4;
    a.r5 = r5;
    a.f4 = w.f4;
    a.f5 = w.f5;
    if (!guest_call(w.c, w.m, w.o, fn, ret, sp, a, r0))
        return false;
    w.f4 = w.c.fr[4];
    w.f5 = w.c.fr[5];
    return true;
}

// r4 the vertex chunks, r5 the work buffer. Per chunk (header: type in the low byte, flags above;
// then its size in words, the first record's index and the vertex count) it fills the descriptor
// and has the type's handler (Ninja's table at 0x8C111204) transform the vertices into the work
// buffer; it counts the vertices drawn (0x8C120F00), the near ones (kNear) and those that count
// for culling (kCounted), and takes the type's lighting switch for the polygons (0x8C1214F8).
bool nj_vertices(Walk& w, std::uint32_t p, std::uint32_t wb, std::uint32_t sp) {
    Out& o = w.o;
    const std::uint32_t frame = sp - 36;  // eight saves and a word for r5
    o.cycles += cost(14);
    for (;;) {
        const std::uint32_t header = o.ld16s(p);
        p += 2;
        o.cycles += cost(5);
        if ((header & 0xFFFFu) == 255u) {
            o.cycles += cost(10);
            return true;
        }
        o.cycles += 1;
        const std::uint32_t size = o.ld16s(p) & 0xFFFFu;
        const std::uint32_t index = o.ld16s(p + 2);
        const std::uint32_t count = o.ld16s(p + 4);
        p += 6;
        o.cycles += cost(8);
        if (!(header & 0x8000u)) {
            o.st32(kNear, 0);
            o.st32(kCounted, 0);
            o.cycles += cost(5);
        }
        const std::uint32_t flags = header & 0x7FFFu;
        const std::uint32_t dest = (index << 5) + wb;
        o.st32(kVtxDesc + 20, count);
        o.st32(kVtxDesc + 24, p);
        o.st32(kVtxDesc + 28, dest);
        o.cycles += cost(14);
        const std::uint32_t sq = nj_sq_of(o, dest);
        const std::uint32_t type = flags & 0xFFu;
        o.st32(kVtxDesc + 32, sq);
        o.st32(0x8C120F00u, o.ld32(0x8C120F00u) + count);
        o.cycles += cost(9);
        std::uint32_t near = count;
        if (type == 44) {
            o.cycles += 1 + cost(4);
            if (!nj_vertex_handler(w, 0x8C03D9B8u, 0x8C01AF1Cu, frame, kVtxDesc, flags, near))
                return false;
            o.cycles += cost(1);
        } else {
            const std::uint32_t fn = o.ld32(0x8C111204u + (type - 32) * 8);
            o.cycles += cost(8);
            if (fn == 0) {
                o.cycles += 1 + cost(2);
            } else {
                o.cycles += cost(9);
                if (!nj_vertex_handler(w, fn, 0x8C01AED8u, frame, kVtxDesc, 0, near))
                    return false;
                o.cycles += cost(2) + cost(1);
            }
        }
        // L_8c01af1e
        o.st32(0x8C1214F8u, o.ld32(0x8C111208u + (type - 32) * 8));
        o.st32(kNear, o.ld32(kNear) + near);
        o.cycles += cost(19);
        if ((flags & 0xFF00u) == 512u) {
            o.cycles += 1;
            o.st32(kCounted, o.ld32(kCounted) + count);
            o.cycles += cost(3);
        } else {
            o.cycles += cost(3);
            if (type == 44) {
                o.cycles += 1;
            } else {
                o.st32(kCounted, o.ld32(kCounted) + count);
                o.cycles += cost(3);
            }
        }
        p += size * 4 - 4;
        o.cycles += cost(4);
    }
}

// 0x8C041B40: the model's bounding sphere (centre at +8, radius at +20, in model space) against
// Ninja's view volume (near and far at 0x8C120EBC, the sides' slopes at 0x8C120E8C and the planes
// from 0x8C120EC4): true when it lies wholly outside.
bool nj_cull(const Ctx& c, Out& o, std::uint32_t model) {
    const Mat M(c);
    float f4 = o.ldf(model + 8), f5 = o.ldf(model + 12), f6 = o.ldf(model + 16), f7 = 1.0f;
    M.apply(f4, f5, f6, f7);
    const float f8 = o.ldf(model + 20);
    float f1 = o.ldf(0x8C120EBCu) + f8;
    f6 = neg(f6);
    bool t = f6 > f1;
    f6 += f8;
    f1 = o.ldf(0x8C120EC0u);
    o.cycles += cost(15);
    auto out = [&] {
        o.cycles += 1 + cost(2);
        return true;
    };
    if (t)
        return out();
    t = f6 > f1;
    float f2 = o.ldf(0x8C120E8Cu);
    o.cycles += cost(4);
    if (!t)
        return out();
    f4 *= f2;
    float f3 = o.ldf(0x8C120E90u);
    f2 *= f8;
    float f9 = f4;
    f5 *= f3;
    float f0 = o.ldf(0x8C120EC4u);
    f3 *= f8;
    float f10 = f5;
    f0 *= f6;
    f1 = o.ldf(0x8C120EC8u);
    f4 -= f2;
    t = f4 > f0;
    f9 += f2;
    o.cycles += cost(14);
    if (t)
        return out();
    f1 *= f6;
    f0 = o.ldf(0x8C120ECCu);
    t = f1 > f9;
    f5 += f3;
    o.cycles += cost(5);
    if (t)
        return out();
    f0 *= f6;
    f1 = o.ldf(0x8C120ED0u);
    f10 -= f3;
    t = f5 > f0;
    f1 *= f6;
    o.cycles += cost(6);
    if (t)
        return out();
    t = f1 > f10;
    o.cycles += cost(2);
    if (t)
        return out();
    o.cycles += cost(2);
    return false;
}

// njCnkDrawModel: r4 the model (+0 its vertex chunks, +4 its polygon chunks, +8 the bounding
// sphere). Culls it when Ninja's flags ask (bit 8), then the vertices into the work buffer at
// 0x8C120EFC and the polygons from them. Returns -1 when culled, else 0.
bool nj_draw_model(Walk& w, std::uint32_t model, std::uint32_t sp, std::uint32_t& r0) {
    Out& o = w.o;
    const std::uint32_t flags = o.ld32(kFlags);
    const std::uint32_t wb = o.ld32(0x8C120EFCu);
    const std::uint32_t vlist = o.ld32(model);
    o.cycles += cost(13);
    if (flags & 256u) {
        const float radius = o.ldf(model + 20);
        o.cycles += cost(5);
        if (radius > 0.0f) {
            o.cycles += cost(3);
            const bool outside = nj_cull(w.c, o, model);
            o.cycles += cost(2);
            if (outside) {
                o.cycles += cost(2) + cost(5);
                r0 = 0xFFFFFFFFu;
                return true;
            }
        }
        o.cycles += 1;
    }
    o.cycles += cost(2);
    if (vlist != 0) {
        o.cycles += cost(3);
        if (!nj_vertices(w, vlist, wb, sp - 16))
            return false;
    } else {
        o.cycles += 1;
    }
    const std::uint32_t plist = o.ld32(model + 4);
    const std::uint32_t limit = o.ld32(kNear);
    o.cycles += cost(5);
    if (plist != 0) {
        o.cycles += cost(2);
        if (!nj_walk(w, plist, limit, wb, sp - 16))
            return false;
    }
    o.cycles += cost(1) + cost(5);
    r0 = 0;
    return true;
}

// ---- 0x8C030EB4: one wide line, cut at the near plane -----------------------------------------
// Rez's other line drawing (called from 0x8C030EA8 and the stage overlays): fr4-fr6 and fr7-fr9 the
// ends on the screen (x, y and 1/w), fr10 the width, r4 where in Kamui's list. An end with 1/w
// below 0 or above 1 is outside: with both outside nothing is drawn; with one, it is moved along
// the line to where 1/w is 1, interpolated in w. Then as the line renderer below: one sprite, the
// ends widened across the line's direction by the width over its length (FSRRA) times the constant
// at 0x8C031214. Returns the list pointer after it. Compiled C.
constexpr std::uint32_t kLineScale = 0x8C031214u;
// The cut: false when nothing is drawn, else the cut ends in `e` (x, y, 1/w of each). Charges the
// original's cycles for the tests and the cut.
bool cut_line(float (&e)[6], Out& o) {
    float f4 = e[0], f5 = e[1], f6 = e[2], f7 = e[3], f8 = e[4], f9 = e[5];
    float f14 = 1.0f;
    f14 /= f6;
    float f15 = 1.0f;
    f15 /= f9;
    o.cycles += cost(10);
    bool a_out = true;
    if (!(0.0f > f6)) {
        o.cycles += cost(2);
        if (!(f6 > 1.0f)) {
            o.cycles += 1;
            a_out = false;
        }
    }
    bool draw = true, clip_a = false, clip_b = false;
    if (a_out) {
        o.cycles += cost(3);
        if (!(0.0f > f9)) {
            o.cycles += 1 + cost(3);
            if (f9 > 1.0f) {
                o.cycles += 1;
                draw = false;
            } else {
                clip_a = true;
            }
        } else {
            o.cycles += cost(2);
            draw = false;
        }
    } else {
        o.cycles += cost(3);
        if (0.0f > f9) {
            o.cycles += 1;
            clip_b = true;
        } else {
            o.cycles += cost(3);
            if (!(f9 > 1.0f))
                o.cycles += 1;
            else
                clip_b = true;
        }
    }
    if (!draw)
        return false;
    if (clip_a) {
        const float t = (1.0f - f15) / (f14 - f15);
        const float bx = f7 * f15, by = f8 * f15;
        f4 = fmac(t, f4 * f14 - bx, bx);
        f5 = fmac(t, f5 * f14 - by, by);
        f6 = 1.0f;
        o.cycles += cost(23);
    } else if (clip_b) {
        const float t = (1.0f - f14) / (f15 - f14);
        const float ax = f4 * f14, ay = f5 * f14;
        f7 = fmac(t, f7 * f15 - ax, ax);
        f8 = fmac(t, f8 * f15 - ay, ay);
        f9 = 1.0f;
        o.cycles += cost(22);
    }
    e[0] = f4, e[1] = f5, e[2] = f6, e[3] = f7, e[4] = f8, e[5] = f9;
    return true;
}
bool rez_line(Regs& x, Out& o, Memory& m) {
    const std::uint8_t* kp = ram_span(m, kLineScale, 4);
    if (!kp)
        return false;
    float e[6] = {x.f[4], x.f[5], x.f[6], x.f[7], x.f[8], x.f[9]};
    std::uint32_t r4 = x.r[4];
    if (cut_line(e, o)) {
        const float f4 = e[0], f5 = e[1], f6 = e[2], f7 = e[3], f8 = e[4], f9 = e[5];
        float dx = f7 - f4;
        float dy = f8 - f5;
        float len = dx * dx;
        len = fmac(dy, dy, len);
        float s = 1.0f / std::sqrt(len);
        s *= x.f[10];
        s *= rdf(kp);
        dy *= s;
        const bool t = 0.0f > dy;
        dx *= s;
        o.cycles += cost(19);
        if (t) {
            dx = neg(dx);
            dy = neg(dy);
            o.cycles += cost(2);
        }
        put8(o, r4, 0xF0000000u, f2u(f4 - dy), f2u(f5 + dx), f2u(f6), f2u(f4 + dy), f2u(f5 - dx), f2u(f6),
             f2u(f7 + dy));
        const std::uint32_t second[8] = {f2u(f8 - dx), f2u(f9), f2u(f7 - dy), f2u(f8 + dx), 0, 0, 0, 0};
        o.partial(r4 + 32, second, 0x0Fu);
        o.cycles += cost(30);
        r4 += 64;
    }
    o.cycles += cost(4);
    x.r[0] = r4;
    x.r[4] = r4;
    return true;
}

// The edge list of a model (+4 the count, from +8 pairs of 16-bit indices into `verts`, 16 bytes
// each), for the two line drawers below: null when it is not all in RAM (the translated code then
// runs). `edges` is null with no edges.
bool line_edges(Memory& m, std::uint32_t list, std::uint32_t verts, std::int32_t& n,
                const std::uint8_t*& edges, const std::uint8_t*& vp) {
    const std::uint8_t* lp = ram_span(m, list, 8);
    if (!lp)
        return false;
    n = static_cast<std::int32_t>(rd32(lp + 4));
    edges = nullptr;
    vp = nullptr;
    if (n <= 0)
        return true;
    edges = ram_span(m, list + 8, static_cast<std::uint32_t>(n) * 4);
    if (!edges)
        return false;
    std::uint32_t top = 0;
    for (std::int32_t i = 0; i < 2 * n; ++i)
        top = std::max<std::uint32_t>(top, rd16s(edges + 2 * i) & 0xFFFFu);
    vp = ram_span(m, verts, (top + 1) * 16);
    return vp != nullptr;
}

// ---- 0x8C03D7E0: the screen positions Rez's line renderer reads -------------------------------
// Called by Ninja's object drawing (0x8C01A256, 0x8C01C5AC) for a wireframe model. r4: a
// descriptor (+20 the count, +24 the vertices, 12 bytes each, +28 a cache-operation pointer, +32
// where the positions go: a store-queue address). Each vertex by Ninja's matrix (FTRV), then 1/-z,
// and the screen scale and centre at 0x8C120E8C: x, y, 1/w and 1.0, 16 bytes, two to a record (an
// odd last one flushed with the queue half's other words as they were). Returns the count of
// vertices with -z not above 1. Hand-written, so every register is left as the original leaves it.
constexpr std::uint32_t kLineScreen = 0x8C120E8Cu;
bool line_vertices(const Ctx& c, Regs& x, Out& o, Memory& m) {
    const std::uint8_t* dp = ram_span(m, x.r[4] + 20, 16);
    const std::uint8_t* sc = ram_span(m, kLineScreen, 16);
    if (!dp || !sc)
        return false;
    const std::uint32_t n = rd32(dp), src = rd32(dp + 4), r7 = rd32(dp + 8), dst = rd32(dp + 12);
    if (n == 0 || n > 0x10000u || (dst & 31u) != 0)
        return false;  // the translated code then runs (zero would loop 2^32 times there)
    const std::uint8_t* sp = ram_span(m, src, 12 * n);
    if (!sp)
        return false;
    const float f0 = rdf(sc), f1 = rdf(sc + 4), f2 = rdf(sc + 8), f3 = rdf(sc + 12);
    const Mat M(c);
    o.cycles += cost(18);
    std::uint32_t near = 0;
    float f4 = 0.0f, f5 = 0.0f, f6 = 0.0f, f7 = 0.0f, f8 = 0.0f;
    std::uint32_t pair[8] = {};
    for (std::uint32_t i = 0; i < n; ++i, sp += 12) {
        f4 = rdf(sp);
        f5 = rdf(sp + 4);
        f6 = rdf(sp + 8);
        f7 = 1.0f;
        M.apply(f4, f5, f6, f7);
        f8 = 1.0f;
        f6 = neg(f6);
        f8 /= f6;
        o.cycles += cost(14);
        if (f6 > 1.0f) {
            o.cycles += 1;
        } else {
            ++near;
            o.cycles += cost(1);
        }
        f5 *= f1;
        f4 *= f0;
        f5 *= f8;
        f4 *= f8;
        f5 += f3;
        f4 += f2;
        o.cycles += cost(12);
        std::uint32_t* v = pair + 4 * (i & 1u);
        v[0] = f2u(f4);
        v[1] = f2u(f5);
        v[2] = f2u(f8);
        v[3] = f2u(1.0f);
        if (i & 1u) {
            put8(o, dst + 32 * (i / 2), pair[0], pair[1], pair[2], pair[3], pair[4], pair[5], pair[6], pair[7]);
            o.cycles += cost(3);
        }
        o.cycles += cost(3);
    }
    o.cycles += cost(2);
    if (n & 1u) {
        o.partial(dst + 32 * (n / 2), pair, 0x0Fu);
        o.cycles += cost(2);
    } else {
        o.cycles += 1;
    }
    o.cycles += cost(2);
    x.r[0] = near;
    x.r[1] = 0;
    x.r[2] = dst + 16 * n;
    x.r[3] = src + 12 * n;
    x.r[4] = kLineScreen + 16;
    x.r[5] = src + 12 + 12 * n;
    x.r[6] = near;
    x.r[7] = r7 + 32 * (n / 2);
    x.t = (n & 1u) == 0 ? 1u : 0u;
    x.f[0] = f0;
    x.f[1] = f1;
    x.f[2] = f2;
    x.f[3] = f3;
    x.f[4] = f4;
    x.f[5] = f5;
    x.f[6] = f6;
    x.f[7] = f7;
    x.f[8] = f8;
    x.f[9] = 1.0f;
    x.f[11] = 1.0f;
    return true;
}

// ---- 0x8C0ACF7E: Rez's wireframe line renderer ------------------------------------------------
// Rez's wireframe look: called by fn_8C0AD0AA and fn_8C0AD03A after a model is drawn, it widens
// each of the model's edges into a quad, as one sprite of Kamui's list. r4: where in the list (a
// store-queue address); r5: the edge list (+4 the count, from +8 pairs of 16-bit vertex indices);
// r6: screen-space vertices, 16 bytes each (x, y, 1/w); fr4 the half width, times the constant at
// 0x8C0AD11C. Per edge: across the edge's direction by the width over its length (FSRRA: the
// runtime's 1/sqrt), on the side where the direction's y is not negative; two records, the second
// only its first four words (the queue gives the rest, as the original's flush takes them). Adds
// the count to 0x8C20F18C and returns the list pointer after the sprites. Compiled C.
constexpr std::uint32_t kLineWidth = 0x8C0AD11Cu, kLineCount = 0x8C20F18Cu;
// The lines go to the GPU: in place of the sprites, one parameter of the type the PowerVR reserves
// (6), kind 1, with the count and the half width, then each line's ends (xa, ya, za, xb, yb, zb)
// in the records after it, the last padded. The display-list decoder turns it into a LineBatch the
// GPU widens into the same quads. The sprites' cycles are still charged (the widening's sign test
// decides them), so guest timing is unchanged.
// A line batch: the parameter (the count, the half width), then the lines' ends, six floats a
// line, eight to a record, the last record padded. Returns the list pointer after it.
std::uint32_t put_line_batch(Out& o, std::uint32_t r4, float half_width, const float* ends, std::uint32_t lines) {
    put8(o, r4, 0xC0000001u, lines, f2u(half_width), 0, 0, 0, 0, 0);
    r4 += 32;
    const std::size_t floats = std::size_t{lines} * 6;
    for (std::size_t i = 0; i < floats; i += 8) {
        std::uint8_t* d = o.begin(r4);
        if (i + 8 <= floats) {
            std::memcpy(d, ends + i, 32);
        } else {
            std::memset(d, 0, 32);
            std::memcpy(d, ends + i, (floats - i) * sizeof(float));
        }
        o.end(r4);
        r4 += 32;
    }
    return r4;
}
bool rez_lines(Regs& x, Out& o, Memory& m) {
    const std::uint8_t* kp = ram_span(m, kLineWidth, 4);
    std::int32_t n = 0;
    const std::uint8_t* edges = nullptr;
    const std::uint8_t* vp = nullptr;
    if (!kp || !line_edges(m, x.r[5], x.r[6], n, edges, vp))
        return false;
    const float f4 = x.f[4] * rdf(kp);
    o.st32(kLineCount, o.ld32(kLineCount) + static_cast<std::uint32_t>(n));
    o.cycles += cost(19);
    std::uint32_t r4 = x.r[4];
    if (n > 0) {
        o.cycles += cost(1);
        thread_local std::vector<float> ends;
        if (ends.size() < std::size_t(n) * 6)
            ends.resize(std::size_t(n) * 6);
        float* e = ends.data();
        for (std::int32_t i = 0; i < n; ++i, e += 6) {
            const std::uint8_t* a = vp + std::size_t{rd16s(edges + 4 * i) & 0xFFFFu} * 16;
            const std::uint8_t* b = vp + std::size_t{rd16s(edges + 4 * i + 2) & 0xFFFFu} * 16;
            const float xa = rdf(a), ya = rdf(a + 4), za = rdf(a + 8);
            const float xb = rdf(b), yb = rdf(b + 4), zb = rdf(b + 8);
            // The original's widening, for its sign test (the cycles it charges).
            const float f6 = xb - xa;
            const float f5 = yb - ya;
            const float f3 = fmac(f5, f5, f6 * f6);
            o.cycles += cost(36) + (0.0f > f5 * ((1.0f / std::sqrt(f3)) * f4) ? cost(2) : 0) + cost(31);
            e[0] = xa, e[1] = ya, e[2] = za, e[3] = xb, e[4] = yb, e[5] = zb;
        }
        r4 = put_line_batch(o, r4, f4, ends.data(), static_cast<std::uint32_t>(n));
    }
    o.cycles += cost(5);
    x.r[0] = r4;
    x.r[4] = r4;
    x.f[4] = f4;
    return true;
}

// ---- 0x8C0AD03A: a model's edges as single wide lines ------------------------------------------
// fn_8C0AD0AA's other path, taken while [0x8C151C40] is set: the line renderer's arguments (r4 the
// list, r5 the edge list, r6 the screen positions, fr4 the width), but each edge goes to the single
// wide line above (0x8C030EB4), so an end outside 1/w in [0, 1] is cut rather than drawn. It adds
// the count to 0x8C20F18C and returns the list pointer after the lines. The original calls the line
// through a register for each edge; here the drawn lines go to the GPU as one batch (as in
// rez_lines). Compiled C.
bool rez_lines_cut(Regs& x, Out& o, Memory& m) {
    const std::uint8_t* kp = ram_span(m, kLineScale, 4);
    std::int32_t n = 0;
    const std::uint8_t* edges = nullptr;
    const std::uint8_t* vp = nullptr;
    if (!kp || !line_edges(m, x.r[5], x.r[6], n, edges, vp))
        return false;
    o.st32(kLineCount, o.ld32(kLineCount) + static_cast<std::uint32_t>(n));
    o.cycles += cost(20);
    std::uint32_t r4 = x.r[4];
    if (n > 0) {
        // Each line cut as the single line cuts it, the drawn ones sent as one line batch, widened
        // on the GPU by the single line's half width (fr4 times the constant at 0x8C031214). The
        // cycles are the single line's: its cut and its widening's sign test.
        o.cycles += cost(1);
        const float half = x.f[4] * rdf(kp);
        thread_local std::vector<float> ends;
        if (ends.size() < std::size_t(n) * 6)
            ends.resize(std::size_t(n) * 6);
        std::uint32_t drawn = 0;
        for (std::int32_t i = 0; i < n; ++i) {
            const std::uint8_t* a = vp + std::size_t{rd16s(edges + 4 * i) & 0xFFFFu} * 16;
            const std::uint8_t* b = vp + std::size_t{rd16s(edges + 4 * i + 2) & 0xFFFFu} * 16;
            float e[6] = {rdf(a), rdf(a + 4), rdf(a + 8), rdf(b), rdf(b + 4), rdf(b + 8)};
            o.cycles += cost(23);
            if (cut_line(e, o)) {
                const float dx = e[3] - e[0];
                const float dy = e[4] - e[1];
                float s = 1.0f / std::sqrt(fmac(dy, dy, dx * dx));
                s *= x.f[4];
                s *= rdf(kp);
                o.cycles += cost(19) + (0.0f > dy * s ? cost(2) : 0) + cost(30);
                std::memcpy(ends.data() + std::size_t{drawn} * 6, e, sizeof e);
                ++drawn;
            }
            o.cycles += cost(4) + cost(3);
        }
        if (drawn != 0)
            r4 = put_line_batch(o, r4, half, ends.data(), drawn);
    }
    o.cycles += cost(9);
    x.r[0] = r4;
    return true;
}

// ---- 0x8C0AC9C8, 0x8C0AC94E: a wireframe model's vertices and edges, merged -------------------
// When a stage's wireframe models are prepared (the frames after an area is chosen, which draw
// nothing), each model's vertices go into the table at [0x8C20F190] (28 bytes each: a key, x, y,
// z, three more floats) and its edges into the list at [0x8C20F198] (pairs of 16-bit indices),
// both merged: a vertex takes the key of the first earlier one at the same position, and an edge
// whose ends' keys match an earlier edge's is not added again. Both scans are quadratic; here they
// read guest memory through pointers and charge the original's cycles for every compare.
// Compiled C.
constexpr std::uint32_t kWireVerts = 0x8C20F190u, kWireAppend = 0x8C20F194u;
// 0x8C0AC9C8 (r4: a block: the table's count so far, the count to add (16-bit each), then per
// vertex x, y, z, three more floats when r5 bit 0 is set (else zeros), 4 bytes skipped when bit 1
// is). Returns the count added.
bool wire_vertices(Regs& x, Out& o, Memory& m) {
    const std::uint32_t blk = x.r[4], flags = x.r[5];
    const std::uint8_t* hp = ram_span(m, blk, 4);
    const std::uint8_t* bp = ram_span(m, kWireVerts, 4);
    if (!hp || !bp)
        return false;
    const std::uint32_t have0 = rd16s(hp), add = rd16s(hp + 2);
    if (have0 >= 0x8000u || add >= 0x8000u)
        return false;  // negative: the translated code then runs
    const std::uint32_t stride = 12 + ((flags & 1u) ? 12 : 0) + ((flags & 2u) ? 4 : 0);
    const std::uint32_t base = rd32(bp), src_a = blk + 4;
    const std::uint8_t* src = ram_span(m, src_a, add * stride);
    const std::uint8_t* tab = ram_span(m, base, (have0 + add) * 28);
    if (!src || !tab)
        return false;
    // The entries written must not be read back as the block (the original reads it as it goes).
    const std::uint32_t w0 = (base & 0x00FFFFFFu) + have0 * 28, w1 = w0 + add * 28;
    const std::uint32_t s0 = src_a & 0x00FFFFFFu, s1 = s0 + add * stride;
    if (add != 0 && w0 < s1 && s0 < w1)
        return false;
    const std::uint32_t c1 = cost(1), c2 = cost(2), c4 = cost(4), c6 = cost(6), c10 = cost(10);
    o.cycles += cost(23);
    std::uint32_t have = have0;
    for (std::uint32_t i = 0; i < add; ++i, src += stride) {
        const float px = rdf(src), py = rdf(src + 4), pz = rdf(src + 8);
        o.cycles += c10;
        std::uint32_t key = have;
        const std::uint8_t* e = tab;
        for (std::uint32_t j = 0; j < have; ++j, e += 28) {
            o.cycles += c4;
            if (rdf(e + 4) != px) {
                o.cycles += 1 + c4;
                continue;
            }
            o.cycles += c4;
            if (rdf(e + 8) != py) {
                o.cycles += 1 + c4;
                continue;
            }
            o.cycles += c4;
            if (rdf(e + 12) != pz) {
                o.cycles += 1 + c4;
                continue;
            }
            o.cycles += c2;
            key = rd32(e);
            break;
        }
        const std::uint32_t at = base + have * 28;
        o.st32(at, key);
        o.st32(at + 4, rd32(src));
        o.st32(at + 8, rd32(src + 4));
        o.st32(at + 12, rd32(src + 8));
        o.cycles += c10;
        if (flags & 1u) {
            o.st32(at + 16, rd32(src + 12));
            o.st32(at + 20, rd32(src + 16));
            o.st32(at + 24, rd32(src + 20));
            o.cycles += c10;
        } else {
            o.st32(at + 16, 0);
            o.st32(at + 20, 0);
            o.st32(at + 24, 0);
            o.cycles += c6;
        }
        o.cycles += c4;
        if (flags & 2u)
            o.cycles += c1;
        o.cycles += c4;
        ++have;
    }
    o.cycles += cost(9);
    x.r[0] = add;
    return true;
}
// 0x8C0AC94E (r4, r5: an edge's two vertex indices; r6: the list's length so far): 0 when an edge
// with the same two keys is listed already, else the edge appended (the smaller index first) at
// [0x8C20F194], which moves on by 4, and 1.
bool wire_edge(Regs& x, Out& o, Memory& m) {
    std::uint32_t a = x.r[4], b = x.r[5];
    const auto n = static_cast<std::int32_t>(x.r[6]);
    const std::uint8_t* gp = ram_span(m, kWireVerts, 12);
    if (!gp)
        return false;
    const std::uint32_t base = rd32(gp), app = rd32(gp + 4), list = rd32(gp + 8);
    o.cycles += cost(7);
    if (static_cast<std::int32_t>(a) > static_cast<std::int32_t>(b)) {
        std::swap(a, b);
        o.cycles += cost(3);
    } else {
        o.cycles += 1;
    }
    const std::uint8_t* pa = ram_span(m, base + 28u * a, 4);
    const std::uint8_t* pb = ram_span(m, base + 28u * b, 4);
    const std::uint8_t* tab = ram_span(m, base, 4);
    if (!pa || !pb || !tab || !ram_span(m, app, 4))
        return false;
    const std::uint32_t ka = rd32(pa), kb = rd32(pb);
    // Each listed index (0-65535) is read through the table: all of it must be RAM.
    const bool whole = ram_span(m, base, 28u * 65536u) != nullptr;
    o.cycles += cost(15);
    if (n > 0) {
        const std::uint8_t* lp = ram_span(m, list, static_cast<std::uint32_t>(n) * 4);
        if (!lp || !whole)
            return false;
        const std::uint32_t c2 = cost(2), c3 = cost(3), c13 = cost(13);
        for (std::int32_t i = 0; i < n; ++i, lp += 4) {
            const std::uint32_t ea = rd16s(lp) & 0xFFFFu, eb = rd16s(lp + 2) & 0xFFFFu;
            o.cycles += c13;
            if (rd32(tab + ea * 28) != ka) {
                o.cycles += c3;
                continue;
            }
            o.cycles += c2;
            if (rd32(tab + eb * 28) != kb) {
                o.cycles += 1 + c3;
                continue;
            }
            o.cycles += c2 + cost(6);
            x.r[0] = 0;
            return true;
        }
    }
    o.st16(app, a);
    o.st16(app + 2, b);
    o.st32(kWireAppend, app + 4);
    o.cycles += cost(10) + cost(6);
    x.r[0] = 1;
    return true;
}
}  // namespace

DC_HOOK_ENTRY(rez_lines) {
    return hook_entry(c, m, 0x8C0ACF7Eu, [&m](Ctx&, Regs& x, Out& o) { return rez_lines(x, o, m); });
}
DC_HOOK_ENTRY(rez_line) {
    return hook_entry(c, m, 0x8C030EB4u, [&m](Ctx&, Regs& x, Out& o) { return rez_line(x, o, m); });
}
DC_HOOK_ENTRY(rez_lines_cut) {
    return hook_entry(c, m, 0x8C0AD03Au, [&m](Ctx&, Regs& x, Out& o) { return rez_lines_cut(x, o, m); });
}
DC_HOOK_ENTRY(rez_wire_vertices) {
    return hook_entry(c, m, 0x8C0AC9C8u, [&m](Ctx&, Regs& x, Out& o) { return wire_vertices(x, o, m); });
}
DC_HOOK_ENTRY(rez_wire_edge) {
    return hook_entry(c, m, 0x8C0AC94Eu, [&m](Ctx&, Regs& x, Out& o) { return wire_edge(x, o, m); });
}
DC_HOOK_ENTRY(rez_line_vertices) {
    return hook_entry(c, m, 0x8C03D7E0u,
                      [&m](Ctx& cc, Regs& x, Out& o) { return line_vertices(cc, x, o, m); });
}

DC_HOOK_ENTRY(nj_vtx41) {
    return hook_entry(c, m, 0x8C05E240u, [&m](Ctx& cc, Regs& x, Out& o) { return vtx41(cc, x, o, m); });
}
DC_HOOK_ENTRY(nj_vtx34) {
    return hook_entry(c, m, 0x8C05E2ECu,
                      [&m](Ctx& cc, Regs& x, Out& o) { return vtx_plain(cc, x, o, m, false); });
}
DC_HOOK_ENTRY(nj_vtx35) {
    return hook_entry(c, m, 0x8C05E07Cu,
                      [&m](Ctx& cc, Regs& x, Out& o) { return vtx_plain(cc, x, o, m, true); });
}
DC_HOOK_ENTRY(nj_draw_model) {
    return hook_entry(c, m, 0x8C01AF76u, [&m](Ctx& cc, Regs& x, Out& o) {
        Walk w{cc, m, o, x.f[4], x.f[5]};
        std::uint32_t r0 = 0;
        if (!nj_draw_model(w, x.r[4], x.r[15], r0))
            return false;
        x.r[0] = r0;
        x.f[4] = w.f4;
        x.f[5] = w.f5;
        return true;
    });
}
DC_HOOK_ENTRY(nj_walk) {
    return hook_entry(c, m, 0x8C01A6C0u, [&m](Ctx& cc, Regs& x, Out& o) {
        Walk w{cc, m, o, x.f[4], x.f[5]};
        if (!nj_walk(w, x.r[4], x.r[5], x.r[6], x.r[15]))
            return false;
        x.f[4] = w.f4;
        x.f[5] = w.f5;
        return true;
    });
}
DC_HOOK_ENTRY(nj_strip_a) {
    return hook_entry(c, m, 0x8C058E98u, [&m](Ctx& cc, Regs& x, Out& o) { return strip_a(cc.pr, x, o, m); });
}
