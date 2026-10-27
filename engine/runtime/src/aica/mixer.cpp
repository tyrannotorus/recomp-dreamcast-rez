// AICA channels, DSP and final mix; see mixer.h. Follows Flycast's sgc_if.cpp / dsp_interp.cpp
// closely (GPL-2.0) so behaviour can be compared line by line; the differences are the
// fixed tables (mixer_tables.inc), CDDA through a source hook (`cdda`), no MIDI/VMU-beep inputs,
// and members instead of globals.
#include "dream/runtime/aica/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dream::aica {

namespace {
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u16 = std::uint16_t;
using s16 = std::int16_t;
using u8 = std::uint8_t;
using s8 = std::int8_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using SampleType = Mixer::SampleType;

#include "mixer_tables.inc"

constexpr u32 kAramMask = 0x1FFFFFu;
constexpr int kEgStepBits = 16;
constexpr int kAegAttackShift = 16;
constexpr u32 kRandFactor = 0x41c64e6du, kRandTerm = 0x3039u;

// 255 -> mute. Converts send levels (DISDL, IMXL, EFSDL) to TL-compatible attenuations.
constexpr u32 kSendLevel[16] = {255,    14 << 3, 13 << 3, 12 << 3, 11 << 3, 10 << 3,
                                9 << 3, 8 << 3,  7 << 3,  6 << 3,  5 << 3,  4 << 3,
                                3 << 3, 2 << 3,  1 << 3,  0 << 3};
// x.8 and x.3 fixed point ADPCM tables
constexpr s32 kAdpcmQs[8] = {0x0e6, 0x0e6, 0x0e6, 0x0e6, 0x133, 0x199, 0x200, 0x266};
constexpr s32 kAdpcmScale[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
constexpr s32 kQTable[32] = {2048,  1536,  1024,  512,   0,     -256,  -512,  -768,
                             -1024, -1280, -1536, -1792, -2048, -2176, -2304, -2432,
                             -2560, -2688, -2816, -2944, -3072, -3136, -3200, -3264,
                             -3328, -3392, -3456, -3520, -3584, -3648, -3712, -3776};

template <typename T>
T fp_mul(T a, T b, int bits) {
    return static_cast<T>((a * b) >> bits);
}

void volume_pan(SampleType value, u32 vol, u32 pan, SampleType& outl, SampleType& outr) {
    const SampleType temp = fp_mul(value, kVolumeLut[vol & 15], 15);
    const SampleType sc = fp_mul(temp, kVolumeLut[0xF - (pan & 0xF)], 15);
    if (pan & 0x10) {
        outl += temp;
        outr += sc;
    } else {
        outl += sc;
        outr += temp;
    }
}

// Register images. All registers are 16 bits on 32-bit slots; bit fields are allocated from the
// least significant bit on every compiler that builds this project, as in Flycast.
#pragma pack(push, 1)
struct ChannelRegs {
    u32 SA_hi : 7, PCMS : 2, LPCTL : 1, SSCTL : 1, : 3, KYONB : 1, KYONEX : 1, : 16;  // +00
    u32 SA_low : 16, : 16;                                                            // +04
    u32 LSA : 16, : 16;                                                               // +08
    u32 LEA : 16, : 16;                                                               // +0C
    u32 AR : 5, : 1, D1R : 5, D2R : 5, : 16;                                          // +10
    u32 RR : 5, DL : 5, KRS : 4, LPSLNK : 1, : 1, : 16;                               // +14
    u32 FNS : 10, : 1, OCT : 4, : 1, : 16;                                            // +18
    u32 ALFOS : 3, ALFOWS : 2, PLFOS : 3, PLFOWS : 2, LFOF : 5, LFORE : 1, : 16;      // +1C
    u32 ISEL : 4, IMXL : 4, : 8, : 16;                                                // +20
    u32 DIPAN : 5, : 3, DISDL : 4, : 4, : 16;                                         // +24
    u32 Q : 5, LPOFF : 1, VOFF : 1, : 1, TL : 8, : 16;                                // +28
    u32 FLV0 : 13, : 3, : 16;                                                         // +2C
    u32 FLV1 : 13, : 3, : 16;                                                         // +30
    u32 FLV2 : 13, : 3, : 16;                                                         // +34
    u32 FLV3 : 13, : 3, : 16;                                                         // +38
    u32 FLV4 : 13, : 3, : 16;                                                         // +3C
    u32 FD1R : 5, : 3, FAR : 5, : 3, : 16;                                            // +40
    u32 FRR : 5, : 3, FD2R : 5, : 3, : 16;                                            // +44
};
static_assert(sizeof(ChannelRegs) == 0x48, "channel register image");

struct CommonRegs {
    u32 MVOL : 4, VER : 4, DAC18B : 1, MEM8MB : 1, : 5, Mono : 1, : 16;               // +00
    u32 RBP : 12, : 1, RBL : 2, TESTB0 : 1, : 16;                                     // +04
    u32 MIBUF : 8, MIEMP : 1, MIFUL : 1, MIOVF : 1, MOEMP : 1, MOFUL : 1, : 3, : 16;  // +08
    u32 MOBUF : 8, MSLC : 6, AFSEL : 1, : 1, : 16;                                    // +0C
    u32 EG : 13, SGC : 2, LP : 1, : 16;                                               // +10
    u32 CA : 16, : 16;                                                                // +14
};
static_assert(sizeof(CommonRegs) == 0x18, "common register image");

struct DspOutVol {
    u32 EFPAN : 5, : 3, EFSDL : 4, : 4, : 16;
};
static_assert(sizeof(DspOutVol) == 4, "dsp out vol image");

struct DspData {
    u32 COEF[128];      // +0x000 15:3
    u32 MADRS[64];      // +0x200 15:0
    u8 pad0[0x100];     // +0x300
    u32 MPRO[128 * 4];  // +0x400 15:0
    u8 pad1[0x400];     // +0xC00
    struct Pair {
        u32 l, h;
    } TEMP[128];    // +0x1000
    Pair MEMS[32];  // +0x1400
    Pair MIXS[16];  // +0x1500
    u32 EFREG[16];  // +0x1580 15:0
    u32 EXTS[2];    // +0x15C0 15:0
};
static_assert(sizeof(DspData) == 0x15C8, "dsp data image");
#pragma pack(pop)

SampleType decode_adpcm(u32 sample, s32 prev, s32& quant) {
    const s32 sign = 1 - 2 * static_cast<s32>(sample / 8);
    const u32 data = sample & 7;
    SampleType rv = (quant * kAdpcmScale[data]) >> 3;
    if (rv > 0x7FFF)
        rv = 0x7FFF;
    rv = sign * rv + prev;
    quant = (quant * kAdpcmQs[data]) >> 8;
    quant = std::clamp(quant, 127, 24576);
    return std::clamp(rv, -32768, 32767);
}

// DSP float format (Audio Overload SDK)
u16 dsp_pack(s32 val) {
    const int sign = (val >> 23) & 1;
    u32 temp = (static_cast<u32>(val) ^ (static_cast<u32>(val) << 1)) & 0xFFFFFFu;
    int exponent = 0;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000u)
            break;
        temp <<= 1;
        exponent += 1;
    }
    u32 uval = static_cast<u32>(val);
    if (exponent < 12)
        uval <<= exponent;
    else
        uval <<= 11;
    uval = (uval >> 11) & 0x7FFu;
    uval |= static_cast<u32>(sign) << 15;
    uval |= static_cast<u32>(exponent) << 11;
    return static_cast<u16>(uval);
}

