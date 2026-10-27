// Native sound: the native driver (native_sound_driver.cpp) and synth (native_synth.cpp) in place
// of Rez's Manatee driver and the sound chip. The game touches the chip's registers only to hold
// and release the driver; that is answered here. Each output sample, the driver runs its share of
// time, then the synth renders.
#include "native_sound.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/system.h"
#include "native_sound_driver.h"
#include "native_synth.h"

namespace rez {

namespace {

using snd::u16;
using snd::u32;
using snd::u8;

dream::System* g_sys = nullptr;
std::unique_ptr<snd::Synth> g_synth;
std::unique_ptr<snd::Driver> g_driver;

// The AV cable the console reports, bits 8-9 of 0x2C00: always VGA (0). Shinobi's video setup
// (0x8C015340) picks 640x480 progressive at 60 Hz for a VGA cable before it looks at the broadcast
// setting, so Rez runs at 60 Hz on every disc and region.
constexpr u8 kCableVga = 0;

// The sound chip's register block: stored and read back, except the cable bits (an input). The ARM
// reset bit holds or releases the driver; the CD input's two output registers set the synth's.
class SoundRegs final : public dream::mem::MmioHandler {
public:
    SoundRegs() { regs_[0x2C00] = 1; }  // the driver starts held
    std::uint32_t read(std::uint32_t addr, unsigned size) override {
        catch_up();
        const u32 o = addr & 0x7FFFu;
        const auto byte = [&](u32 i) -> u32 {
            return i == 0x2C01 ? static_cast<u32>((regs_[i] & ~3u) | kCableVga) : regs_[i];
        };
        if (size == 1)
            return byte(o);
        return byte(o & ~1u) | byte((o & ~1u) + 1) << 8;
    }
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override {
        catch_up();
        const u32 o = addr & 0x7FFFu;
        const bool held = regs_[0x2C00] & 1;
        if (size == 1) {
            regs_[o] = static_cast<u8>(value);
        } else {
            regs_[o & ~1u] = static_cast<u8>(value);
            regs_[(o & ~1u) + 1] = static_cast<u8>(value >> 8);
        }
        if (!g_driver)
            return;
        if ((o & ~1u) == 0x2C00) {
            const bool hold = regs_[0x2C00] & 1;
            if (held && !hold)
                g_driver->release();
            else if (hold)
                g_driver->hold();
        } else if ((o & ~3u) == 0x2040 || (o & ~3u) == 0x2044) {
            const u32 r = static_cast<u32>(regs_[o & ~3u] | regs_[(o & ~3u) + 1] << 8);
            g_synth->set_output(16 + ((o & ~3u) - 0x2040) / 4, static_cast<u8>((r >> 8) & 15), static_cast<u8>(r & 31));
        }
    }

private:
    // Renders the samples due by the scheduler's present (lazy samples, see native_sound_start).
    static void catch_up() {
        if (g_sys && g_sys->aica)
            g_sys->aica->catch_up(g_sys->sched.now());
    }
    std::array<u8, 0x8000> regs_{};
};
SoundRegs g_regs;

}  // namespace

dream::mem::MmioHandler* native_sound_regs() {
    return &g_regs;
}

void native_sound_start(dream::System& sys) {
    g_sys = &sys;
    if (sys.aica) {
        g_synth = std::make_unique<snd::Synth>(sys.memory.aram());
        g_driver = std::make_unique<snd::Driver>(sys.memory.aram(), *g_synth);
        dream::aica::Aica& aica = *sys.aica;
        g_synth->soft_clip = aica.mixer.soft_clip;
        g_synth->external = aica.mixer.cdda;  // the disc's audio, when the native drive plays it
        g_synth->on_key_on = [&aica](unsigned v, u32 sa, unsigned oct, unsigned fns) {
            if (aica.mixer.on_key_on)
                aica.mixer.on_key_on(v, sa, oct, fns);
        };
        aica.source = [](std::int16_t& l, std::int16_t& r) {
            g_driver->sample();
            g_synth->render(l, r);
        };
        // Lazy samples (Aica::set_lazy): the driver and synth raise no SH-4 interrupt and are seen
        // only through sound RAM and the sound registers, so they catch up when the SH-4 looks:
        //  - before a sound-RAM access, up to the scheduler's present;
        //  - before a sound-register access, the same, after the device sync;
        //  - before a frame starts, the samples strictly before its line, so each key-on keeps
        //    its frame;
        //  - at the run's stop (native_sound_stop).
        sys.memory.on_aram_access = [] { g_sys->aica->catch_up(g_sys->sched.now()); };
        sys.spg.before_frame = [](std::uint64_t now) { g_sys->aica->catch_up(now, false); };
        aica.set_lazy(true);
        // The effects run on a sound thread.
        g_synth->start_thread();
        sys.memory.on_aram_write = [](u32 offset, unsigned size) { g_synth->ram_written(offset, size); };
    }
}

// At the run's stop, while the System is alive (PortInfo::on_stop): the samples up to the present.
void native_sound_stop() {
    if (g_driver && g_sys && g_sys->aica)
        g_sys->aica->catch_up(g_sys->sched.now());
}

}  // namespace rez
