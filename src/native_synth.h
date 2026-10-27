// The synth: 64 voices and the effects processor that play Rez's banks, driven by calls from the
// native sound driver and mixed to 16-bit stereo at 44.1 kHz. The banks hold AICA voice settings
// and effect programs, so the arithmetic is the AICA model's (Flycast's sgc_if.cpp and
// dsp_interp.cpp, GPL-2.0), with its generated tables; there is no register file, timer or
// interrupt.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rez::snd {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

enum class Fmt : u8 { Pcm16 = 0, Pcm8 = 1, Adpcm = 2, AdpcmStream = 3 };
enum class Eg : u8 { Attack = 0, Decay1 = 1, Decay2 = 2, Release = 3 };

// A voice's settings, field for field what a bank split and the driver give it.
struct Settings {
    // the sample: start address in sound RAM, format, looping, loop start and end (samples)
    u32 sa = 0;
    Fmt fmt = Fmt::Pcm16;
    bool loop = false, noise = false;
    u16 lsa = 0, lea = 0;
    // amplitude envelope: rates 0-31, decay level 0-31, key rate scaling 0-15 (15 off), the
    // attack ending at the loop start
    u8 ar = 0, d1r = 0, d2r = 0, rr = 0, dl = 0, krs = 0;
    bool lpslnk = false;
    // pitch: octave (4-bit, signed) and the 10-bit fraction
    u8 oct = 0;
    u16 fns = 0;
    // LFO: reset, frequency, pitch and amplitude waveforms and depths
    bool lfore = false;
    u8 lfof = 0, plfows = 0, plfos = 0, alfows = 0, alfos = 0;
    // mix: effects input and its send level, direct level and pan, total level (attenuation)
    u8 isel = 0, imxl = 0, disdl = 0, dipan = 0, tl = 0;
    // filter: resonance, off, the five levels and the envelope rates
    u8 q = 0;
    bool lpoff = false;
    u16 flv[5] = {};
    u8 far = 0, fd1r = 0, fd2r = 0, frr = 0;
};

class Synth {
public:
    explicit Synth(const u8* sound_ram);
    ~Synth();
    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;
    void reset();

    // A voice's settings, by group, as the driver changes them; each takes effect at once on a
    // playing voice.
    Settings& settings(unsigned v);
    void sample_changed(unsigned v);    // sa, fmt, loop, noise
    void loop_changed(unsigned v);      // lsa, lea
    void envelope_changed(unsigned v);  // ar, d1r, d2r, rr, dl, krs, lpslnk
    void pitch_changed(unsigned v);     // oct, fns
    void lfo_changed(unsigned v);       // lfore, lfof, the waveforms and depths
    void mix_changed(unsigned v);       // isel, imxl, disdl, dipan, tl
    void filter_changed(unsigned v);    // q, lpoff, flv, the filter rates

    // Keys: each voice has a key flag; `apply_keys` starts every keyed voice that is not playing
    // and releases every unkeyed one that is, all at once (the voices' keys change together, as
    // the chip's KYONEX makes them). A voice that ends drops its key.
    void set_key(unsigned v, bool on);
    bool key(unsigned v) const;
    void apply_keys();

    // What the driver reads of a voice: its amplitude envelope (state, level 0-0x3FF), whether it
    // still plays, its play position (samples) and whether it passed its loop end since the last
    // `take_looped`.
    Eg eg_state(unsigned v) const;
    s32 eg_level(unsigned v) const;
    bool playing(unsigned v) const;
    u32 position(unsigned v) const;
    bool take_looped(unsigned v);

    // The effects processor: a program image (COEF, MADRS, MPRO as the AICA lays them out at
    // 0x3000, 3,072 bytes), its delay ring length code (0-3: 8K to 64K words), the 16 outputs'
    // levels (0-15) and pans (5-bit), and the two external inputs' (the disc's CD audio).
    void load_program(const u8* image);
    void clear_program();
    void set_ring(unsigned rbl);
    void fill_ring(u16 word);
    void set_output(unsigned i, u8 level, u8 pan);
    void set_master(u8 volume, bool mono);
    std::function<void(s16& left, s16& right)> external;

    // One output sample. `soft_clip`, when it points at a true flag, rounds off peaks instead of
    // clamping them (the enhancement, the launcher's setting).
    void render(s16& left, s16& right);
    const bool* soft_clip = nullptr;

    // The effects on a sound thread. The voices stay on the caller's thread, in step with the
    // driver that reads them back; each sample's direct mix, sends and CD audio go to the sound
    // thread with any effect-state change made before it, and the thread steps the effects and
    // mixes. render() then returns the sample finished kLatency samples earlier (the first
    // kLatency are silence): the same samples, delayed. Off until start_thread().
    static constexpr unsigned kLatency = 1024;
    void start_thread();
    // A store into sound RAM by anyone (the SH-4, syG2's copy, the driver), after it lands: the
    // sound thread's copy takes it at the same point in the samples (with the thread on).
    void ram_written(u32 offset, unsigned size);
    bool threaded() const noexcept { return thread_ != nullptr; }

    // Every key-on that starts a voice (channel, sample address, octave, fraction), for
    // --keyon-trace.
    std::function<void(unsigned v, u32 sa, unsigned oct, unsigned fns)> on_key_on;
    std::uint64_t samples = 0;

    struct Voice;
    struct Dsp;

private:
    struct Thread;
    // The effects and the final mix of one sample, from the voices' part.
    void finish(s32 mixl, s32 mixr, const s32* sends, s16 ext_l, s16 ext_r, s16& left, s16& right);
    // A voice's settings groups, as the *_changed calls name them.
    enum Group : unsigned { kSampleGroup, kLoopGroup, kEnvelopeGroup, kPitchGroup, kLfoGroup, kMixGroup, kFilterGroup };
    static void update_group(Voice& vo, unsigned group);
    void changed(unsigned v, unsigned group);
    void keys_into(Voice* voices);
    void mix_voices(Voice* voices, std::uint64_t enabled, s32& mixl, s32& mixr);
    void apply_program(const u8* image);
    void apply_clear();
    void apply_output(unsigned i, u8 level, u8 pan);

    // The split: the samples each control voice has been stepped through (sync).
    void sync(unsigned v) const;
    mutable std::uint64_t synced_[64] = {};

    const u8* ram_;
    std::unique_ptr<Voice[]> voices_;
    std::unique_ptr<Voice[]> render_;  // the split: the sound thread's copies (start_thread)
    // Each set's enabled voices, a bit each, kept by key_on and disable, so a sample's loop visits
    // only those.
    std::uint64_t voices_on_ = 0, render_on_ = 0;
    std::vector<u8> mirror_;           // and their sound RAM, kept by the thread's kAram entries
    std::unique_ptr<Dsp> dsp_;
    std::array<u8, 18> out_level_{}, out_pan_{};
    u8 master_ = 0;
    bool mono_ = false;
    s32 sends_[16] = {};  // the voices' sends to the effects for the sample being rendered (MIXS)
    std::unique_ptr<Thread> thread_;
};

}  // namespace rez::snd