s32 dsp_unpack(u16 val) {
    const int sign = (val >> 15) & 1;
    int exponent = (val >> 11) & 0xF;
    const int mantissa = val & 0x7FF;
    u32 uval = static_cast<u32>(mantissa) << 11;
    uval |= static_cast<u32>(sign) << 22;
    if (exponent > 11)
        exponent = 11;
    else
        uval ^= 1u << 22;
    uval |= static_cast<u32>(sign) << 23;
    s32 r = static_cast<s32>(uval << 8);
    r >>= 8;
    r >>= exponent;
    return r;
}
}  // namespace

// ---- Channel -----------------------------------------------------------------------------------

struct Mixer::Channel {
    Mixer* mixer = nullptr;
    ChannelRegs* ccd = nullptr;
    int number = 0;
    u32 SA = 0, CA = 0;
    Fp22_10 step{};
    u32 update_rate = 0;
    SampleType s0 = 0, s1 = 0;
    struct {
        u32 LSA = 0, LEA = 0;
        u8 looped = 0;
    } loop;
    struct {
        s32 last_quant = 127, loopstart_quant = 0;
        SampleType loopstart_prev_sample = 0;
        bool in_loop = false;
    } adpcm;
    u32 noise_state = 0;
    struct {
        u32 DLAtt = 0, DRAtt = 0, DSPAtt = 0;
        SampleType* DSPOut = nullptr;
    } VolMix;
    struct {
        s32 val = 0;
        EgState state = kAttack;
        u32 AttackRate = 0, Decay1Rate = 0, Decay2Value = 0, Decay2Rate = 0, ReleaseRate = 0;
        s32 value() const noexcept { return val >> kEgStepBits; }
        void set(u32 v) noexcept { val = static_cast<s32>(v << kEgStepBits); }
    } AEG;
    struct {
        u32 value = 0;
        EgState state = kAttack;
        SampleType prev1 = 0, prev2 = 0;
        s32 fractSave = 0, q = 0;
        u32 AttackRate = 0, Decay1Rate = 0, Decay2Rate = 0, ReleaseRate = 0;
        bool active = false;
        u32 get() const noexcept { return value >> kEgStepBits; }
        void set(u32 v) noexcept { value = v << kEgStepBits; }
    } FEG;
    struct Lfo {
        u32 counter = 0, start_value = 0;
        u8 state = 0, alfo = 0, alfo_shft = 8;
        Fp22_10 plfo_step{};
        const int* plfo_scale = kPlfoScales[0];
        LfoType alfoType = kSawtooth, plfoType = kSawtooth;
        void calc_alfo() noexcept {
            u32 rv = 0;
            switch (alfoType) {
                case kSawtooth:
                    rv = state;
                    break;
                case kSquare:
                    rv = (state & 0x80) ? 255 : 0;
                    break;
                case kTriangle:
                    rv = ((state & 0x7f) ^ ((state & 0x80) ? 0x7F : 0)) << 1;
                    break;
                case kRandom:
                    rv = (state * kRandFactor + kRandTerm) & 0xff;
                    break;
            }
            alfo = static_cast<u8>(rv >> alfo_shft);
        }
        void calc_plfo() noexcept {
            u32 rv = 0;
            switch (plfoType) {
                case kSawtooth:
                    rv = state;
                    break;
                case kSquare:
                    rv = (state & 0x80) ? 0xff : 0;
                    break;
                case kTriangle:
                    rv = ((state & 0x7f) ^ ((state & 0x80) ? 0x7F : 0)) << 1;
                    break;
                case kRandom:
                    rv = (state * kRandFactor + kRandTerm) & 0xff;
                    break;
            }
            plfo_step.full = static_cast<u32>(plfo_scale[static_cast<u8>(rv)]);
        }
        void step_lfo() noexcept {
            counter--;
            if (counter == 0) {
                state++;
                counter = start_value;
                calc_alfo();
                calc_plfo();
            } else {
                // Random a/plfo does not depend on the frequency (LFOF)
                if (alfoType == kRandom)
                    calc_alfo();
                if (plfoType == kRandom)
                    calc_plfo();
            }
        }
        void reset() noexcept {
            state = 0;
            counter = start_value;
            calc_alfo();
            calc_plfo();
        }
    } lfo;
    bool enabled = false;
    bool quiet = false;

