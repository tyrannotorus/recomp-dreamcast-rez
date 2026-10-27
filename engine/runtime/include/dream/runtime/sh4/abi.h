// Functions emitted code calls into the runtime. The full runtime and the bare harness each
// provide an implementation; emitted translation units only see this header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "dream/runtime/memory.h"
#include "dream/runtime/sh4/ctx.h"

namespace dream::sh4 {

using GuestFn = void (*)(Ctx&, ::dream::Memory&);
// Resume entry: enters the function at a block start or call-return address inside it.
using ResumeFn = void (*)(Ctx&, ::dream::Memory&, std::uint32_t pc);

struct FunctionEntry {
    std::uint32_t address;
    GuestFn fn;
    // Overlay units: the instruction words the function was translated
    // from. The entry only resolves while guest memory at `address` still holds them, so several
    // translations of code the program copies to the same RAM address over time can coexist.
    const std::uint16_t* signature = nullptr;
    std::uint32_t signature_words = 0;
    std::uint32_t end = 0;      // one past the last instruction (0: no range, e.g. HLE hooks)
    ResumeFn resume = nullptr;  // non-local returns re-enter the function here
};

// A guest `rts` whose PR is not the address the function was called from: a setjmp/longjmp or a
// cooperative task switch (Katana's context restore). The host frames between the throw and
// run_guest belong to the abandoned guest context and are dropped; run_guest continues at `pc`.
struct NonLocalReturn {
    std::uint32_t pc;
};

// Registers a translation unit's function table; called from a static initialiser the emitter
// generates. Tables from several units are merged.
void register_functions(const FunctionEntry* entries, std::size_t count) noexcept;

// Looks the guest address up in the merged table. Returns nullptr when untranslated. Entries with
// a signature are only returned when `m` is given and the words match memory at `address`.
GuestFn find_function(std::uint32_t address, ::dream::Memory* m = nullptr) noexcept;

// Control transfers whose target is only known at run time.
void call_indirect(Ctx& c, ::dream::Memory& m, std::uint32_t target);

// Non-local returns. Emitted code calls nonlocal_return from an `rts` whose target is not its entry
// PR. It sets c.unwinding with the target in c.unwind_pc and returns; emitted code returns after
// every call while the flag is set, so the host frames unwind by plain returns.
// resume_at enters the translated function containing `pc` at that address (its entry when `pc`
// is one); with no such translation it hands the address to the hooks' miss handler.
// resume_miss is what a resume entry calls for an address it has no label for.
// run_guest runs the program from `entry` until control returns to the PR it was started with,
// re-entering translated code through resume_at after every non-local return.
DREAM_RARE_CALL void nonlocal_return(Ctx& c, ::dream::Memory& m, std::uint32_t pc);

// The entry points native code calls (call_indirect, deliver_irq, trapa, a translated function's
// plain NAME) turn a flag still set when the guest code returns into the NonLocalReturn exception.
// Emitted code calls the _in forms (and NAME__in), which leave the flag for it to see.
inline void rethrow_unwinding(Ctx& c) {
    if (c.unwinding) {
        c.unwinding = 0;
        throw NonLocalReturn{c.unwind_pc};
    }
}
void call_indirect_in(Ctx& c, ::dream::Memory& m, std::uint32_t target);
DREAM_RARE_CALL void deliver_irq_in(Ctx& c, ::dream::Memory& m);
void trapa_in(Ctx& c, ::dream::Memory& m, std::uint32_t imm);
void resume_at(Ctx& c, ::dream::Memory& m, std::uint32_t pc);
DREAM_RARE_CALL void resume_miss(Ctx& c, ::dream::Memory& m, std::uint32_t pc);
void run_guest(Ctx& c, ::dream::Memory& m, std::uint32_t entry);
// Same, starting from a function pointer.
void run_guest(Ctx& c, ::dream::Memory& m, GuestFn entry);

// Exceptions and interrupts. Emitted code calls deliver_irq when `c.cycles >= c.next_event`.
void deliver_irq(Ctx& c, ::dream::Memory& m);
void trapa(Ctx& c, ::dream::Memory& m, std::uint32_t imm);
// Returns true when the RTE closes a delivered exception and resumes the interrupted context (the
// emitted `return` then unwinds to the delivery point) and false when it is a jump: SR and PC are
// restored from SSR/SPC and emitted code continues through nonlocal_return(c.pc) (Katana's
// startup enters the first task this way). An RTE that closes the exception but resumes a
// different context (a task switch inside the handler) sets a non-local return to SPC from
// inside.
bool rte(Ctx& c, ::dream::Memory& m);

// The full runtime plugs its scheduler and exception model in here (runtime/system.h). Without
// hooks the bare harness behaviour applies: no interrupts, TRAPA/RTE and untranslated targets
// throw.
class Hooks {
public:
    virtual ~Hooks() = default;
    virtual void on_poll(Ctx& c, ::dream::Memory& m) = 0;
    virtual void on_trapa(Ctx& c, ::dream::Memory& m, std::uint32_t imm) = 0;
    virtual void on_rte(Ctx& c, ::dream::Memory& m) = 0;
    virtual void on_untranslated(Ctx& c, ::dream::Memory& m, std::uint32_t target) = 0;
    // A non-local return landed at `pc`, an address inside translated code with no resume label,
    // or inside untranslated code.
    virtual void on_resume(Ctx& c, ::dream::Memory& m, std::uint32_t pc) {
        on_untranslated(c, m, pc);
    }
    // True while an exception delivered by the runtime is on the C++ stack, so an RTE unwinds to
    // it. Otherwise RTE is a plain transfer to SPC with SR from SSR (Katana's startup enters the
    // program that way).
    virtual bool in_exception_frame() const noexcept { return true; }
};
void set_hooks(Hooks* hooks) noexcept;
Hooks* hooks() noexcept;

// An instruction the emitter does not lower (SH-4A only, illegal).
[[noreturn]] void unimplemented(Ctx& c, std::uint32_t raw, std::uint32_t pc);

}  // namespace dream::sh4
