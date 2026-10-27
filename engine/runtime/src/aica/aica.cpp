// AICA control half; see aica.h. Register semantics follow Flycast (aica.cpp, aica_mem.cpp,
// aica_if.cpp; GPL-2.0).
#include "dream/runtime/aica/aica.h"

namespace dream::aica {

Aica::Aica(sched::Scheduler& sched, holly::Intc& holly, mem::DcMemory& memory)
    : mixer(regs_.data(), memory.aram()),
      sched_(sched),
      holly_(holly) {
    reset();
    event_ = sched_.add("aica.sample", [this](std::uint64_t now, std::uint64_t) {
        if (lazy_) {
            catch_up(now);
            schedule_lazy();
            return;
        }
        sample_tick();
        sched_.request(event_, kSh4CyclesPerSample);
    });
    sched_.request(event_, kSh4CyclesPerSample);
}

void Aica::set_lazy(bool on) {
    if (on == lazy_)
        return;
    if (on) {
        next_at_ = sched_.deadline(event_);  // the sample the event was armed for
        lazy_ = true;
        schedule_lazy();
    } else {
        catch_up(sched_.now());
        lazy_ = false;
        sched_.request(event_, next_at_ - sched_.now());
    }
}

// The event at the last of the next kLazyBlock samples' times.
void Aica::schedule_lazy() {
    sched_.request(event_, next_at_ + (kLazyBlock - 1) * kSh4CyclesPerSample - sched_.now());
}

void Aica::catch_up(std::uint64_t limit, bool inclusive) {
    if (!lazy_ || catching_up_)
        return;
    catching_up_ = true;
    while (inclusive ? next_at_ <= limit : next_at_ < limit) {
        sample_tick();
        next_at_ += kSh4CyclesPerSample;
    }
    catching_up_ = false;
}

void Aica::reset() {
    regs_.fill(0);
    regs_[kArmRst] = 1;  // the ARM starts held in reset; the BIOS or the title releases it
    timer_step_ = {1, 1, 1};
    timer_left_ = {1, 1, 1};
    mixer.reset();
}

void Aica::set16(std::uint32_t offset, std::uint32_t value) noexcept {
    regs_[offset] = static_cast<std::uint8_t>(value);
    regs_[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

// Registers are 16 bits wide on 32-bit slots: a 32-bit read returns the low half, as Flycast does.
std::uint32_t Aica::reg_read(std::uint32_t offset, unsigned size) {
    offset &= 0x7FFFu;
    if (offset >= 0x2808u && offset < 0x2818u)
        mixer.common_reg_read(offset, size == 1);
    if (offset >= 0x4000u && offset < 0x4580u)  // the DSP's work registers (Flycast aica_mem.cpp)
        return (offset & 2) ? 0u : mixer.dsp_state_read(offset, size);
    if (size == 1)
        return regs_[offset];
    const std::uint32_t o = offset & ~1u;
    return reg16(o);
}

void Aica::store(std::uint32_t offset, std::uint32_t value, unsigned size) {
    if (size == 1) {
        regs_[offset] = static_cast<std::uint8_t>(value);
        return;
    }
    set16(offset & ~1u, value & 0xFFFFu);
}

void Aica::reg_write(std::uint32_t offset, std::uint32_t value, unsigned size) {
    offset &= 0x7FFFu;
    // A 32-bit store writes the 16-bit register in the low half (Flycast does the same); the
    // mixer's channel handlers only distinguish byte from halfword, and drivers key voices on with
    // 32-bit STRs.
    if (size == 4)
        size = 2;
    if (on_reg_write)
        on_reg_write(offset, value, size);
    const std::uint32_t slot = offset & ~3u;
    if (offset < 0x2000u) {  // 64 channels x 0x80 bytes
        store(offset, value, size);
        mixer.channel_reg_written(offset >> 7, offset & 0x7Fu, size);
        return;
    }
    if (offset >= 0x3000u) {  // DSP coefficients, programme and data
        // As Flycast: writes to the upper halfword of a DSP register slot are dropped, and the
        // TEMP/MEMS/MIXS window goes to the DSP's own state, not the register image.
        if (offset & 2u)
            return;
        if (offset >= 0x4000u && offset < 0x4580u) {
            mixer.dsp_state_write(offset, value, size);
            return;
        }
        store(offset, value, size);
        if (offset >= 0x3400u && offset < 0x3C00u)
            mixer.dsp_program_written();
        return;
    }
    if (slot == 0x2804u) {  // RBP/RBL: the DSP ring buffer
        store(offset, value, size);
        mixer.ring_buffer_written();
        return;
    }
    // The interrupt and timer registers act only on a write to their own address (the low byte or
    // the whole halfword); a byte store to the high byte is just stored, as in Flycast.
    if (offset != slot && slot != kArmRst) {
        store(offset, value, size);
        return;
    }
    switch (slot) {
        case kScieb:
            set16(kScieb, value & kIntMask);
            return;
        case kScipd:  // only the CPU-to-ARM bit is writable
            if (value & kIntScpu) {
                set16(kScipd, reg16(kScipd) | kIntScpu);
            }
            return;
        case kScire:
            set16(kScipd, reg16(kScipd) & ~(value & kIntMask));
            return;
        case kMcieb:
            set16(kMcieb, value & kIntMask);
            update_sh4_interrupts();
            return;
        case kMcipd:
            if (value & kIntScpu) {
                set16(kMcipd, reg16(kMcipd) | kIntScpu);
                update_sh4_interrupts();
            }
            return;
        case kMcire:
            set16(kMcipd, reg16(kMcipd) & ~(value & kIntMask));
            update_sh4_interrupts();
            return;
        case kTimerA:
        case kTimerB:
        case kTimerC:
            store(offset, value, size);
            timer_written(static_cast<int>((slot - kTimerA) / 4));
            return;
        case 0x288C:  // DEXE/DDIR/DLG: the register-to-wave-memory DMA; not modelled
            store(offset, value, size);
            return;
        case kArmRst: {
            // Low byte ARMRST (bit 0 holds the core in reset), high byte VREG.
            if (size == 1) {
                regs_[offset] = static_cast<std::uint8_t>(value);
            } else {
                regs_[kArmRst] = static_cast<std::uint8_t>(value);
                regs_[kArmRst + 1] = static_cast<std::uint8_t>(value >> 8);
            }
            regs_[kArmRst] &= 1u;
            return;
        }
        default:
            store(offset, value, size);
            return;
    }
}

std::uint32_t Aica::read(std::uint32_t addr, unsigned size) {
    return reg_read(addr - kRegBase, size);
}

void Aica::write(std::uint32_t addr, std::uint32_t value, unsigned size) {
    reg_write(addr - kRegBase, value, size);
}

// Timer registers: bits 0-7 count up once per 2^md samples (bits 8-10) and interrupt on wrap.
void Aica::timer_written(int i) {
    const std::uint32_t r = reg16(kTimerA + 4u * static_cast<std::uint32_t>(i));
    const int step = 1 << ((r >> 8) & 7u);
    if (step != timer_step_[static_cast<std::size_t>(i)]) {
        timer_step_[static_cast<std::size_t>(i)] = step;
        timer_left_[static_cast<std::size_t>(i)] = step;
    }
}

void Aica::step_timers() {
    static constexpr std::uint32_t kBits[3] = {kIntTimerA, kIntTimerB, kIntTimerC};
    for (std::size_t i = 0; i < 3; ++i) {
        if (--timer_left_[i] != 0)
            continue;
        timer_left_[i] = timer_step_[i];
        const std::uint32_t off = kTimerA + 4u * static_cast<std::uint32_t>(i);
        const std::uint32_t r = reg16(off);
        const std::uint32_t count = (r + 1u) & 0xFFu;
        set16(off, (r & 0xFF00u) | count);
        if (count == 0) {
            set16(kScipd, reg16(kScipd) | kBits[i]);
            set16(kMcipd, reg16(kMcipd) | kBits[i]);
        }
    }
}

void Aica::update_sh4_interrupts() {
    const bool pending = (reg16(kMcieb) & reg16(kMcipd)) != 0;
    if (pending) {
        if (!holly_.raised(holly::Irq::AicaIrq)) {
            holly_.raise(holly::Irq::AicaIrq);
        }
    } else if (holly_.raised(holly::Irq::AicaIrq)) {
        holly_.clear(holly::Irq::AicaIrq);
    }
}

void Aica::sample_tick() {
    ++samples;
    if (source) {
        source(last_left, last_right);
        if (on_sample)
            on_sample(last_left, last_right);
        return;
    }
    step_timers();
    set16(kScipd, reg16(kScipd) | kIntSampleDone);
    set16(kMcipd, reg16(kMcipd) | kIntSampleDone);
    mixer.sample(last_left, last_right);
    if (on_sample)
        on_sample(last_left, last_right);
    update_sh4_interrupts();
}

}  // namespace dream::aica
