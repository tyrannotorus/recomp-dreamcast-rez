// The running Dreamcast: memory, virtual clock, the SH-4 interrupt model and the devices, wired
// into the ABI hooks emitted code calls. Interrupts are cooperative: emitted code polls
// `cycles >= next_event` at function entries and back-edges; on_poll runs the scheduler up to the
// guest clock, and if an interrupt is deliverable it performs the SH-4 entry sequence and runs the
// handler at VBR+0x600 as a nested guest call. The handler's RTE restores SR and returns to the
// poll site, which is where hardware would have resumed.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/spg.h"
#include "dream/runtime/sched/scheduler.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ctx.h"
#include "dream/runtime/sh4/intc.h"
#include "dream/runtime/sh4/tmu.h"

namespace dream {

namespace pvr {
class Core;
}
namespace gdrom {
class Disc;
}
namespace maple {
class Bus;
struct ControllerState;
}
namespace aica {
class Aica;
class Rtc;
}

// Thrown by the release configuration (no dev interpreter) when emitted code transfers control to
// an address with no translation. The fault log holds every miss recorded before it.
struct UntranslatedCall : std::runtime_error {
    UntranslatedCall(std::uint32_t target_, std::uint32_t from_, const std::string& what)
        : std::runtime_error(what), target(target_), from(from_) {}
    std::uint32_t target, from;
};

class System final : public sh4::Hooks {
public:
    System();
    ~System() override;

    // Katana reset state: SR 0x700000F0, FPSCR 0x00040001, VBR 0, clock at zero.
    void reset();
    // Installs this system as the ABI hook target (one system at a time).
    void install();

    void on_poll(sh4::Ctx& c, ::dream::Memory& m) override;
    void on_trapa(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t imm) override;
    void on_rte(sh4::Ctx& c, ::dream::Memory& m) override;
    void on_untranslated(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t target) override;
    void on_resume(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t pc) override;
    bool in_exception_frame() const noexcept override { return nesting > 0; }

    // Delivers every interrupt SR currently lets through, most urgent first, running handlers to
    // completion. Returns how many were delivered.
    unsigned deliver_pending(sh4::Ctx& c, ::dream::Memory& m);

    mem::DcMemory memory;
    sched::Scheduler sched;
    sh4::Intc intc;
    sh4::Tmu tmu;
    holly::Intc holly;
    pvr::Spg spg;
    sh4::Ctx ctx{};

    unsigned nesting = 0;  // exception frames entered and not yet left
    bool advancing_for_device = false;  // re-entrancy guard for the device-access clock advance
    // The interrupted pc and r15 of each exception frame: an RTE that resumes elsewhere is a task
    // switch inside the handler (the guest kernel resumed a different context).
    std::vector<std::uint32_t> frame_pcs, frame_sps;
    // The PVR the launcher owns, for port code that replaces a library at the device boundary and
    // hands over directly what the library would have sent to the hardware. Null until the
    // launcher sets it, and in tools without one.
    pvr::Core* pvr_core = nullptr;
    // Each port's pad snapshot as the launcher's input layer keeps it (SDL, a recording, --press),
    // for port code that reads input natively instead of through Maple. Null where no pad is.
    const maple::ControllerState* pad_state[4] = {};
    // The console clock the launcher keeps (seeded, ticked once a guest second), for port code that
    // answers the game's clock reads natively. Null until the launcher sets it.
    aica::Rtc* rtc = nullptr;
    // The launcher's Maple bus (for its attached devices: pads, memory cards, rumble packs) and
    // sound chip, for port code that drives them natively. Null until the launcher sets them.
    maple::Bus* maple = nullptr;
    aica::Aica* aica = nullptr;
    // The disc the launcher opened (with --mod overlays applied), for port code that reads the
    // game's files natively instead of through the GD-ROM interface. Null without a disc.
    gdrom::Disc* disc = nullptr;
    // The host's rumble for player `port` (0-3): strength 0..1 for `ms` milliseconds, 0 to stop.
    // Set by the launcher (a no-op headless); the launcher's vibration packs report through it, and
    // a port that implements the game's vibration library natively calls it directly.
    std::function<void(unsigned port, float strength, unsigned ms)> rumble;
    // Set by a port that serves the disc itself: the BIOS stand-in then refuses every GD-ROM
    // call, counted as gdrom.refused in the run report, so none can pass unnoticed.
    bool no_gdrom = false;
    // The memory guest code runs on, when a port puts a layer of its own in front of the memory
    // map (native code answering device registers). Set before the first guest
    // instruction (a port's on_start); the launcher then starts the guest on it, and every
    // re-entry after a task switch uses it too. Null: `memory`.
    ::dream::Memory* guest_memory = nullptr;

private:
    void enter_exception(sh4::Ctx& c, ::dream::Memory& m, std::uint32_t vector_offset);
    void arm_next_poll(sh4::Ctx& c) noexcept;
};

}  // namespace dream