    // ---- streaming ----
    template <PcmsType PCMS>
    SampleType read_sample(u32 addr) const noexcept {
        const u8* ram = mixer->aram_;
        if (PCMS == kPcm16) {
            s16 v;
            std::memcpy(&v, ram + (addr & kAramMask & ~1u), 2);
            return v;
        }
        return static_cast<s8>(ram[addr & kAramMask]) << 8;
    }
    u8 read_nibble_byte(u32 addr) const noexcept { return mixer->aram_[addr & kAramMask]; }

    template <PcmsType PCMS, bool Last>
    void step_decode_sample(u32 ca) {
        // Only the ADPCM formats need the intermediate steps of a run; the others decode once, at
        // the end. `if constexpr` with an `else` discards the body entirely for those
        // instantiations: an early `return` would leave it compiled but unreachable, which MSVC
        // rejects under /W4 /WX.
        if constexpr (!Last && (PCMS == kPcm16 || PCMS == kPcm8 || PCMS == kNoise)) {
            (void)ca;
        } else {
            u32 next_addr = ca + 1;
            // ADPCM long-stream mode ends on a 4-sample boundary (Flycast's alignedLEA).
            const u32 lea = PCMS == kAdpcmStream ? ((loop.LEA + 3) & ~3u) : loop.LEA;
            if (next_addr >= lea && lea > loop.LSA)
                next_addr = loop.LSA;
            // `switch (PCMS)` on a template constant leaves every other arm as dead code, which
            // MSVC rejects under /W4 /WX; `if constexpr` says the same thing and compiles only the
            // arm in use.
            SampleType ns0 = 0, ns1 = 0;
            if constexpr (PCMS == kNoise) {
                noise_state = noise_state * kRandFactor + kRandTerm;
                ns0 = static_cast<s32>(noise_state) >> 16;
                noise_state = noise_state * kRandFactor + kRandTerm;
                ns1 = static_cast<s32>(noise_state) >> 16;
            } else if constexpr (PCMS == kPcm16) {
                ns0 = read_sample<kPcm16>(SA + ca * 2);
                ns1 = read_sample<kPcm16>(SA + next_addr * 2);
            } else if constexpr (PCMS == kPcm8) {
                ns0 = read_sample<kPcm8>(SA + ca);
                ns1 = read_sample<kPcm8>(SA + next_addr);
            } else {  // kAdpcm and kAdpcmStream
                u8 ad1 = read_nibble_byte(SA + (ca >> 1));
                u8 ad2 = read_nibble_byte(SA + (next_addr >> 1));
                ad1 = static_cast<u8>((ad1 >> ((ca & 1) * 4)) & 0xF);
                ad2 = static_cast<u8>((ad2 >> ((next_addr & 1) * 4)) & 0xF);
                s32 q = adpcm.last_quant;
                if constexpr (PCMS == kAdpcm) {
                    if (ca == loop.LSA) {
                        if (!adpcm.in_loop) {
                            adpcm.in_loop = true;
                            adpcm.loopstart_quant = q;
                            adpcm.loopstart_prev_sample = s0;
                        } else {
                            q = adpcm.loopstart_quant;
                            s0 = adpcm.loopstart_prev_sample;
                        }
                    }
                }
                ns0 = decode_adpcm(ad1, s0, q);
                adpcm.last_quant = q;
                if constexpr (Last) {
                    SampleType prev = ns0;
                    if constexpr (PCMS == kAdpcm) {
                        if (next_addr == loop.LSA && adpcm.in_loop) {
                            q = adpcm.loopstart_quant;
                            prev = adpcm.loopstart_prev_sample;
                        }
                    }
                    ns1 = decode_adpcm(ad2, prev, q);
                } else {
                    ns1 = 0;
                }
            }
            s0 = ns0;
            s1 = ns1;
        }
    }

    template <PcmsType PCMS, bool LPCTL, bool LPSLNK>
    void stream_step() {
        step.full += fp_mul(update_rate, lfo.plfo_step.full, 10);
        Fp22_10 sp = step;
        step.p.ip = 0;
        while (sp.p.ip > 0) {
            sp.p.ip--;
            u32 ca = CA + 1;
            // LEA/LSA are meant to be 4-sample aligned in stream mode: the end is the aligned LEA.
            const u32 lea = PCMS == kAdpcmStream ? ((loop.LEA + 3) & ~3u) : loop.LEA;
            if constexpr (LPSLNK) {
                if (AEG.state == kAttack && ca >= loop.LSA)
                    set_aeg_state(kDecay1);
            }
            if (ca >= lea) {
                if (loop.LSA > lea) {
                    // LSA > LEA: play on until LSA, then reset CA and stop, looping or not.
                    if (ca >= loop.LSA) {
                        loop.looped = 1;
                        ca = 0;
                        disable();
                    }
                } else {
                    loop.looped = 1;
                    if constexpr (!LPCTL) {
                        ca = 0;
                        disable();
                    } else {
                        ca = loop.LSA;
                    }
                }
            }
            CA = ca;
            if (sp.p.ip == 0)
                step_decode_sample<PCMS, true>(ca);
            else
                step_decode_sample<PCMS, false>(ca);
        }
    }

    using StepFn = void (Channel::*)();
    StepFn step_stream = nullptr, step_stream_initial = nullptr;

    template <PcmsType PCMS>
    void step_decode_initial() {
        step_decode_sample<PCMS, true>(0);
    }

    template <PcmsType PCMS>
    static StepFn stream_fn(bool lpctl, bool lpslnk) {
        if (lpctl)
            return lpslnk ? &Channel::stream_step<PCMS, true, true>
                          : &Channel::stream_step<PCMS, true, false>;
        return lpslnk ? &Channel::stream_step<PCMS, false, true>
                      : &Channel::stream_step<PCMS, false, false>;
    }
    static StepFn stream_fn_for(PcmsType fmt, bool lpctl, bool lpslnk) {
        switch (fmt) {
            case kPcm16:
                return stream_fn<kPcm16>(lpctl, lpslnk);
            case kPcm8:
                return stream_fn<kPcm8>(lpctl, lpslnk);
            case kAdpcm:
                return stream_fn<kAdpcm>(lpctl, lpslnk);
            case kAdpcmStream:
                return stream_fn<kAdpcmStream>(lpctl, lpslnk);
            default:
                return stream_fn<kNoise>(lpctl, lpslnk);
        }
    }
    static StepFn initial_fn_for(PcmsType fmt) {
        switch (fmt) {
            case kPcm16:
                return &Channel::step_decode_initial<kPcm16>;
            case kPcm8:
                return &Channel::step_decode_initial<kPcm8>;
            case kAdpcm:
                return &Channel::step_decode_initial<kAdpcm>;
            case kAdpcmStream:
                return &Channel::step_decode_initial<kAdpcmStream>;
            default:
                return &Channel::step_decode_initial<kNoise>;
        }
    }

