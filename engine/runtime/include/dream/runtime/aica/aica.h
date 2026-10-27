// AICA sound processor, control half: the 32 KB register block at 0x00700000 as the SH-4 sees
// it, the three timers, the interrupt registers (MCIEB/MCIPD reach the SH-4 through Holly's
// external AICA line), the ARM reset register and the 44.1 kHz sample tick that steps the timers
// on the virtual clock. The sound CPU (ARM7) is not run: a port replaces the sound driver with
// native code (`source`). Modelled on Flycast's aica.cpp / aica_mem.cpp / aica_if.cpp (GPL-2.0).
// Channel and DSP registers are stored and read back; the mixer (mixer.h) generates the sound.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "dream/runtime/aica/mixer.h"
#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sched/scheduler.h"

namespace dream::aica {

class Aica final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kRegBase = 0x00700000u, kRegEnd = 0x00708000u;
    static constexpr std::uint64_t kSh4CyclesPerSample = 200'000'000 / 44100;  // 4535
    // Register offsets (16-bit registers on 32-bit slots)
    static constexpr std::uint32_t kTimerA = 0x2890, kTimerB = 0x2894, kTimerC = 0x2898,
                                   kScieb = 0x289C, kScipd = 0x28A0, kScire = 0x28A4,
                                   kMcieb = 0x28B4, kMcipd = 0x28B8, kMcire = 0x28BC,
                                   kArmRst = 0x2C00;
    // Interrupt bits shared by SCIEB/SCIPD/SCIRE and MCIEB/MCIPD/MCIRE
    static constexpr std::uint32_t kIntMidiIn = 1u << 3, kIntDmaEnd = 1u << 4, kIntScpu = 1u << 5,
                                   kIntTimerA = 1u << 6, kIntTimerB = 1u << 7, kIntTimerC = 1u << 8,
                                   kIntMidiOut = 1u << 9, kIntSampleDone = 1u << 10,
                                   kIntMask = 0x7FFu;

    Aica(sched::Scheduler& sched, holly::Intc& holly, mem::DcMemory& memory);
    void reset();

    // SH-4 side (mem::MmioHandler)
    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    // Advances one 44.1 kHz sample: timers step, SAMPLE_DONE is raised, the SH-4's interrupt is
    // re-evaluated and the mixer produces one stereo sample, handed to `on_sample` when set.
    // Scheduled on the virtual clock.
    void sample_tick();
    std::function<void(std::int16_t left, std::int16_t right)> on_sample;
    // A native replacement for the sound driver and the chip: when set, each tick's sample comes
    // from it and the timers, interrupt controllers and mixer stay idle.
    std::function<void(std::int16_t& left, std::int16_t& right)> source;
    // Lazy samples, for a `source` that raises no interrupt and that the SH-4 sees only through
    // sound RAM and the sound registers: one event every kLazyBlock samples instead of one per
    // sample. catch_up(limit) runs every sample due by `limit` (strictly before it unless
    // `inclusive`); call it before the SH-4 or a DMA touches sound RAM or the registers.
    void set_lazy(bool on);
    void catch_up(std::uint64_t limit, bool inclusive = true);
    bool lazy() const noexcept { return lazy_; }
    std::uint64_t next_due() const noexcept { return next_at_; }  // lazy: the next sample's time
    static constexpr std::uint64_t kLazyBlock = 1024;
    // Every register write, before the chip acts on it: offset in the 32 KB
    // block, value, size (1 or 2).
    std::function<void(std::uint32_t offset, std::uint32_t value, unsigned size)> on_reg_write;
    std::int16_t last_left = 0, last_right = 0;

    std::uint32_t reg16(std::uint32_t offset) const noexcept {
        return regs_[offset] | (static_cast<std::uint32_t>(regs_[offset + 1]) << 8);
    }
    bool arm_in_reset() const noexcept { return (regs_[kArmRst] & 1u) != 0; }

private:
    std::array<std::uint8_t, 0x8000> regs_{};  // before the mixer: it is built on it

public:
    Mixer mixer;
    std::uint64_t samples = 0;  // produced so far

private:
    std::uint32_t reg_read(std::uint32_t offset, unsigned size);
    void reg_write(std::uint32_t offset, std::uint32_t value, unsigned size);
    void store(std::uint32_t offset, std::uint32_t value, unsigned size);
    void set16(std::uint32_t offset, std::uint32_t value) noexcept;
    void timer_written(int i);
    void step_timers();
    void update_sh4_interrupts();

    sched::Scheduler& sched_;
    holly::Intc& holly_;
    std::array<int, 3> timer_step_{1, 1, 1}, timer_left_{1, 1, 1};
    int event_ = -1;
    bool lazy_ = false, catching_up_ = false;
    std::uint64_t next_at_ = 0;  // lazy: when the next sample is due
    void schedule_lazy();
};

}  // namespace dream::aica
