// AICA sound generation: the 64 channels (PCM/ADPCM/noise streaming with
// loops, amplitude and filter envelopes, LFOs, panning and send levels), the effects DSP and the
// final mix to 16-bit stereo at 44.1 kHz. A port of Flycast's sgc_if.cpp and dsp_interp.cpp
// (GPL-2.0; the DSP interpreter descends from the Audio Overload SDK, R. Belmont and
// R. Bannister). Integer fixed-point throughout, with lookup tables generated once
// (tools/aica/gen_tables.py), so the output is bit-identical on every host.
//
// The mixer works directly on the AICA register bytes (channel slots at 0x0000, common block at
// 0x2800, DSP data at 0x3000) and on sound RAM; the Aica class calls it on register writes, on
// reads of the monitor registers and once per sample.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace dream::aica {

class Mixer {
public:
    Mixer(std::uint8_t* regs, std::uint8_t* aram) noexcept;
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;
    void reset();

    // A write landed in channel `channel`'s 0x80-byte slot at byte offset `reg` (size 1 or 2).
    void channel_reg_written(unsigned channel, unsigned reg, unsigned size);
    // RBP/RBL (0x2804) changed; DSP programme memory (0x3400..0x3BFF) changed.
    void ring_buffer_written();
    void dsp_program_written();
    // The DSP's work registers as the CPUs see them at 0x4000-0x457F (TEMP, MEMS, MIXS: a low
    // byte word and a high 16-bit word each), mapped as Flycast's aica_mem.cpp does.
    std::uint32_t dsp_state_read(std::uint32_t offset, unsigned size) const;
    void dsp_state_write(std::uint32_t offset, std::uint32_t value, unsigned size);
    // Refreshes the monitor registers (MIBUF at 0x2808, EG/SGC/LP at 0x2810, CA at 0x2814) before
    // a read of `offset` (0x2808..0x2817).
    void common_reg_read(unsigned offset, bool byte);
    // Produces one stereo sample.
    void sample(std::int16_t& left, std::int16_t& right);

    std::uint64_t samples = 0;  // produced so far
    // Called on every key-on that starts a voice, with its channel, sample start address (SA) and
    // pitch (OCT, FNS); `samples` is the output position at that moment. Set by --keyon-trace.
    std::function<void(unsigned channel, std::uint32_t sa, unsigned oct, unsigned fns)> on_key_on;
    // Enhancement, off unless pointed at a true flag: instead of the hardware's hard
    // 16-bit clamp, peaks above -2.5 dBFS are rounded off smoothly, so a loud mix that clips
    // (crackles) on the console sounds clean. Read every sample; owned by the launcher.
    const bool* soft_clip = nullptr;
    // The CD-DA input: when set, called once per sample for the drive's audio, which reaches the
    // mix as the hardware's external inputs do: EXTS0/EXTS1 for the DSP, and effect slots 16 and
    // 17 (EFSDL/EFPAN) straight into the output, as in Flycast. Unset, both inputs are silent.
    std::function<void(std::int16_t& left, std::int16_t& right)> cdda;

    // ---- everything below is the ported model ----
    using SampleType = std::int32_t;
    enum EgState : unsigned { kAttack = 0, kDecay1 = 1, kDecay2 = 2, kRelease = 3 };
    enum LfoType : unsigned { kSawtooth = 0, kSquare = 1, kTriangle = 2, kRandom = 3 };
    enum PcmsType : unsigned { kPcm16 = 0, kPcm8 = 1, kAdpcm = 2, kAdpcmStream = 3, kNoise = 4 };

    union Fp22_10 {
        struct Parts {
            std::uint32_t fp : 10;
            std::uint32_t ip : 22;
        } p;
        std::uint32_t full;
    };

    struct DspState {
        std::int32_t TEMP[128];  // 24 bits
        std::int32_t MEMS[32];   // 24 bits
        std::int32_t MIXS[16];   // 20 bits
        std::uint32_t RBP, RBL, MDEC_CT;
        bool stopped, dirty;
        // Steps up to and including the program's last non-zero instruction: the
        // all-zero steps after it only set dsp_step's locals, which nothing reads after the loop.
        int steps;
        // The programme decoded once per change: dsp_step ran its bit-field
        // extraction for every step of every sample. Coefficients and MADRS stay live reads.
        struct Op {
            std::uint8_t tra, twa, ira, iwa, ewa, masa, ysel, shift;
            std::uint16_t flags;  // the kOp* bits below
        } ops[128];
    };
    enum : std::uint16_t {
        kOpNop = 1u << 0,  // all four words zero
        kOpTwt = 1u << 1, kOpXsel = 1u << 2, kOpIwt = 1u << 3, kOpEwt = 1u << 4,
        kOpAdrl = 1u << 5, kOpFrcl = 1u << 6, kOpYrl = 1u << 7, kOpNegb = 1u << 8,
        kOpZero = 1u << 9, kOpBsel = 1u << 10, kOpMwt = 1u << 11, kOpMrd = 1u << 12,
        kOpTable = 1u << 13, kOpAdreb = 1u << 14, kOpNxadr = 1u << 15,
    };

    struct Channel;  // defined in mixer.cpp

private:
    std::uint8_t* regs_;
    std::uint8_t* aram_;
    Channel* chans_;  // 64, allocated by the constructor
    DspState dsp_{};
    void dsp_step();
    void channels_init();
    void channels_free();
};

}  // namespace dream::aica