    // ---- envelopes ----
    void aeg_step() {
        switch (AEG.state) {
            case kAttack:
                if (AEG.AttackRate != 0) {
                    AEG.val -= static_cast<s32>(
                        ((static_cast<u64>(static_cast<u32>(AEG.val)) << kAegAttackShift) /
                         AEG.AttackRate) +
                        1);
                    if (AEG.value() <= 0) {
                        if (!ccd->LPSLNK)
                            set_aeg_state(kDecay1);
                        AEG.set(0);
                    }
                }
                break;
            case kDecay1:
                AEG.val += static_cast<s32>(AEG.Decay1Rate);
                if (static_cast<u32>(AEG.value()) >= AEG.Decay2Value)
                    set_aeg_state(kDecay2);
                break;
            case kDecay2:
                AEG.val += static_cast<s32>(AEG.Decay2Rate);
                if (AEG.value() >= 0x3FF) {
                    AEG.set(0x3FF);
                    set_aeg_state(kRelease);
                }
                break;
            case kRelease:
                AEG.val += static_cast<s32>(AEG.ReleaseRate);
                if (AEG.value() >= 0x3FF)
                    disable();
                break;
        }
    }

    void feg_step() {
        if (!FEG.active)
            return;
        u32 delta = 0, target = 0;
        switch (FEG.state) {
            case kAttack:
                delta = FEG.AttackRate;
                target = ccd->FLV1;
                break;
            case kDecay1:
                delta = FEG.Decay1Rate;
                target = ccd->FLV2;
                break;
            case kDecay2:
                delta = FEG.Decay2Rate;
                target = ccd->FLV3;
                break;
            case kRelease:
                delta = FEG.ReleaseRate;
                target = ccd->FLV4;
                break;
        }
        target <<= kEgStepBits;
        if (FEG.value < target) {
            const u32 maxd = target - FEG.value;
            FEG.value += std::min(delta, maxd);
        } else if (FEG.value > target) {
            const u32 maxd = FEG.value - target;
            FEG.value -= std::min(delta, maxd);
        } else if (FEG.state < kDecay2) {
            set_feg_state(static_cast<EgState>(static_cast<unsigned>(FEG.state) + 1));
        }
    }

