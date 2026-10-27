// Harness implementation of the emitted-code ABI: a function table, direct calls, and hard
// failures for anything that needs the real runtime (exceptions, interrupts, untranslated code).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ops.h"

namespace dream::sh4 {
namespace {

std::vector<FunctionEntry>& table() {
    static std::vector<FunctionEntry> t;
    return t;
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

}  // namespace

namespace {
// A direct-mapped cache in front of find_function. The guest calls through registers millions of
// times a guest second, and each one would be a binary search over a few thousand entries
// followed by a short scan.
//
// find_function is not a pure function of the address: where a title installs an overlay, several
// entries share one address and the right one is chosen by reading a signature out of guest
// memory, so the answer there changes during the run. register_functions sorts signed entries
// *before* the unsigned one at the same address, so an address whose first entry is unsigned has
// no overlay at all and its answer is a property of the table alone: such a hit is returned as is.
// For a signed address the slot also keeps the address's first entry and the one that matched, and
// a hit is re-checked against memory as the search would (the matched entry's words still there,
// no earlier entry at the address matching).
constexpr std::size_t kFnCacheSize = 1u << 12;
struct FnCacheEntry {
    std::uint32_t phys;
    GuestFn fn;  // null means the slot is empty; only hits are cached
    const FunctionEntry* first = nullptr;  // signed: the address's first entry; null: unsigned
    const FunctionEntry* hit = nullptr;    // signed: the entry that matched
};
FnCacheEntry g_fn_cache[kFnCacheSize]{};

inline std::size_t fn_cache_slot(std::uint32_t phys) noexcept {
    return (phys >> 1) & (kFnCacheSize - 1);  // guest instructions are two-byte aligned
}

// Anything that can change what find_function would answer has to empty this, or a run would keep
// using a translation that is no longer the right one.
void flush_fn_cache() noexcept {
    for (auto& e : g_fn_cache) e = FnCacheEntry{};
}

// Whether guest memory at `address` holds the entry's signature words: one compare of the bytes
// where main RAM is mapped flat (a little-endian guest on a little-endian host), read16 otherwise.
bool signature_matches(const FunctionEntry& e, std::uint32_t address, ::dream::Memory& m) noexcept {
    const std::uint32_t bytes = 2 * e.signature_words;
    if (m.fast_ram && ::dream::Memory::is_ram(address) &&
        (address & 0x00FFFFFFu) + bytes <= 0x01000000u)
        return std::memcmp(m.fast_ram + (address & 0x00FFFFFFu), e.signature, bytes) == 0;
    for (std::uint32_t i = 0; i < e.signature_words; ++i)
        if (m.read16(address + 2 * i) != e.signature[i])
            return false;
    return true;
}

}  // namespace

void register_functions(const FunctionEntry* entries, std::size_t count) noexcept {
    flush_fn_cache();
    // Keyed by physical address so every RAM alias (P0/P1/P2/P3) of an entry resolves in O(log n).
    auto& t = table();
    for (std::size_t i = 0; i < count; ++i)
        t.push_back(FunctionEntry{entries[i].address & 0x1FFFFFFFu, entries[i].fn,
                                  entries[i].signature, entries[i].signature_words,
                                  entries[i].end ? entries[i].end & 0x1FFFFFFFu : 0u,
                                  entries[i].resume});
    // Signed (overlay) entries sort before the unconditional one at the same address.
    std::sort(t.begin(), t.end(), [](const FunctionEntry& a, const FunctionEntry& b) {
        if (a.address != b.address)
            return a.address < b.address;
        return (a.signature != nullptr) > (b.signature != nullptr);
    });
}

GuestFn find_function(std::uint32_t address, ::dream::Memory* m) noexcept {
    const std::uint32_t phys = address & 0x1FFFFFFFu;
    FnCacheEntry& slot = g_fn_cache[fn_cache_slot(phys)];
    if (slot.fn != nullptr && slot.phys == phys) {
        if (!slot.first)
            return slot.fn;
        // As the search below would go: no entry before the cached one matching now, and the
        // cached one still matching (an unsigned one always does).
        if (m) {
            bool earlier = false;
            for (const FunctionEntry* e = slot.first; !earlier && e != slot.hit; ++e)
                earlier = signature_matches(*e, address, *m);
            if (!earlier && (!slot.hit->signature || signature_matches(*slot.hit, address, *m)))
                return slot.fn;
        }
    }
    const auto& t = table();
    auto it =
        std::lower_bound(t.begin(), t.end(), phys,
                         [](const FunctionEntry& e, std::uint32_t a) { return e.address < a; });
    const FunctionEntry* first = it != t.end() ? &*it : nullptr;
    for (; it != t.end() && it->address == phys; ++it) {
        if (!it->signature) {
            // With no signed entry before it, the answer cannot change while the table stands; after signed ones, a hit re-checks them.
            slot.phys = phys;
            slot.fn = it->fn;
            slot.first = &*it == first ? nullptr : first;
            slot.hit = &*it;
            return it->fn;
        }
        if (!m)
            continue;
        if (signature_matches(*it, address, *m)) {
            slot.phys = phys;
            slot.fn = it->fn;
            slot.first = first;
            slot.hit = &*it;
            return it->fn;
        }
    }
    return nullptr;
}

namespace {
Hooks* g_hooks = nullptr;
}

void set_hooks(Hooks* h) noexcept {
    g_hooks = h;
}
Hooks* hooks() noexcept {
    return g_hooks;
}

namespace {
// The registered function whose range contains `address` and whose signature (if any) matches
// memory. Ranges can overlap (discovery keeps shared tails as their own functions), so the scan
// walks back from the first entry above the address.
const FunctionEntry* find_containing(std::uint32_t address, ::dream::Memory* m) noexcept {
    const auto& t = table();
    const std::uint32_t phys = address & 0x1FFFFFFFu;
    auto it =
        std::upper_bound(t.begin(), t.end(), phys,
                         [](std::uint32_t a, const FunctionEntry& e) { return a < e.address; });
    for (int n = 0; it != t.begin() && n < 1024; ++n) {
        --it;
        if (!it->end || phys < it->address || phys >= it->end)
            continue;
        if (!it->signature)
            return &*it;
        if (!m)
            continue;
        bool same = true;
        for (std::uint32_t i = 0; same && i < it->signature_words; ++i)
            same = m->read16(it->address + 2 * i) == it->signature[i];
        if (same)
            return &*it;
    }
    return nullptr;
}
}  // namespace

DREAM_RARE_CALL void nonlocal_return(Ctx& c, ::dream::Memory&, std::uint32_t pc) {
    c.pc = pc;
    c.unwinding = 1;
    c.unwind_pc = pc;
}

DREAM_RARE_CALL void resume_miss(Ctx& c, ::dream::Memory& m, std::uint32_t pc) {
    if (g_hooks) {
        g_hooks->on_resume(c, m, pc);
        return;
    }
    throw std::runtime_error("cannot resume translated code at " + hex(pc));
}

void resume_at(Ctx& c, ::dream::Memory& m, std::uint32_t pc) {
    c.pc = pc;
    const std::uint32_t phys = pc & 0x1FFFFFFFu;
    if (const FunctionEntry* e = find_containing(pc, &m)) {
        if (e->address == phys) {
            e->fn(c, m);
            return;
        }
        if (e->resume) {
            e->resume(c, m, pc);
            return;
        }
    }
    resume_miss(c, m, pc);
}

namespace {
template <typename Start>
void run_guest_impl(Ctx& c, ::dream::Memory& m, Start start) {
    const std::uint32_t stop_pr = c.pr;
    std::uint32_t pc = 0;
    bool resumed = false;
    // A non-local return arrives as c.unwinding from translated code and as NonLocalReturn from
    // native code in between (abi.h, rethrow_unwinding): both continue at the target.
    auto take = [&](std::uint32_t target) {
        if (target == stop_pr)
            return false;
        pc = target;
        resumed = true;
        return true;
    };
    for (;;) {
        try {
            if (!resumed) {
                start();
                if (c.unwinding) {
                    c.unwinding = 0;
                    if (take(c.unwind_pc))
                        continue;
                }
                return;  // the entry function returned through PR, as translated code does
            }
            // After a non-local return the host stack no longer mirrors the guest's: every return
            // comes back here and continues at PR.
            resume_at(c, m, pc);
            if (c.unwinding) {
                c.unwinding = 0;
                if (take(c.unwind_pc))
                    continue;
                return;
            }
            if (c.pr == stop_pr)
                return;
            pc = c.pr;
        } catch (const NonLocalReturn& n) {
            if (!take(n.pc))
                return;
        }
    }
}
}  // namespace

void run_guest(Ctx& c, ::dream::Memory& m, std::uint32_t entry) {
    run_guest_impl(c, m, [&] { call_indirect_in(c, m, entry); });
}

void run_guest(Ctx& c, ::dream::Memory& m, GuestFn entry) {
    run_guest_impl(c, m, [&] { entry(c, m); });
}

void call_indirect(Ctx& c, ::dream::Memory& m, std::uint32_t target) {
    call_indirect_in(c, m, target);
    rethrow_unwinding(c);
}

void call_indirect_in(Ctx& c, ::dream::Memory& m, std::uint32_t target) {
    if (GuestFn fn = find_function(target, &m)) {
        fn(c, m);
        return;
    }
    // A call or jump through a register that lands *inside* an already-translated function. That
    // is an ordinary thing for SH-4 code to do and it is not a missing translation: the block is
    // compiled, it simply is not a function entry and so has no row of its own in the table. The
    // function's resume entry can start at any of its block starts, which is the same door a
    // non-local return comes through. (Naming such a block as a discovery seed instead would
    // truncate the enclosing function.)
    if (const FunctionEntry* e = find_containing(target, &m); e && e->resume) {
        c.pc = target;
        // resume() dispatches on its own block starts and calls resume_miss for anything else,
        // so a target that is inside the range but not a block start still faults honestly
        // rather than running from the wrong place.
        e->resume(c, m, target);
        return;
    }
    if (g_hooks) {
        g_hooks->on_untranslated(c, m, target);
        return;
    }
    throw std::runtime_error("untranslated call target " + hex(target) + " from pc " + hex(c.pc));
}

void deliver_irq(Ctx& c, ::dream::Memory& m) {
    deliver_irq_in(c, m);
    rethrow_unwinding(c);
}

DREAM_RARE_CALL void deliver_irq_in(Ctx& c, ::dream::Memory& m) {
    if (g_hooks) {
        g_hooks->on_poll(c, m);
        return;
    }
    c.next_event = ~std::uint64_t{0};  // the bare harness never raises interrupts
}

void trapa(Ctx& c, ::dream::Memory& m, std::uint32_t imm) {
    trapa_in(c, m, imm);
    rethrow_unwinding(c);
}

void trapa_in(Ctx& c, ::dream::Memory& m, std::uint32_t imm) {
    if (g_hooks) {
        g_hooks->on_trapa(c, m, imm);
        return;
    }
    throw std::runtime_error("TRAPA #" + std::to_string(imm) + " at " + hex(c.pc) +
                             " (no exception model in the bare harness)");
}

bool rte(Ctx& c, ::dream::Memory& m) {
    if (g_hooks) {
        const bool unwind = g_hooks->in_exception_frame();
        g_hooks->on_rte(c, m);
        return unwind;
    }
    // The bare harness delivers no exceptions, so an RTE can only be a jump.
    write_sr(c, c.ssr);
    c.pc = c.spc;
    return false;
}

void unimplemented(Ctx& c, std::uint32_t raw, std::uint32_t pc) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "unimplemented instruction 0x%04x at 0x%08x", raw, pc);
    c.pc = pc;
    throw std::runtime_error(buf);
}

}  // namespace dream::sh4