    // ---- state ----
    void init(Mixer* m, int cn, u8* regs) {
        mixer = m;
        ccd = reinterpret_cast<ChannelRegs*>(regs + static_cast<std::size_t>(cn) * 0x80);
        number = cn;
        quiet = true;
        for (u32 i = 0; i < 0x80; i += 2) reg_write(i, 2);
        quiet = false;
        disable();
    }
    void disable() {
        enabled = false;
        set_aeg_state(kRelease);
        AEG.set(0x3FF);
        CA = 0;
    }
    SampleType interpolate() const noexcept {
        const SampleType fp = static_cast<SampleType>(step.p.fp);
        return fp_mul(s0, 1024 - fp, 10) + fp_mul(s1, fp, 10);
    }
    SampleType low_pass(SampleType sample) {
        if (!FEG.active)
            return sample;
        constexpr u32 kCoefBits = 30;
        const u32 fv = FEG.get();
        const u32 exp = fv >> 9;
        const u32 mant = (fv & 0x1FF) | 0x200;
        u64 a0 = (static_cast<u64>(mant) << 30) >> ((15 - exp) * 2);
        a0 *= (mant - 1) / 8;
        a0 >>= (47 - kCoefBits);
        s64 f = (static_cast<s64>(mant) << exp) << (kCoefBits - 25);
        f += static_cast<s64>(FEG.q) * f / 4096;
        const s64 b1 = 128ll * 1024 * (1 << (kCoefBits - 16)) - (f + static_cast<s64>(a0));
        const s64 b2 = 64ll * 1024 * (1 << (kCoefBits - 16)) - f;
        if (exp == 0)
            FEG.fractSave = 0;  // avoid a residual signal
        const s64 mac =
            -static_cast<s64>(a0) * sample + b1 * FEG.prev1 - b2 * FEG.prev2 - FEG.fractSave;
        sample = static_cast<SampleType>(mac >> kCoefBits);
        FEG.fractSave = static_cast<s32>((static_cast<s64>(sample) << kCoefBits) - mac);
        FEG.prev2 = FEG.prev1;
        sample = std::clamp(sample, -512 * 1024, 512 * 1024 - 1);
        FEG.prev1 = sample;
        return sample;
    }
    bool step_channel(SampleType& oLeft, SampleType& oRight, SampleType& oDsp) {
        if (!enabled) {
            oLeft = oRight = oDsp = 0;
            return false;
        }
        SampleType sample = interpolate() << 4;
        sample = low_pass(sample);
        // Attenuations add up, then are applied through the log table (>= 255 mutes).
        u32 ofsatt;
        if (ccd->VOFF == 1) {
            ofsatt = 0;
        } else {
            ofsatt = lfo.alfo + static_cast<u32>(AEG.value() >> 2);
            ofsatt = std::min(ofsatt, 255u);
        }
        const u32 max_att = ((16u << 4) - 1) - ofsatt;
        const s32* logtable = kTlLut + ofsatt;
        const u32 dl = std::min(VolMix.DLAtt, max_att);
        const u32 dr = std::min(VolMix.DRAtt, max_att);
        const u32 ds = std::min(VolMix.DSPAtt, max_att);
        oLeft = static_cast<SampleType>(fp_mul<s64>(sample, logtable[dl], 19));  // 16 bits
        oRight = static_cast<SampleType>(fp_mul<s64>(sample, logtable[dr], 19));
        oDsp = static_cast<SampleType>(fp_mul<s64>(sample, logtable[ds], 15));  // 20 bits
        aeg_step();
        if (enabled) {
            feg_step();
            (this->*step_stream)();
            lfo.step_lfo();
        }
        return true;
    }
    void step_mix(SampleType& mixl, SampleType& mixr) {
        SampleType oLeft, oRight, oDsp;
        step_channel(oLeft, oRight, oDsp);
        *VolMix.DSPOut += oDsp;
        mixl += oLeft;
        mixr += oRight;
    }
    void set_aeg_state(EgState s) {
        AEG.state = s;
        if (s == kRelease)
            ccd->KYONB = 0;
    }
    void set_feg_state(EgState s) {
        FEG.state = s;
        if (s == kAttack) {
            FEG.set(ccd->FLV0);
            FEG.prev1 = FEG.prev2 = 0;
            FEG.fractSave = 0;
        }
    }
    void key_on() {
        if (AEG.state != kRelease)
            return;
        enabled = true;
        set_aeg_state(kAttack);
        AEG.set(0x280);  // start value taken from hardware traces (Flycast)
        set_feg_state(kAttack);
        CA = 0;
        step.full = 0;
        loop.looped = 0;
        adpcm.last_quant = 127;
        adpcm.loopstart_quant = 0;
        adpcm.loopstart_prev_sample = 0;
        adpcm.in_loop = false;
        s0 = 0;
        (this->*step_stream_initial)();
        if (mixer->on_key_on)
            mixer->on_key_on(static_cast<unsigned>(number), SA, ccd->OCT, ccd->FNS);
    }
    void key_off() {
        if (AEG.state == kRelease)
            return;
        set_aeg_state(kRelease);
        set_feg_state(kRelease);
    }
    void update_stream_step() {
        PcmsType fmt = static_cast<PcmsType>(ccd->PCMS);
        if (ccd->SSCTL)
            fmt = kNoise;
        step_stream = stream_fn_for(fmt, ccd->LPCTL != 0, ccd->LPSLNK != 0);
        step_stream_initial = initial_fn_for(fmt);
    }
    void update_sa() {
        SA = (ccd->SA_hi << 16) | ccd->SA_low;
        if (ccd->PCMS == kPcm16)
            SA &= ~1u;
    }
    void update_loop() {
        loop.LSA = ccd->LSA;
        loop.LEA = ccd->LEA;
    }
    u32 eg_eff_rate(u32 rate) const noexcept {
        u32 effrate = rate * 2;
        if (ccd->KRS < 0xF) {
            effrate += (ccd->FNS >> 9) & 1;
            effrate += static_cast<u32>(std::max<int>(
                0, (static_cast<int>(ccd->KRS) + (static_cast<int>(ccd->OCT) ^ 8) - 8) * 2));
        }
        return std::min(effrate, 0x3fu);
    }
    void update_aeg() {
        AEG.AttackRate = kAegAttackSps[eg_eff_rate(ccd->AR)];
        AEG.Decay1Rate = kAegDsrSps[eg_eff_rate(ccd->D1R)];
        AEG.Decay2Value = ccd->DL << 5;
        AEG.Decay2Rate = kAegDsrSps[eg_eff_rate(ccd->D2R)];
        AEG.ReleaseRate = kAegDsrSps[eg_eff_rate(ccd->RR)];
    }
    void update_pitch() {
        const u32 oct = ccd->OCT;
        u32 rate = 1024 | ccd->FNS;
        if (oct & 8)
            rate >>= 16 - oct;
        else
            rate <<= oct;
        update_rate = rate;
    }
    void update_lfo(bool derived_state) {
        lfo.alfoType = static_cast<LfoType>(ccd->ALFOWS);
        lfo.plfoType = static_cast<LfoType>(ccd->PLFOWS);
        if (lfo.alfoType == kRandom && lfo.plfoType == kRandom) {
            lfo.start_value = 1;  // LFOF is ignored when both are random
            lfo.counter = 1;
        } else {
            const int N = static_cast<int>(ccd->LFOF);
            const int S = N >> 2;
            const int M = (~N) & 3;
            const int G = 128 >> S;
            const int L = (G - 1) << 2;
            const int O = L + G * (M + 1);
            lfo.start_value = static_cast<u32>(O);
            if (!derived_state)
                lfo.counter = static_cast<u32>(O);
        }
        lfo.alfo_shft = static_cast<u8>(8 - ccd->ALFOS);
        lfo.plfo_scale = kPlfoScales[ccd->PLFOS];
        if (ccd->LFORE && !derived_state) {
            lfo.reset();
        } else {
            lfo.calc_alfo();
            lfo.calc_plfo();
        }
    }
    void update_dsp_mix() { VolMix.DSPOut = &mixer->dsp_.MIXS[ccd->ISEL]; }
    void update_atts() {
        const u32 total_level = ccd->VOFF ? 0 : ccd->TL;
        const u32 attFull = total_level + kSendLevel[ccd->DISDL];
        const u32 attPan = attFull + kSendLevel[(~ccd->DIPAN) & 0xF];
        if (ccd->DIPAN & 0x10) {  // 0x1*: right decreases
            VolMix.DLAtt = attFull;
            VolMix.DRAtt = attPan;
        } else {  // 0x0*: left decreases
            VolMix.DLAtt = attPan;
            VolMix.DRAtt = attFull;
        }
        VolMix.DSPAtt = total_level + kSendLevel[ccd->IMXL];
    }
    void update_feg() {
        FEG.active =
            ccd->LPOFF == 0 && (ccd->FLV0 < 0x1ff8 || ccd->FLV1 < 0x1ff8 || ccd->FLV2 < 0x1ff8 ||
                                ccd->FLV3 < 0x1ff8 || ccd->FLV4 < 0x1ff8 || ccd->Q != 4);
        if (!FEG.active)
            return;
        FEG.q = kQTable[ccd->Q];
        FEG.AttackRate = kAegDsrSps[eg_eff_rate(ccd->FAR)];  // FEG_SPS == AEG_DSR_SPS in Flycast
        FEG.Decay1Rate = kAegDsrSps[eg_eff_rate(ccd->FD1R)];
        FEG.Decay2Rate = kAegDsrSps[eg_eff_rate(ccd->FD2R)];
        FEG.ReleaseRate = kAegDsrSps[eg_eff_rate(ccd->FRR)];
    }
    void reg_write(u32 offset, unsigned size) {
        switch (offset) {
            case 0x00:  // PCMS, SA
            case 0x01:  // KYONEX, KYONB, SSCTL, LPCTL, PCMS
                update_stream_step();
                if (offset == 0 || size == 2)
                    update_sa();
                if ((offset == 1 || size == 2) && ccd->KYONEX) {
                    ccd->KYONEX = 0;
                    for (unsigned i = 0; i < 64; ++i) {
                        Channel& ch = mixer->chans_[i];
                        if (ch.ccd->KYONB)
                            ch.key_on();
                        else
                            ch.key_off();
                    }
                }
                break;
            case 0x04:
            case 0x05:
                update_sa();
                break;
            case 0x08:
            case 0x09:
            case 0x0C:
            case 0x0D:
                update_loop();
                break;
            case 0x10:
            case 0x11:
                update_aeg();
                break;
            case 0x14:
            case 0x15:
                update_stream_step();
                update_aeg();
                break;
            case 0x18:
            case 0x19:
                update_pitch();
                update_aeg();
                update_feg();
                break;
            case 0x1C:
            case 0x1D:
                update_lfo(false);
                break;
            case 0x20:
                update_dsp_mix();
                update_atts();
                break;
            case 0x24:
            case 0x25:
                update_atts();
                break;
            case 0x28:
            case 0x29:
                if (size == 2 || offset == 0x28)
                    update_feg();
                update_atts();
                break;
            case 0x2C:
            case 0x2D:
            case 0x30:
            case 0x31:
            case 0x34:
            case 0x35:
            case 0x38:
            case 0x39:
            case 0x3C:
            case 0x3D:
            case 0x40:
            case 0x41:
            case 0x44:
            case 0x45:
                update_feg();
                break;
            default:
                break;
        }
    }
};

// ---- Mixer -------------------------------------------------------------------------------------

Mixer::Mixer(std::uint8_t* regs, std::uint8_t* aram) noexcept
    : regs_(regs), aram_(aram), chans_(nullptr) {
    chans_ = new Channel[64];
    reset();
}

void Mixer::channels_init() {
    for (int i = 0; i < 64; ++i) chans_[i].init(this, i, regs_);
}

void Mixer::channels_free() {
    delete[] chans_;
    chans_ = nullptr;
}

Mixer::~Mixer() {
    channels_free();
}

void Mixer::reset() {
    std::memset(&dsp_, 0, sizeof dsp_);
    dsp_.RBL = 0x8000 - 1;
    dsp_.RBP = 0;
    dsp_.MDEC_CT = 1;
    dsp_.dirty = true;
    dsp_.stopped = true;
    for (int i = 0; i < 64; ++i) chans_[i] = Channel{};
    channels_init();
    samples = 0;
}

void Mixer::channel_reg_written(unsigned channel, unsigned reg, unsigned size) {
    chans_[channel & 63].reg_write(reg & 0x7F, size);
}

void Mixer::ring_buffer_written() {
    const CommonRegs* common = reinterpret_cast<const CommonRegs*>(regs_ + 0x2800);
    dsp_.RBL = (8192u << common->RBL) - 1;
    dsp_.RBP = (common->RBP * 2048u) & kAramMask;
}

std::uint32_t Mixer::dsp_state_read(std::uint32_t addr, unsigned size) const {
    u32 v;
    if (addr < 0x4500) {
        v = static_cast<u32>(addr < 0x4400 ? dsp_.TEMP[(addr - 0x4000) / 8]
                                           : dsp_.MEMS[(addr - 0x4400) / 8]);
        v = (addr & 4) ? (v >> 8) & 0xFFFFu : v & 0xFFu;
    } else {
        v = static_cast<u32>(dsp_.MIXS[(addr - 0x4500) / 8]);
        v = (addr & 4) ? (v >> 4) & 0xFFFFu : v & 0xFu;
    }
    if (size == 1)
        v = (addr & 1) ? v >> 8 : v & 0xFFu;
    return v;
}

void Mixer::dsp_state_write(std::uint32_t addr, std::uint32_t data, unsigned size) {
    if (addr < 0x4500) {
        s32& v = addr < 0x4400 ? dsp_.TEMP[(addr - 0x4000) / 8] : dsp_.MEMS[(addr - 0x4400) / 8];
        if (addr & 4) {
            if (size == 1) {
                if (addr & 1)
                    v = (v & 0x0000FFFF) | ((static_cast<s32>(data << 24)) >> 8);
                else
                    v = static_cast<s32>((static_cast<u32>(v) & 0xFFFF00FFu) | ((data & 0xFFu) << 8));
            } else {
                v = (v & 0xFF) | ((static_cast<s32>(data << 16)) >> 8);
            }
        } else if (size != 1 || (addr & 1) == 0) {
            v = static_cast<s32>((static_cast<u32>(v) & ~0xFFu) | (data & 0xFFu));
        }
    } else {
        s32& v = dsp_.MIXS[(addr - 0x4500) / 8];
        if (addr & 4) {
            if (size == 1) {
                if (addr & 1)
                    v = (v & 0x00000FFF) | ((static_cast<s32>(data << 24)) >> 12);
                else
                    v = static_cast<s32>((static_cast<u32>(v) & 0xFFFFF00Fu) | ((data & 0xFFu) << 4));
            } else {
                v = (v & 0xF) | ((static_cast<s32>(data << 16)) >> 12);
            }
        } else if (size != 1 || (addr & 1) == 0) {
            v = static_cast<s32>((static_cast<u32>(v) & ~0xFu) | (data & 0xFu));
        }
    }
}

void Mixer::dsp_program_written() {
    dsp_.dirty = true;
}

void Mixer::common_reg_read(unsigned offset, bool byte) {
    CommonRegs* common = reinterpret_cast<CommonRegs*>(regs_ + 0x2800);
    switch (offset) {
        case 0x2808:
        case 0x2809:  // MIDI input FIFO: nothing is ever received
            common->MIEMP = 1;
            common->MIFUL = 0;
            common->MIOVF = 0;
            common->MOEMP = 1;
            common->MOFUL = 0;
            break;
        case 0x2810:
        case 0x2811: {  // EG, SGC, LP of the monitored slot
            Channel& ch = chans_[common->MSLC];
            common->LP = ch.loop.looped ? 1u : 0u;
            const s32 aeg = ch.AEG.value();
            // EG is 13 bits (the filter envelope's width); the amplitude envelope is 10, and a
            // value past its knee reads as full scale.
            common->EG = aeg > 0x3BF ? 0x1FFFu : (static_cast<u32>(aeg) & 0x1FFFu);
            common->SGC = static_cast<u32>(ch.AEG.state) & 3u;
            if (!byte || offset == 0x2811)
                ch.loop.looped = 0;
            break;
        }
        case 0x2814:
        case 0x2815: {  // CA of the monitored slot
            const Channel& ch = chans_[common->MSLC];
            u32 ca = ch.CA;
            // Titles that poll CA against LEA to detect the end of a sound need CA to stay a
            // few samples short of it (Flycast's observation on hardware).
            if (ch.loop.LEA > ch.loop.LSA && ch.loop.LEA > 3)
                ca = std::min(ca, ch.loop.LEA - 3);
            common->CA = ca & 0xFFFFu;
            break;
        }
        default:
            break;
    }
}

void Mixer::dsp_step() {
    DspData* dsp = reinterpret_cast<DspData*>(regs_ + 0x3000);
    if (dsp_.dirty) {
        dsp_.dirty = false;
        dsp_.stopped = true;
        dsp_.steps = 0;
        for (int step = 0; step < 128; ++step) {
            const u32* p = dsp->MPRO + step * 4;
            DspState::Op& op = dsp_.ops[step];
            op = DspState::Op{};
            if (!(p[0] | p[1] | p[2] | p[3])) {
                op.flags = kOpNop;
                continue;
            }
            dsp_.stopped = false;
            dsp_.steps = step + 1;
            op.tra = static_cast<std::uint8_t>((p[0] >> 9) & 0x7F);
            op.twa = static_cast<std::uint8_t>((p[0] >> 1) & 0x7F);
            op.ira = static_cast<std::uint8_t>((p[1] >> 7) & 0x3F);
            op.iwa = static_cast<std::uint8_t>((p[1] >> 1) & 0x1F);
            op.ysel = static_cast<std::uint8_t>((p[1] >> 13) & 3);
            op.ewa = static_cast<std::uint8_t>((p[2] >> 8) & 0x0F);
            op.shift = static_cast<std::uint8_t>((p[2] >> 4) & 3);
            op.masa = static_cast<std::uint8_t>((p[3] >> 9) & 0x3F);
            std::uint16_t f = 0;
            f |= (p[0] & 0x100) ? kOpTwt : 0;
            f |= (p[1] & 0x8000) ? kOpXsel : 0;
            f |= (p[1] & 0x40) ? kOpIwt : 0;
            f |= (p[2] & 0x1000) ? kOpEwt : 0;
            f |= (p[2] & 0x80) ? kOpAdrl : 0;
            f |= (p[2] & 0x40) ? kOpFrcl : 0;
            f |= (p[2] & 8) ? kOpYrl : 0;
            f |= (p[2] & 4) ? kOpNegb : 0;
            f |= (p[2] & 2) ? kOpZero : 0;
            f |= (p[2] & 1) ? kOpBsel : 0;
            f |= (p[2] & 0x4000) ? kOpMwt : 0;
            f |= (p[2] & 0x2000) ? kOpMrd : 0;
            f |= (p[2] & 0x8000) ? kOpTable : 0;
            f |= (p[3] & 0x100) ? kOpAdreb : 0;
            f = static_cast<std::uint16_t>(f | ((p[3] & 0x80) ? kOpNxadr : 0));
            op.flags = f;
        }
    }
    if (dsp_.stopped)
        return;
    s32 ACC = 0, SHIFTED = 0, X = 0, Y = 0, B = 0, INPUTS = 0;
    s32 MEMVAL[4] = {0, 0, 0, 0};
    s32 FRC_REG = 0, Y_REG = 0;
    u32 ADRS_REG = 0;
    DspState& st = dsp_;
    // Up to the last real instruction only: programs often use a few dozen of the 128 steps,
    // and the rest are all-zero steps that change nothing past the loop.
    for (int step = 0; step < st.steps; ++step) {
        const DspState::Op& op = st.ops[step];
        const std::uint16_t f = op.flags;
        if (f & kOpNop) {
            X = st.TEMP[st.MDEC_CT & 0x7F];
            Y = FRC_REG;
            ACC = static_cast<s32>((static_cast<s64>(X) * Y) >> 12) + X;
            continue;
        }
        const u32 IRA = op.ira;
        const u32 SHIFT = op.shift;
        if (IRA <= 0x1f)
            INPUTS = st.MEMS[IRA];
        else if (IRA <= 0x2F)
            INPUTS = st.MIXS[IRA - 0x20] << 4;  // MIXS is 20 bits
        else if (IRA <= 0x31)
            INPUTS = static_cast<s32>(dsp->EXTS[IRA - 0x30]) << 8;  // EXTS is 16 bits
        else
            INPUTS = 0;
        if (f & kOpIwt)
            st.MEMS[op.iwa] = MEMVAL[step & 3];
        if (!(f & kOpZero)) {
            B = (f & kOpBsel) ? ACC : st.TEMP[(op.tra + st.MDEC_CT) & 0x7F];
            if (f & kOpNegb)
                B = -B;
        } else {
            B = 0;
        }
        X = (f & kOpXsel) ? INPUTS : st.TEMP[(op.tra + st.MDEC_CT) & 0x7F];
        if (op.ysel == 0)
            Y = FRC_REG;
        else if (op.ysel == 1)
            Y = static_cast<s32>(static_cast<s16>(dsp->COEF[step])) >> 3;
        else if (op.ysel == 2)
            Y = Y_REG >> 11;
        else
            Y = (Y_REG >> 4) & 0x0FFF;
        if (f & kOpYrl)
            Y_REG = INPUTS;
        // One-step delay at the adder output: the shifter sees the previous ACC.
        SHIFTED = (SHIFT == 0 || SHIFT == 3) ? ACC : ACC << 1;
        if (SHIFT < 2)
            SHIFTED = std::min(std::max(SHIFTED, -0x00800000), 0x007FFFFF);
        ACC = static_cast<s32>((static_cast<s64>(X) * Y) >> 12) + B;
        if (f & kOpTwt)
            st.TEMP[(op.twa + st.MDEC_CT) & 0x7F] = SHIFTED;
        if (f & kOpFrcl)
            FRC_REG = SHIFT == 3 ? (SHIFTED & 0x0FFF) : (SHIFTED >> 11);
        if ((step & 1) && (f & (kOpMrd | kOpMwt))) {
            u32 ADDR = dsp->MADRS[op.masa];
            if (f & kOpAdreb)
                ADDR += ADRS_REG & 0x0FFF;
            if (f & kOpNxadr)
                ADDR++;
            if (!(f & kOpTable)) {
                ADDR += st.MDEC_CT;
                ADDR &= st.RBL;
            } else {
                ADDR &= 0xFFFF;
            }
            ADDR <<= 1;
            ADDR += st.RBP;
            if (f & kOpMrd) {
                u16 w;
                std::memcpy(&w, aram_ + (ADDR & kAramMask & ~1u), 2);
                MEMVAL[(step + 2) & 3] = dsp_unpack(w);
            }
            if (f & kOpMwt) {
                const u16 w = dsp_pack(SHIFTED);
                std::memcpy(aram_ + (ADDR & kAramMask & ~1u), &w, 2);
            }
        }
        if (f & kOpAdrl)
            ADRS_REG =
                SHIFT == 3 ? static_cast<u32>(SHIFTED >> 12) : static_cast<u32>(INPUTS >> 16);
        if (f & kOpEwt)
            dsp->EFREG[op.ewa] = static_cast<u32>(SHIFTED >> 8) & 0xFFFFu;
    }
    --st.MDEC_CT;
    if (st.MDEC_CT == 0)
        st.MDEC_CT = st.RBL + 1;
}

void Mixer::sample(std::int16_t& left, std::int16_t& right) {
    ++samples;
    SampleType mixl = 0, mixr = 0;
    std::memset(dsp_.MIXS, 0, sizeof dsp_.MIXS);
    // A disabled channel contributes exactly zero (step_channel's early-out); skipping it saves
    // 64 scattered calls per sample when few voices play.
    for (int i = 0; i < 64; ++i)
        if (chans_[i].enabled)
            chans_[i].step_mix(mixl, mixr);
    DspData* dsp = reinterpret_cast<DspData*>(regs_ + 0x3000);
    const DspOutVol* out_vol = reinterpret_cast<const DspOutVol*>(regs_ + 0x2000);
    // CDDA: the drive's audio on the external inputs (silence without a source). The DSP reads
    // EXTS as a signed value, so it is stored sign-extended, as Flycast stores its s32 samples.
    s16 exts_l = 0, exts_r = 0;
    if (cdda)
        cdda(exts_l, exts_r);
    dsp->EXTS[0] = static_cast<u32>(static_cast<s32>(exts_l));
    dsp->EXTS[1] = static_cast<u32>(static_cast<s32>(exts_r));
    dsp_step();
    if (!dsp_.stopped)
        for (int i = 0; i < 16; i++)
            volume_pan(static_cast<s16>(dsp->EFREG[i]), out_vol[i].EFSDL, out_vol[i].EFPAN, mixl,
                       mixr);
    // ...and directly through effect slots 16 and 17 (Flycast's VOLPAN of EXTS0L/EXTS0R).
    if (exts_l | exts_r) {
        volume_pan(exts_l, out_vol[16].EFSDL, out_vol[16].EFPAN, mixl, mixr);
        volume_pan(exts_r, out_vol[17].EFSDL, out_vol[17].EFPAN, mixl, mixr);
    }
    const CommonRegs* common = reinterpret_cast<const CommonRegs*>(regs_ + 0x2800);
    if (common->Mono)
        mixl = mixr = (mixl + mixr) >> 1;
    const s32 val = kVolumeLut[common->MVOL];
    mixl = static_cast<s32>(fp_mul<s64>(mixl, val, 15));
    mixr = static_cast<s32>(fp_mul<s64>(mixr, val, 15));
    if (common->DAC18B) {
        mixl >>= 2;
        mixr >>= 2;
    }
    if (soft_clip && *soft_clip) {
        // A smooth knee: unchanged below 24576, then tanh into the remaining headroom.
        auto soften = [](s32 x) {
            constexpr float kKnee = 24576.0f, kRange = 32767.0f - 24576.0f;
            const float a = static_cast<float>(x < 0 ? -x : x);
            if (a <= kKnee)
                return x;
            const float y = kKnee + kRange * std::tanh((a - kKnee) / kRange);
            return static_cast<s32>(x < 0 ? -y : y);
        };
        mixl = soften(mixl);
        mixr = soften(mixr);
    }
    mixl = std::clamp(mixl, -32768, 32767);
    mixr = std::clamp(mixr, -32768, 32767);
    left = static_cast<std::int16_t>(mixl);
    right = static_cast<std::int16_t>(mixr);
}

}  // namespace dream::aica
