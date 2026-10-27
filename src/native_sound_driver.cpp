// The native sound driver; see native_sound_driver.h. Hex addresses in comments are those of the
// matching routines in the Manatee driver's ARM7 code.
#include "native_sound_driver.h"

#include <algorithm>

namespace rez::snd {

namespace {

// ---- time --------------------------------------------------------------------------------------
// The driver's steps cost what they cost on the ARM7 (512 cycles per sample), so batches, key-ons
// and program loads keep the original's timing. The timer starts kInitSamples after the release.
constexpr u32 kCyclesPerSample = 512;
constexpr u64 kInitSamples = 1133;
// The reset clears the status area (0x12000-0x16FFF) this many samples after the release; until
// then the SH-4's polls of the tick counter there see the old value.
constexpr u64 kClearSamples = 18;
// An idle pass: 110 instructions, about 990 cycles, split among its steps by instruction share.
constexpr u32 kAdvance = 160, kPop = 100, kMonitor = 230, kFlags = 200;
constexpr u32 kSmall = 150;
constexpr u32 kRunner = 4112;  // the 32 slots scanned, plus each command's own cost (command_cost)
constexpr u32 kBeforeLoad = 9000, kAfterLoad = 2954;
constexpr u32 kLoadSave = 300, kFadeDown = 6216, kLoadClear = 19400, kFill64Words = 1570, kCopy = 26600,
              kFadeUpStart = 1024, kFadeUp = 6283;

// ---- the driver's tables, in its image at sound RAM 0 ----
constexpr u32 kTl = 0x4F68, kLevel = 0x5068, kPan = 0x50A8, kPan5 = 0x5128, kStreamEg = 0x51C8,
              kOctave = 0x425C, kFraction = 0x4340, kRingNeed = 0x80A4;
constexpr u32 kBuiltinSmpb = 0x4B40, kBuiltinSmdb = 0x4E1C, kBuiltinSfpb = 0x3600, kBuiltinSfob = 0x4F2C;
constexpr u32 kSmpb = 0x14080, kSmdb = 0x14290, kSfpb = 0x14200, kSfob = 0x14280, kSfpw = 0x14288,
              kSpsr = 0x14180;
constexpr u32 kSlots = 0x12200, kDoorbell = 0x12400, kTicks = 0x12418, kStreamStatus = 0x13800;

constexpr u32 fourcc(const char* s) {
    return static_cast<u32>(s[0]) | static_cast<u32>(s[1]) << 8 | static_cast<u32>(s[2]) << 16 |
           static_cast<u32>(s[3]) << 24;
}

int clamp31(int x) { return x < 0 ? 0 : (x & 0xE0) ? 31 : x; }
int clamp63(int x) { return (x < 0 ? 0 : (x & 0xC0) ? 63 : x) & 63; }
int clamp127(int x) { return x < 0 ? 0 : (x & 0x80) ? 127 : x; }
int clamp_lev(int x) { return x < 0 ? 0 : (x & 0x100) ? 255 : x; }

// What each command costs the runner, in ARM7 cycles.
u32 command_cost(u8 code) {
    switch (code) {
        case 0x86: return 411;
        case 0x21: return 1806;
        case 0x22: return 461;
        case 0x2E: return 547;
        case 0x82: return 2850;
        case 0x84: return 1726;
        default: return 150;
    }
}

}  // namespace

Driver::Work Driver::estimate(u32 w) const {
    Work k;
    const unsigned type = (w >> 24) & 7, ch = (w >> 16) & 15, port = (w >> 20) & 7, note = (w >> 8) & 0x7F,
                   vel = w & 0x7F;
    auto channel_voices = [&](bool in_use) {
        unsigned n = 0;
        for (unsigned v = 0; v < nmidi_; ++v) {
            const Voice& V = voices_[v];
            if (V.port == port && V.ch == ch && !(in_use && V.free()))
                ++n;
        }
        return n;
    };
    if (type == 0 || (type == 1 && vel == 0)) {
        for (unsigned v = 0; v < nmidi_; ++v) {
            const Voice& V = voices_[v];
            if ((V.flags & 0x80) && V.port == port && V.ch == ch && V.note == note)
                ++k.voices;
        }
        return k;
    }
    if (type == 3) {
        k.voices = channel_voices(((w >> 8) & 0x7F) != 1);
        return k;
    }
    if (type == 6) {
        k.voices = channel_voices(true);
        return k;
    }
    if (type != 1)
        return k;
    const Chan& C = chans_[port * 16 + ch];
    if (C.mute)
        return k;
    // The note-on's walk, on a copy of what it changes: voices cut or taken.
    std::array<u8, 64> taken{};
    u32 lp = C.prog_addr;
    for (int L = 0; L < 4; ++L, lp += 4) {
        const u32 lo = rd32(lp);
        if (lo == 0)
            continue;
        const u32 layer = C.bank_addr + lo;
        const u32 lw = rd32(layer);
        if (lw & 0x80)
            continue;
        ++k.layers;
        u32 n = lw & 0x7F;
        if (n == 0)
            continue;
        u32 S = rd32(layer + 4) + C.bank_addr;
        do {
            ++k.splits;
            if (static_cast<int>(note) < rd8(S + 0x24) || rd8(S + 0x25) < static_cast<int>(note) ||
                static_cast<int>(vel) < rd8(S + 0x2B) || rd8(S + 0x2C) < static_cast<int>(vel)) {
                S += 0x30;
                continue;
            }
            if (rd8(S + 0x2D) && rd8(S + 0x2E)) {
                ++k.cuts;
                for (unsigned v = 0; v < nmidi_; ++v)
                    if (voices_[v].port == port && voices_[v].ch == ch && voices_[v].member == rd8(S + 0x2E))
                        taken[v] |= 1;  // cut: flags 0
            }
            unsigned scan = 0;
            bool found = false;
            for (unsigned v = 0; v < nmidi_; ++v) {
                ++scan;
                const Voice& V = voices_[v];
                const u8 flags = (taken[v] & 2) ? 0x80 : (taken[v] & 1) ? 0 : V.flags;
                if (flags == 0 && V.rel == 0) {
                    taken[v] |= 2;
                    found = true;
                    break;
                }
            }
            k.scanned += scan;
            if (!found && nmidi_)
                found = true;  // a voice is stolen after the full scan
            if (!found) {
                S += 0x30;
                continue;
            }
            ++k.programmed;
            break;
        } while (--n);
    }
    return k;
}

// What a MIDI word costs the driver in ARM7 cycles: linear in the work estimate() counts.
u32 Driver::word_cost(u32 w) const {
    if (w == 0)
        return kPop;
    const Work k = estimate(w);
    const unsigned type = (w >> 24) & 7;
    if (type == 0 || (type == 1 && (w & 0x7F) == 0))
        return 3605 + 351 * k.voices;
    switch (type) {
        case 1:
            return 2855 + 114 * k.layers + 92 * k.splits + 233 * k.scanned + 3516 * k.cuts + 867 * k.programmed;
        case 3:
            switch ((w >> 8) & 0x7F) {
                case 7:
                case 11: return 3215 + 566 * k.voices;
                case 10: return 3803 + 566 * k.voices;
                case 120:
                case 121: return 3637 + 229 * k.voices;
                case 123: return 3415 + 785 * k.voices;
                case 1: return 1500 + 566 * k.voices;
                default: return kSmall;
            }
        case 4: return 821;
        case 6: return 1500 + 300 * k.voices;
        default: return kSmall;
    }
}

Driver::Driver(u8* sound_ram, Synth& synth) : ram_(sound_ram), synth_(synth) {}

void Driver::wr32(u32 a, u32 v) {
    for (unsigned i = 0; i < 4; ++i) wr8(a + i, static_cast<u8>(v >> (8 * i)));
}

// ---- the voices as programmed ------------------------------------------------------------------

u16 Driver::r00(unsigned v) const {
    Settings& s = const_cast<Synth&>(synth_).settings(v);
    return static_cast<u16>(((s.sa >> 16) & 0x7F) | static_cast<u32>(s.fmt) << 7 | (s.loop ? 0x200u : 0u) |
                            (s.noise ? 0x400u : 0u) | (synth_.key(v) ? 0x4000u : 0u));
}

// Register 0 of a voice: the sample's high address bits, format, loop, noise and key. Writing the
// key-apply bit (KYONEX) makes every voice's key take effect.
void Driver::set_r00(unsigned v, u32 value) {
    Settings& s = synth_.settings(v);
    s.sa = (s.sa & 0xFFFFu) | (value & 0x7Fu) << 16;
    s.fmt = static_cast<Fmt>((value >> 7) & 3);
    s.loop = (value >> 9) & 1;
    s.noise = (value >> 10) & 1;
    synth_.sample_changed(v);
    synth_.set_key(v, (value >> 14) & 1);
    if (value & 0x8000)
        synth_.apply_keys();
}

void Driver::set_pitch_reg(unsigned v, u32 value) {
    Settings& s = synth_.settings(v);
    s.fns = value & 0x3FF;
    s.oct = (value >> 11) & 15;
    synth_.pitch_changed(v);
}

void Driver::set_rr(unsigned v, unsigned rr) {
    synth_.settings(v).rr = static_cast<u8>(rr & 31);
    synth_.envelope_changed(v);
}

// ---- start, timer, main loop -------------------------------------------------------------------

void Driver::hold() {
    running_ = false;
}

// The reset vector: every voice keyed off, the state areas cleared, the defaults, the effects
// outputs off and the DSP cleared, master volume 15, then "EXEC" and the timer, which starts
// kInitSamples later (the ARM7's time for all of this).
void Driver::release() {
    running_ = true;
    wr32(0xF8, 0xFF);
    synth_.set_master(0, false);
    for (unsigned v = 0; v < 64; ++v) {
        Settings& s = synth_.settings(v);
        s.rr = 31;
        s.dl = 0;
        s.krs = 0;
        s.lpslnk = false;
        synth_.envelope_changed(v);
        set_r00(v, 0x8000);
    }
    nmidi_ = 48;
    nshot_ = 8;
    nstream_ = 8;
    out_off_.fill(0x80);
    err_ = 0;
    ring_.clear();
    ports_and_channels();
    for (auto& v : voices_) v = Voice{};
    tag_.fill(0);
    for (unsigned i = 0; i < 18; ++i) {
        out_level_[i] = out_pan_[i] = 0;
        synth_.set_output(i, 0, 0);
    }
    dsp_clear();
    fx_status_rbp_ = (rd32(kSfpw) >> 11) & 0xFFF;
    master_ = 15;
    mono_ = 0;
    synth_.set_master(master_, false);
    fx_pending_ = false;
    load_step_ = -1;
    cursor_ = 0;
    flag_1ms_ = flag_4ms_ = false;
    drop_ = 441;
    sub_ = 0;
    ticks_ = 0;
    samples_ = 0;
    arm_ = kInitSamples * kCyclesPerSample;
    next_int_ = kInitSamples + 1;
    stage_ = Stage::Advance;
    stage_end_ = arm_ + kAdvance;
}

// Timer A: every interrupt sets the 1-ms flag; every 441st is not counted; every 4th counted one
// is a tick: the counter the SH-4 reads, the 4-ms flag.
void Driver::interrupt() {
    flag_1ms_ = true;
    if (--drop_ == 0) {
        drop_ = 441;
        return;
    }
    sub_ = (sub_ + 1) & 3;
    if (sub_ != 0)
        return;
    ++ticks_;
    wr32(kTicks, ticks_);
    flag_4ms_ = true;
    flag_1ms_ = false;
}

// The reset's clear of the status area and the memory map's defaults (the built-in drum, effects
// program and output banks), at the time the original reset makes them.
void Driver::clear_status() {
    for (u32 a = 0x12000; a < 0x17000; a += 4) wr32(a, 0);  // globals, slots, status, map, log
    wr32(0x14290, kBuiltinSmdb);
    wr32(0x14294, rd32(0x4F28));
    wr32(0x14298, kBuiltinSmdb);
    wr32(0x1429C, rd32(0x4F28));
    wr32(kSfpb, kBuiltinSfpb);
    wr32(kSfpb + 4, rd32(0x4258));
    wr32(kSfob, kBuiltinSfob);
    wr32(kSfob + 4, rd32(0x4F64));
}

void Driver::sample() {
    if (!running_)
        return;
    ++samples_;
    if (samples_ == kClearSamples)
        clear_status();
    if (samples_ == kInitSamples)
        wr32(0xF8, fourcc("EXEC"));
    if (samples_ == next_int_) {
        interrupt();
        next_int_ += 44;
    }
    if (samples_ < kInitSamples)
        return;
    // The main loop's steps that end within this sample's cycles act now, in order.
    const u64 end = samples_ * kCyclesPerSample;
    while (stage_end_ <= end) {
        arm_ = stage_end_;
        step();
    }
}

void Driver::begin(Stage s, u32 cost) {
    stage_ = s;
    stage_end_ = arm_ + cost;
}

// The main loop, one step at a time: each pass the monitor steps on, up to 4 MIDI words are
// handled, the monitor reads its slot, then the flags: the 4-ms block runs the command slots and
// the per-tick routines; a program load the batch asked for holds the loop.
void Driver::step() {
    switch (stage_) {
        case Stage::Advance:
            monitor_advance();
            pops_ = 0;
            begin(Stage::Pop, word_cost(ring_.empty() ? 0 : ring_.front()));
            return;
        case Stage::Pop:
            if (!ring_.empty()) {
                const u32 w = ring_.front();
                ring_.pop_front();
                midi(w);
            }
            if (++pops_ < 4)
                begin(Stage::Pop, word_cost(ring_.empty() ? 0 : ring_.front()));
            else
                begin(Stage::Monitor, kMonitor);
            return;
        case Stage::Monitor:
            monitor_read();
            begin(Stage::Flags, kFlags);
            return;
        case Stage::Flags:
            if (flag_1ms_) {
                flag_1ms_ = false;
                begin(Stage::Advance, kAdvance);
                return;
            }
            if (!flag_4ms_) {
                begin(Stage::Advance, kAdvance);
                return;
            }
            flag_4ms_ = false;
            if (rd8(kDoorbell)) {
                u32 cost = kRunner;
                for (unsigned i = 0; i < 32; ++i)
                    if (const u8 code = rd8(kSlots + 16 * i))
                        cost += command_cost(code);
                begin(Stage::Runner, cost);
            } else {
                begin(Stage::Tick, kBeforeLoad);
            }
            return;
        case Stage::Runner:
            run_commands();
            begin(Stage::Tick, kBeforeLoad);
            return;
        case Stage::Tick:
            voice_tick();
            fx_rbp();
            if (fx_pending_) {
                fx_pending_ = false;
                load_step_ = 0;
                begin(Stage::Load, kLoadSave);
            } else {
                begin(Stage::After, kAfterLoad);
            }
            return;
        case Stage::Load: {
            const u32 next = fx_load();
            if (next)
                begin(Stage::Load, next);
            else
                begin(Stage::After, kAfterLoad);
            return;
        }
        case Stage::After:
            begin(Stage::Advance, kAdvance);
            return;
    }
}

// ---- the monitor ----

void Driver::monitor_advance() {
    cursor_ = static_cast<u8>((cursor_ + 1) & 63);
}

void Driver::monitor_read() {
    const unsigned s = cursor_;
    const bool looped = synth_.take_looped(s);
    if (s >= nmidi_ + nshot_) {
        const unsigned n = s - nmidi_ - nshot_;
        Stream& T = streams_[n & 15];
        const u32 t = kStreamStatus + n * 0x40;
        if (looped) {
            ++T.loops;
            wr32(t + 0x18, T.loops);
        }
        u32 ca = synth_.position(s);
        const Settings& st = synth_.settings(s);
        if (st.lea > st.lsa && st.lea > 3)
            ca = std::min<u32>(ca, st.lea - 3u);  // the play position reads at most LEA - 3
        ca &= 0xFFFF;
        if (!(T.flags & 1))
            return;
        T.ca = ca;
        T.bytes = T.fmt4 == 0 ? ca << 1 : T.fmt4 == 4 ? ca : ca >> 1;
        T.total = T.loops * T.lea + ca;
        wr32(t + 0x1C, T.ca);
        wr32(t + 0x34, T.bytes);
        wr32(t + 0x20, T.total);
        return;
    }
    if (s >= nmidi_)
        return;  // shots: never played by Rez
    Voice& V = voices_[s];
    if (looped && V.group)
        V.end_seen = V.group;
    // The amplitude envelope as the chip reports it: 13 bits, full scale past the knee.
    const s32 lvl = synth_.eg_level(s);
    const u32 eg = lvl > 0x3BF ? 0x1FFFu : static_cast<u32>(lvl) & 0x1FFFu;
    u8 b = (eg & 0x1F00) ? 0 : static_cast<u8>(0xFF - (eg & 0xFF));
    if (synth_.eg_state(s) != Eg::Release)
        b = 0xFF;
    V.rel = b;
    if (V.free())
        tag_[s] = 0;
}

// 0xB6C: ages, delayed key-ons, auto-off of one-shots that reached their end.
void Driver::voice_tick() {
    for (unsigned v = 0; v < nmidi_; ++v) {
        Voice& V = voices_[v];
        if (V.flags == 0)
            continue;
        if (V.flags & 0xC0)
            ++V.age;
        if (V.delay) {
            if (--V.delay == 0)
                set_r00(v, r00(v) | 0xC000);
        }
        if ((V.flags & 0x20) && V.end_seen) {
            set_r00(v, (r00(v) & ~0x4000u) | 0x8000);
            V.age = 0;
            V.flags = 0;
        }
    }
}

// 0x850: the delay ring's place and length from the memory map's work block.
void Driver::fx_rbp() {
    const u32 a = rd32(kSfpw);
    if (static_cast<s32>(a) < 0x18000)
        return;
    const u32 rbp = (a >> 11) & 0xFFF;
    if ((fx_status_rbp_ & 0xFFF) != rbp) {
        rbp_changed_ = true;
        fx_status_rbp_ = rbp;
    }
    const u32 s = rd32(kSfpw + 4) >> 14;
    const u32 rbl = (s & 0xF8) ? 0x6000 : (s & 4) ? 0x4000 : (s & 2) ? 0x2000 : 0;
    if ((fx_status_rbp_ & 0x6000) != rbl) {
        fx_status_rbp_ = (fx_status_rbp_ & 0xFFF) | rbl;
    } else if (!rbp_changed_) {
        return;
    }
    rbp_changed_ = false;
    synth_.set_ring((fx_status_rbp_ >> 13) & 3);
}

// ---- the command slots -------------------------------------------------------------------------

u8 Driver::err(u8 bit) {
    err_ |= bit;
    return err_;
}

void Driver::run_commands() {
    for (unsigned i = 0; i < 32; ++i) {
        const u32 pkt = kSlots + 16 * i;
        if (rd8(pkt))
            command(pkt);
        wr8(pkt, 0);
    }
    set_r00(63, r00(63) | 0x8000);  // the batch's keys take effect together (slot 63's KYONEX)
    wr8(kDoorbell, 0);
}

void Driver::command(u32 pkt) {
    u8 p[16];
    for (unsigned i = 0; i < 16; ++i) p[i] = rd8(pkt + i);
    const u8 code = p[0];
    const auto h4 = static_cast<u32>(p[4] | p[5] << 8);
    switch (code) {
        case 0x02: {  // port stop: release its voices (p3 bit 0: RR 20), reset the port
            const unsigned port = p[2];
            if (nmidi_ == 0)
                break;
            for (unsigned v = 0; v < nmidi_; ++v)
                if (voices_[v].port == port) {
                    voices_[v].flags = 0;
                    voices_[v].rel = 0;
                    if (p[3] & 1)
                        set_rr(v, 20);
                    set_r00(v, (r00(v) & 0x7FFu) | 0x8000);
                }
            port_reset(port);
            break;
        }
        case 0x05: {  // port volume, live voices' TL
            const unsigned port = p[2] & 7;
            ports_[port].vol = p[3];
            const int d = p[3] * 2 - 256;
            for (unsigned v = 0; v < nmidi_; ++v) {
                Voice& V = voices_[v];
                if (V.port != port || (V.flags == 0 && V.rel == 0))
                    continue;
                int x = V.level + d;
                if (x < 0)
                    x = 0;
                if (x & 0xFF00)
                    x = 255;
                synth_.settings(v).tl = rd8(kTl + static_cast<u32>(x));
                synth_.mix_changed(v);
            }
            break;
        }
        case 0x06: {  // port pan, live voices' DIPAN
            const unsigned port = p[2] & 7;
            ports_[port].pan = p[3];
            for (unsigned v = 0; v < nmidi_; ++v) {
                Voice& V = voices_[v];
                if (V.port != port || (V.flags == 0 && V.rel == 0))
                    continue;
                int x = V.pan + p[3] - 128;
                if (x < 0)
                    x = 0;
                if (x & 0x80)
                    x = 127;
                synth_.settings(v).dipan = rd8(kPan + static_cast<u32>(x));
                synth_.mix_changed(v);
            }
            break;
        }
        case 0x07:  // port speed: the driver's own sequencer only
            ports_[p[2] & 7].speed = h4;
            break;
        case 0x08: {  // port pitch, live voices re-pitched
            const unsigned port = p[2];
            ports_[port & 7].pitch = h4;
            const u32 d = h4 - 0x8000u;
            for (unsigned v = 0; v < nmidi_; ++v) {
                Voice& V = voices_[v];
                if (V.port != port || (V.flags == 0 && V.rel == 0))
                    continue;
                set_pitch(v, V.split, V.tune + d + V.bend);
            }
            break;
        }
        case 0x0A: ports_[p[2] & 7].fx = p[3]; break;
        case 0x0B: ports_[p[2] & 7].direct = p[3]; break;
        case 0x0D: {  // channel mute mask
            const unsigned port = p[2];
            const u32 m = rd32(pkt + 4);
            for (unsigned c = 0; c < 16; ++c) chan(port, c).mute = (m >> c) & 1 ? 0xFF : 0;
            for (unsigned v = 0; v < nmidi_; ++v) {
                Voice& V = voices_[v];
                if (V.port != port || (V.flags == 0 && V.rel == 0) || !((m >> V.ch) & 1))
                    continue;
                V.flags = 0;
                V.rel = 0;
                set_r00(v, (r00(v) & 0x7FFu) | 0x8000);
            }
            break;
        }
        case 0x0E: {  // port reset: mutes and port levels; no voice
            auto reset_values = [this](unsigned port) {
                Port& P = ports_[port & 7];
                P.vol = P.pan = P.direct = P.fx = 0x80;
                P.pitch = P.speed = 0x8000;
                P.flags &= 0x7F;
            };
            if (p[2] == 0xFF) {
                for (unsigned port = 0; port < 8; ++port) reset_values(port);
                for (auto& c : chans_) c.mute = 0;
                break;
            }
            const unsigned port = p[2] & 7;
            for (unsigned c = 0; c < 16; ++c) chan(port, c).mute = 0;
            reset_values(port);
            break;
        }
        case 0x0F: {  // all voices off, all ports reset
            for (unsigned v = 0; v < nmidi_; ++v) {
                voices_[v].flags = 0;
                voices_[v].rel = 0;
                set_rr(v, 20);
                set_r00(v, (r00(v) & 0x7FFu) | 0x8000);
            }
            for (unsigned port = 0; port < 8; ++port) port_reset(port);
            break;
        }
        case 0x21: stream_play(pkt); break;
        case 0x22: stream_stop(pkt); break;
        case 0x25:
        case 0x26:
        case 0x27:
        case 0x28:
        case 0x29:
        case 0x2A:
        case 0x2B:
        case 0x2D:
        case 0x2E: {
            const unsigned n = p[2];
            if (nstream_ == 0)
                break;
            if (code == 0x2E && n == 0xFF) {
                for (unsigned k = 0; k < nstream_; ++k) stream_reset(k);
                break;
            }
            if (nstream_ < n) {
                // the driver's error path writes through an unset pointer
                break;
            }
            Stream& T = streams_[n & 15];
            const unsigned slot = nmidi_ + nshot_ + n;
            Settings& s = synth_.settings(slot);
            if (code == 0x25) {  // volume offset
                T.vol_off = p[3];
                int x = T.level + p[3] * 2 - 256;
                if (x < 0)
                    x = 0;
                if (x & 0x100)
                    x = 255;
                s.tl = rd8(kTl + static_cast<u32>(x));
                synth_.mix_changed(slot);
            } else if (code == 0x26) {  // pan offset
                T.pan_off = p[3];
                s.dipan = rd8(kPan + static_cast<u32>(clamp127(T.pan + p[3] - 128) & 127));
                synth_.mix_changed(slot);
            } else if (code == 0x27 || code == 0x28) {
                (code == 0x27 ? T.pitch2 : T.pitch1) = h4;
                stream_pitch(n);
            } else if (code == 0x29) {  // ISEL / send overrides
                T.isel_over = p[3];
                T.send_over = p[4];
                const unsigned isel = (p[3] == 0xFF ? T.isel : p[3]) & 15;
                const unsigned send = p[4] == 0xFF ? T.send : p[4];
                s.isel = static_cast<u8>(isel);
                s.imxl = rd8(kLevel + static_cast<u32>(clamp63((T.send_off >> 1) - 64 + static_cast<int>(send >> 2))));
                synth_.mix_changed(slot);
            } else if (code == 0x2A) {  // FX send offset
                T.send_off = p[3];
                const unsigned send = T.send_over == 0xFF ? T.send : T.send_over;
                s.imxl = rd8(kLevel + static_cast<u32>(clamp63((p[3] >> 1) - 64 + static_cast<int>(send >> 2))));
                synth_.mix_changed(slot);
            } else if (code == 0x2B) {  // direct level offset
                T.direct_off = p[3];
                const int x = std::clamp((T.direct & 127) + p[3] - 128, 0, 127);
                s.disdl = rd8(kLevel + static_cast<u32>(x >> 1));
                synth_.mix_changed(slot);
            } else if (code == 0x2D) {  // filter
                if ((p[6] & 0xF0) == 0) {
                    T.flv_off = h4;
                    const u32 f = flv(T.flv, h4 >> 2, 0);
                    for (auto& x : s.flv) x = static_cast<u16>(f);
                }
                if ((p[6] & 0x0F) == 0) {
                    T.q_off = p[3];
                    s.q = static_cast<u8>(std::clamp((T.q & 31) + (p[3] >> 2) - 32, 0, 31));
                    s.lpoff = false;
                }
                synth_.filter_changed(slot);
            } else {
                stream_reset(n);
            }
            break;
        }
        case 0x2F:  // all streams off
            for (unsigned n = 0; n < nstream_; ++n) {
                const unsigned slot = nmidi_ + nshot_ + n;
                set_rr(slot, 20);
                synth_.set_key(slot, false);
                streams_[n].flags = 0;
            }
            break;
        case 0x80: {  // reset
            if (p[2]) {
                for (unsigned v = 0; v < nmidi_; ++v) {
                    voices_[v].flags = 0;
                    voices_[v].rel = 0;
                    set_rr(v, 20);
                    set_r00(v, (r00(v) & 0x7FFu) | 0x8000);
                }
                for (unsigned port = 0; port < 8; ++port) port_reset(port);
            }
            if (p[4])
                for (unsigned n = 0; n < nstream_; ++n) {
                    const unsigned slot = nmidi_ + nshot_ + n;
                    set_rr(slot, 20);
                    synth_.set_key(slot, false);
                    streams_[n].flags = 0;
                }
            if (p[5])
                for (unsigned i = 16; i < 18; ++i) {
                    out_level_[i] = out_pan_[i] = 0;
                    synth_.set_output(i, 0, 0);
                }
            if (p[6])
                dsp_clear();
            if (p[7])
                for (unsigned i = 0; i < 16; ++i) {
                    out_level_[i] = out_pan_[i] = 0;
                    synth_.set_output(i, 0, 0);
                }
            break;
        }
        case 0x81:  // master volume
            master_ = (p[2] >> 4) & 15;
            synth_.set_master(master_, mono_ != 0);
            break;
        case 0x82: fx_output(p[2]); break;
        case 0x83: {  // one output adjusted, relative to entry 0 of the selected output bank
            const unsigned o = p[2] & 15;
            if (!sfob_entry_)
                break;
            const u32 src = sfob_entry_ + 2 * o;
            out_off_[2 * o] = p[3];
            const int x = std::clamp((p[3] >> 1) - 64 + (rd8(src) & 15) * 4, 0, 63);
            out_off_[2 * o + 1] = p[4];
            const int y = std::clamp(p[4] - 128 + rd8(kPan5 + rd8(src + 1)), 0, 127);
            out_level_[o] = rd8(kLevel + static_cast<u32>(x)) & 15;
            out_pan_[o] = rd8(kPan + static_cast<u32>(y));
            synth_.set_output(o, out_level_[o], out_pan_[o]);
            break;
        }
        case 0x84: fx_select(p[2]); break;
        case 0x85: dsp_clear(); break;
        case 0x86: ring_.push_back(rd32(pkt + 4)); break;  // a MIDI word, handled after the batch
        case 0x89: break;
        case 0x8A:  // mono / stereo
            mono_ = p[2];
            synth_.set_master(master_, mono_ != 0);
            break;
        case 0x8C: break;  // the SH-4 interrupt after each batch: Rez never asks for it
        case 0x8D: {       // voice partition
            if (p[2] == 0 && p[3] == 0 && p[4] == 0) {
                nmidi_ = 48;
                nshot_ = 8;
                nstream_ = 8;
                break;
            }
            int nt = static_cast<s8>(p[4]);
            if (nt >= 16)
                nt = 16;
            const int rest = 64 - nt;
            int ns = static_cast<s8>(p[3]);
            if (ns >= rest)
                ns = rest;
            nmidi_ = static_cast<unsigned>(rest - ns);
            nshot_ = static_cast<unsigned>(ns);
            nstream_ = static_cast<unsigned>(nt);
            ports_and_channels();
            break;
        }
        default:
            // the driver's sequencer, shots, MIDI input, re-init: unreachable
            break;
    }
}

// 0x8D40: the port's queued note-ons deleted from the ring, its voices released.
void Driver::port_reset(unsigned port) {
    port &= 7;
    for (u32& x : ring_)
        if ((x & 0x07000000u) == 0x01000000u && ((x >> 20) & 0xF) == port)
            x = 0;
    for (unsigned v = 0; v < nmidi_; ++v)
        if (voices_[v].port == port) {
            voices_[v].flags &= static_cast<u8>(~0xA0);
            set_r00(v, (r00(v) & 0x7FFu) | 0x8000);
        }
    ports_[port].flags = 0;
}

// 0x560: port, stream and channel defaults.
void Driver::ports_and_channels() {
    for (auto& P : ports_) P = Port{};
    for (unsigned n = 0; n < nstream_ && n < 16; ++n) {
        Stream& T = streams_[n];
        T.vol_off = T.pan_off = T.q_off = T.send_off = T.direct_off = 0x80;
        T.flv_off = T.pitch1 = T.pitch2 = 0x8000;
        T.isel_over = T.send_over = 0xFF;
    }
    const u32 prog = kBuiltinSmpb + rd32(kBuiltinSmpb + rd32(kBuiltinSmpb + 0x10));
    const u32 curves = kBuiltinSmpb + rd32(kBuiltinSmpb + 0x18);
    for (auto& C : chans_) {
        C = Chan{};
        C.bank_addr = kBuiltinSmpb;
        C.prog_addr = prog;
        C.curve_addr = curves;
    }
}

// ---- streams ----

void Driver::stream_play(u32 pkt) {
    u8 p[16];
    for (unsigned i = 0; i < 16; ++i) p[i] = rd8(pkt + i);
    const unsigned n = p[2] & 15;
    if (nstream_ == 0)
        return;
    if (nstream_ < n) {
        return;
    }
    Stream& T = streams_[n];
    const unsigned ring = p[3] & 15;
    const u32 addr = rd32(kSpsr + ring * 8), size = rd32(kSpsr + 4 + ring * 8);
    if (addr == 0 || size == 0) {
        T.err |= 1;
        T.flags |= 0x80;
        return;
    }
    if (T.flags && T.prio && static_cast<s8>(p[11]) < static_cast<s8>(T.prio)) {
        T.err |= 2;
        T.flags |= 0x80;
        return;
    }
    T.prio = p[11];
    T.ring = static_cast<u8>(ring);
    T.root = p[6];
    T.fine = static_cast<s8>(p[7]);
    const unsigned slot = nmidi_ + nshot_ + n;
    Settings& s = synth_.settings(slot);
    s.lsa = 0;
    synth_.loop_changed(slot);
    unsigned f = p[2] >> 2;
    if (f & 8)
        f = 12;
    f &= 12;
    T.fmt4 = static_cast<u8>(f);
    s32 samples = f == 0 ? static_cast<s32>(size >> 1) : f == 4 ? static_cast<s32>(size) : static_cast<s32>(size << 1);
    const s32 cap = f == 0 ? 0xF800 : f == 4 ? 0xF000 : 0xE000;
    if (cap < samples)
        samples = cap;
    s.lea = static_cast<u16>(samples);
    synth_.loop_changed(slot);
    T.lea = static_cast<u32>(samples);
    T.level = p[4];
    s.tl = rd8(kTl + static_cast<u32>(clamp_lev(p[4] + T.vol_off * 2 - 256)));
    synth_.mix_changed(slot);
    // The envelope, LFO and filter envelope words of the stream table (32-bit entries).
    const u16 eg10 = rd16(kStreamEg), eg14 = rd16(kStreamEg + 4), lfo = rd16(kStreamEg + 8),
              f40 = rd16(kStreamEg + 12), f44 = rd16(kStreamEg + 16);
    s.ar = eg10 & 31;
    s.d1r = (eg10 >> 6) & 31;
    s.d2r = static_cast<u8>((eg10 >> 11) & 31);
    synth_.envelope_changed(slot);
    s.rr = eg14 & 31;
    s.dl = (eg14 >> 5) & 31;
    s.krs = (eg14 >> 10) & 15;
    s.lpslnk = (eg14 >> 14) & 1;
    synth_.envelope_changed(slot);
    s.alfos = lfo & 7;
    s.alfows = (lfo >> 3) & 3;
    s.plfos = (lfo >> 5) & 7;
    s.plfows = (lfo >> 8) & 3;
    s.lfof = (lfo >> 10) & 31;
    s.lfore = (lfo >> 15) & 1;
    synth_.lfo_changed(slot);
    s.fd1r = f40 & 31;
    s.far = (f40 >> 8) & 31;
    synth_.filter_changed(slot);
    s.frr = f44 & 31;
    s.fd2r = (f44 >> 8) & 31;
    synth_.filter_changed(slot);
    T.flv = static_cast<u32>(p[12] | p[13] << 8);
    const u32 fl = flv(T.flv, T.flv_off >> 2, 0);
    for (auto& x : s.flv) x = static_cast<u16>(fl);
    synth_.filter_changed(slot);
    const u8 q = p[14];
    T.q = q & 31;
    if (q & 0x80) {
        s.q = 0;
        s.lpoff = true;
    } else {
        s.q = static_cast<u8>(clamp31((q & 31) + (T.q_off >> 2) - 32));
        s.lpoff = false;
    }
    synth_.filter_changed(slot);
    T.pan = p[5] & 127;
    s.dipan = rd8(kPan + static_cast<u32>(std::clamp(T.pan_off - 128 + T.pan, 0, 127)));
    s.disdl = 0;
    synth_.mix_changed(slot);
    T.direct = p[10] & 127;
    s.disdl = rd8(kLevel + static_cast<u32>(std::clamp(T.direct_off - 128 + T.direct, 0, 127) >> 1));
    synth_.mix_changed(slot);
    T.send = p[9];
    const unsigned send = T.send_over != 0xFF ? T.send_over : p[9];
    T.isel = p[8];
    s.imxl = rd8(kLevel + static_cast<u32>(clamp63((T.send_off >> 1) - 64 + static_cast<int>(send >> 2))));
    s.isel = static_cast<u8>((T.isel_over != 0xFF ? T.isel_over : p[8]) & 15);
    synth_.mix_changed(slot);
    stream_pitch(n);
    s.sa = (s.sa & 0x7F0000u) | (addr & 0xFFFF);
    synth_.sample_changed(slot);
    // Format (PCMS 0, 1 or 3), loop, key; it starts at the batch's closing KYONEX.
    set_r00(slot, ((addr >> 16) & 0x7F) | f << 5 | 0x4200);
    T.flags |= 1;
    T.loops = T.ca = T.total = T.bytes = 0;
    const u32 t = kStreamStatus + n * 0x40;
    wr32(t + 0x18, 0);
    wr32(t + 0x1C, 0);
    wr32(t + 0x20, 0);
    wr32(t + 0x34, 0);
}

void Driver::stream_stop(u32 pkt) {
    const unsigned n = rd8(pkt + 2);
    if (nstream_ == 0)
        return;
    if (nstream_ < n) {
        return;
    }
    const unsigned slot = nmidi_ + nshot_ + n;
    if (rd8(pkt + 3) & 1)
        set_rr(slot, 20);
    synth_.set_key(slot, false);  // keyed off by the batch's closing KYONEX
    streams_[n & 15].flags = 0;
}

// 0x8AA8: the MIDI pitch walk with the note fixed at 60.
void Driver::stream_pitch(unsigned n) {
    const Stream& T = streams_[n & 15];
    const s32 d = static_cast<s32>(T.pitch1 - 0x8000u) + static_cast<s32>(T.pitch2 - 0x8000u) + T.fine;
    const u32 i = ((static_cast<u32>(d) >> 8) + 60 + 96) & 0xFF;
    s32 k = static_cast<s32>(i) - (T.root & 0x7F);
    if (k < 0)
        k = 0;
    k &= 0xFF;
    const u8 t = rd8(kOctave + static_cast<u32>(k));
    const u32 oct = (static_cast<u32>(t) << 7) & 0x7800;
    const u32 j = ((static_cast<u32>(t) << 8 & 0xF00) | (static_cast<u32>(d) & 0xFE)) >> 1;
    const u32 fns = rd8(kFraction + j) + 256u * ((j >= 0x1EF) + (j >= 0x383) + (j >= 0x4D9));
    const u32 r = oct | fns;
    const unsigned slot = nmidi_ + nshot_ + n;
    const Settings& s = synth_.settings(slot);
    if ((static_cast<u32>(s.oct) << 11 | s.fns) != r)
        set_pitch_reg(slot, r);
}

void Driver::stream_reset(unsigned n) {
    Stream& T = streams_[n & 15];
    T.vol_off = T.pan_off = T.direct_off = T.send_off = T.q_off = 0x80;
    T.flv_off = T.pitch1 = T.pitch2 = 0x8000;
    T.isel_over = T.send_over = 0xFF;
    if (T.flags == 0)
        return;
    const unsigned slot = nmidi_ + nshot_ + n;
    Settings& s = synth_.settings(slot);
    s.tl = rd8(kTl + T.level);
    s.dipan = rd8(kPan + T.pan);
    synth_.mix_changed(slot);
    stream_pitch(n);
    s.disdl = rd8(kLevel + (T.direct & 15) * 4u);
    s.isel = T.isel & 15;
    s.imxl = rd8(kLevel + ((T.send & 0xF0u) >> 2));
    synth_.mix_changed(slot);
    T.flags &= 0x7F;
    T.err = 0;
}

// ---- the effects processor ----

void Driver::fx_output(u8 e) {
    u32 b = rd32(kSfob);
    if (b == 0)
        b = kBuiltinSfob;
    if (rd32(b) != fourcc("SFOB")) {
        err(0x20);
        return;
    }
    if (rd8(b + 4) != 2) {
        err(0x08);
        return;
    }
    if (rd32(b + rd32(b + 8) - 4) != fourcc("ENDB")) {
        err(0x40);
        return;
    }
    if (e >= rd8(b + 0xC)) {
        err(0x10);
        return;
    }
    const u32 base = b + rd8(b + 0x10);
    sfob_entry_ = base;  // entry 0's address, whatever the entry (as the driver keeps it)
    const u32 ent = base + e * 32u;
    for (unsigned i = 0; i < 16; ++i) {
        const int lvl = rd8(ent + 2 * i) & 15;
        const int x = std::clamp(out_off_[2 * i] - 128 + lvl * 8, 0, 127);
        const int y = std::clamp(out_off_[2 * i + 1] - 128 + rd8(kPan5 + (rd8(ent + 2 * i + 1) & 31u)), 0, 127);
        out_level_[i] = rd8(kLevel + static_cast<u32>(x >> 1)) & 15;
        out_pan_[i] = rd8(kPan + static_cast<u32>(y));
        synth_.set_output(i, out_level_[i], out_pan_[i]);
    }
}

void Driver::fx_select(u8 k) {
    u32 b = rd32(kSfpb);
    if (b == 0)
        b = kBuiltinSfpb;
    if (rd32(b) != fourcc("SFPB")) {
        err(0x20);
        return;
    }
    if (rd8(b + 4) != 2) {
        err(0x08);
        return;
    }
    if (rd32(b + rd32(b + 8) - 4) != fourcc("ENDB")) {
        err(0x40);
        return;
    }
    if (k >= rd8(b + 0xC)) {
        err(0x10);
        return;
    }
    fx_index_ = k;
    const u32 prog = b + rd32(b + 0x10 + 4u * k);
    const u8 code = rd8(prog + 0x20);
    const u32 need = rd32(kRingNeed + 4u * code);
    const u32 wa = rd32(kSfpw), ws = rd32(kSfpw + 4);
    if (wa == 0 || static_cast<s32>(wa) < 0x18000) {
        err(1);
        return;
    }
    if (ws < need) {
        err(static_cast<u8>(code * 4));
        return;
    }
    fx_rbp_ = ((wa >> 11) & 0xFFF) | (static_cast<u32>(code) & 3) << 13;
    fx_prog_ = prog;
    fx_pending_ = true;
}

// 0x87C0 and its helpers, one step at a time; each returns the cycles to the next (0 when the
// load is done): the outputs fade down a level a step, every voice's effects send is cut and the
// program cleared, the ring filled with silence, the ring set up and the program copied, the
// outputs fade back up.
u32 Driver::fx_load() {
    if (load_step_ == 0)
        for (unsigned i = 0; i < 16; ++i) {
            load_saved_level_[i] = out_level_[i];
            load_saved_pan_[i] = out_pan_[i];
        }
    if (load_step_ < 16) {
        for (unsigned i = 0; i < 16; ++i)
            if (out_level_[i] >= 1) {
                --out_level_[i];
                synth_.set_output(i, out_level_[i], out_pan_[i]);
            }
        ++load_step_;
        return load_step_ < 16 ? kFadeDown : kFadeDown + kLoadClear;
    }
    if (load_step_ == 16) {  // 0x80F4: sends cut on every voice, the program cleared
        for (unsigned v = 0; v < 64; ++v) {
            Settings& s = synth_.settings(v);
            s.isel = s.imxl = 0;
            synth_.mix_changed(v);
        }
        synth_.clear_program();
        load_step_ = 17;
        return static_cast<u32>(static_cast<u64>(kFill64Words) * (rd32(kSfpw + 4) >> 2) / 64);
    }
    if (load_step_ == 17) {  // the ring filled and set up
        if (rd32(kSfpw) || rd32(kSfpw + 4))
            synth_.fill_ring(0x6000);
        if (fx_rbp_)
            synth_.set_ring((fx_rbp_ >> 13) & 3);
        fx_status_rbp_ = fx_rbp_;
        load_step_ = 18;
        return kCopy;
    }
    if (load_step_ == 18) {  // the program copied
        u8 image[0xC00];
        for (u32 i = 0; i < sizeof image; ++i) image[i] = rd8(fx_prog_ + 0x40 + i);
        synth_.load_program(image);
        load_step_ = 19;
        return kFadeUpStart;
    }
    // Fade up: the pans back, then levels 0..15 up to each output's saved level.
    const int lvl = load_step_ - 19;
    if (lvl == 0)
        for (unsigned i = 0; i < 16; ++i) out_pan_[i] = load_saved_pan_[i];
    if (lvl <= 15) {
        for (unsigned i = 0; i < 16; ++i)
            if (load_saved_level_[i] >= lvl) {
                out_level_[i] = static_cast<u8>(lvl);
                synth_.set_output(i, out_level_[i], out_pan_[i]);
            }
        ++load_step_;
        return kFadeUp;
    }
    load_step_ = -1;
    return 0;
}

// 0x80D0 at once: the program cleared, the outputs back at their levels.
void Driver::dsp_clear() {
    for (unsigned v = 0; v < 64; ++v) {
        Settings& s = synth_.settings(v);
        s.isel = s.imxl = 0;
        synth_.mix_changed(v);
    }
    synth_.clear_program();
}

// ---- MIDI --------------------------------------------------------------------------------------

void Driver::midi(u32 w) {
    if (w == 0)
        return;
    const unsigned type = (w >> 24) & 7, ch = (w >> 16) & 15, port = (w >> 20) & 7;
    switch (type) {
        case 0: note_off(port, ch, (w >> 8) & 0x7F); break;
        case 1: note_on(w); break;
        case 3: control(port, ch, (w >> 8) & 0x7F, w & 0x7F); break;
        case 4: program_change(chan(port, ch), (w >> 8) & 0x7F); break;
        case 6: pitch_bend(port, ch, w); break;
        default: break;  // aftertouch ignored; system messages never come from Rez
    }
}

void Driver::note_off(unsigned port, unsigned ch, unsigned note) {
    for (unsigned v = 0; v < nmidi_; ++v) {
        Voice& V = voices_[v];
        if (!(V.flags & 0x80))
            continue;
        if (V.port != port || V.ch != ch || V.note != note)
            continue;
        V.flags &= 0x7F;
        if (V.flags & 0x40)
            continue;  // sustained: released at the pedal
        if (V.flags & 0x20)
            continue;  // a one-shot plays to its end
        V.age = 0;
        V.delay = 0;
        V.rel = 0xFF;
        set_r00(v, (r00(v) & 0x7FFu) | 0x8000);
    }
}

void Driver::note_on(u32 w) {
    const unsigned note = (w >> 8) & 0x7F, vel = w & 0x7F, ch = (w >> 16) & 15, port = (w >> 20) & 7,
                   prio = (w >> 27) & 31;
    if (vel == 0) {
        note_off(port, ch, note);
        return;
    }
    Chan& C = chan(port, ch);
    if (C.mute)
        return;
    u32 lp = C.prog_addr;
    for (int L = 0; L < 4; ++L, lp += 4) {
        const u32 lo = rd32(lp);
        if (lo == 0)
            continue;
        const u32 layer = C.bank_addr + lo;
        const u32 delay = rd32(layer + 8);
        const u8 up = rd8(layer + 0xC), dn = rd8(layer + 0xD);
        const u32 lw = rd32(layer);
        if (lw & 0x80)
            continue;
        u32 n = lw & 0x7F;
        if (n == 0) {
            // the driver would loop 2^32 times
            continue;
        }
        u32 S = rd32(layer + 4) + C.bank_addr;
        do {
            if (static_cast<int>(note) < rd8(S + 0x24) || rd8(S + 0x25) < static_cast<int>(note) ||
                static_cast<int>(vel) < rd8(S + 0x2B) || rd8(S + 0x2C) < static_cast<int>(vel)) {
                S += 0x30;
                continue;
            }
            if (rd8(S + 0x2D) && rd8(S + 0x2E))
                excl_cut(port, ch, rd8(S + 0x2E));
            unsigned v = 0;
            if (!alloc(prio, v)) {
                S += 0x30;
                continue;
            }
            program_voice(v, port, ch, prio, note, vel, S, delay, up, dn);
            break;  // the first matching split of each layer only
        } while (--n);
    }
}

// The voice's registers, written in the driver's order (numbered below).
void Driver::program_voice(unsigned v, unsigned port, unsigned ch, unsigned prio, unsigned note, unsigned vel,
                           u32 S, u32 delay, u8 up, u8 dn) {
    Chan& C = chan(port, ch);
    Port& P = ports_[port];
    Voice& V = voices_[v];
    Settings& s = synth_.settings(v);
    V.prio = static_cast<u8>(prio);
    V.ch = static_cast<u8>(ch);
    V.port = static_cast<u8>(port);
    tag_[v] = static_cast<u8>(port | 0x80);
    V.flags &= static_cast<u8>(~0x20);
    set_r00(v, (r00(v) & 0x1FFu) | 0x8000);  // (1) this voice off; every voice's key applied
    const bool excl = rd8(S + 0x2D) != 0;
    V.delay = delay;
    V.split = S;
    if (C.rpn_range & 0x80) {
        V.bend_up = V.bend_dn = C.rpn_range & 0x7F;
    } else {
        V.bend_up = up;
        V.bend_dn = dn;
    }
    V.tune = static_cast<u32>(C.tune);
    V.note = static_cast<u8>(note);
    const u32 w0 = rd32(S);
    const u32 sa = (((w0 >> 16) | (w0 << 16)) & 0x7FFFFFu) + C.bank_addr;
    s.sa = (s.sa & 0x7F0000u) | (sa & 0xFFFF);  // (2)
    synth_.sample_changed(v);
    const u32 img0 = (w0 & 0x780u) | ((sa >> 16) & 0x7F);
    const u32 lsalea = rd32(S + 4);
    s.lsa = static_cast<u16>(lsalea);  // (3)
    synth_.loop_changed(v);
    s.lea = static_cast<u16>(lsalea >> 16);  // (4)
    synth_.loop_changed(v);
    const u32 e = rd32(S + 8);
    s.ar = static_cast<u8>(clamp31(static_cast<int>(e & 31) + C.ar_off - 32));
    s.d1r = static_cast<u8>(clamp31(static_cast<int>((e >> 6) & 31) + C.d1r_off - 32));
    s.d2r = static_cast<u8>(clamp31(static_cast<int>((e >> 11) & 31) + C.d2r_off - 32));
    synth_.envelope_changed(v);  // (5)
    const u16 h = rd16(S + 0xA);
    int rr = clamp31((h & 31) + C.rr_off - 32);
    if (rr == 0)
        rr = 1;
    s.rr = static_cast<u8>(rr);
    s.dl = static_cast<u8>(clamp31(((h >> 5) & 31) + C.dl_off - 32));
    s.krs = (h >> 10) & 15;
    s.lpslnk = (h >> 14) & 1;
    synth_.envelope_changed(v);  // (6)
    u32 lfo = rd16(S + 0xE);
    const u32 m = C.mod + 1u;
    lfo = (lfo & 0xFF1Fu) | ((((((lfo >> 5) & 7) + 1) * m - 1) >> 7) & 7) << 5;
    lfo = (lfo & 0xFFF8u) | (((((lfo & 7) + 1) * m - 1) >> 7) & 7);
    s.alfos = lfo & 7;
    s.alfows = (lfo >> 3) & 3;
    s.plfos = (lfo >> 5) & 7;
    s.plfows = (lfo >> 8) & 3;
    s.lfof = (lfo >> 10) & 31;
    s.lfore = (lfo >> 15) & 1;
    synth_.lfo_changed(v);  // (7)
    const int fxa = ((P.fx >> 1) & 0x7F) - 64;
    const u32 send = C.fx_send ? C.fx_send : rd32(S + 0x10);
    s.imxl = rd8(kLevel + static_cast<u32>(clamp63(static_cast<int>((send & 0xF0) >> 2) + fxa)));
    const u32 isel = (C.isel & 0xF0) ? rd32(S + 0x10) : C.isel;
    s.isel = isel & 15;
    synth_.mix_changed(v);  // (8)
    const u8 s14 = rd8(S + 0x14);
    s.q = static_cast<u8>(clamp31((s14 & 31) + C.reso - 64));
    s.lpoff = C.filter == 0 ? (s14 & 0x20) != 0 : !(C.filter & 0x40);
    synth_.filter_changed(v);  // (9)
    for (unsigned i = 0; i < 5; ++i) {
        s.flv[i] = static_cast<u16>(flv(rd16(S + 0x16 + 2 * i), C.flv_off[i], C.cutoff));
        synth_.filter_changed(v);  // (10)
    }
    const u32 fe = rd32(S + 0x20);
    s.fd1r = static_cast<u8>(clamp31(static_cast<int>(fe & 31) + C.fd1r_off - 32));
    s.far = static_cast<u8>(clamp31(static_cast<int>((fe >> 8) & 31) + C.far_off - 32));
    synth_.filter_changed(v);  // (11)
    s.frr = static_cast<u8>(clamp31(static_cast<int>((fe >> 16) & 31) + C.frr_off - 32));
    s.fd2r = static_cast<u8>(clamp31(static_cast<int>((fe >> 24) & 31) + C.fd2r_off - 32));
    synth_.filter_changed(v);  // (12)
    V.curve = rd8(S + 0x2A);
    V.vel = static_cast<u8>(vel);
    const u32 cv = rd8(C.curve_addr + V.curve * 128u + vel);
    const u32 lev = (((cv + 1) * (C.vol + 1u)) * ((rd8(S + 0x15) ^ 0xFFu) + 1) - 1) >> 14 & 0xFF;
    V.level = static_cast<u8>(lev);
    V.pstat = port;
    s.tl = rd8(kTl + static_cast<u32>(clamp_lev(P.vol * 2 - 256 + static_cast<int>(lev))));
    synth_.mix_changed(v);  // (13)
    const u32 x = rd16(S + 0x12);
    const int pan = clamp127(rd8(kPan5 + (x & 31)) + C.pan - 64);
    V.pan = static_cast<u8>(pan);
    s.dipan = rd8(kPan + static_cast<u32>(clamp127(P.pan - 128 + pan)));
    s.disdl = rd8(kLevel + static_cast<u32>(clamp63(static_cast<int>((x >> 6) & 63) + ((P.direct >> 1) & 0x7F) - 64)));
    synth_.mix_changed(v);  // (14)
    if (excl) {
        synth_.take_looped(v);  // the monitor pointed here and read: a stale loop flag cleared
        monitor_advance();      // and the sweep skips a slot
    }
    const u32 p = V.tune + (P.pitch - 0x8000u);
    const s32 b = C.bend;
    const u32 rng = (static_cast<u32>(b) & 0xFFFF0000u) ? V.bend_dn : V.bend_up;
    const u32 bo = (static_cast<u32>(b * static_cast<s32>(rng)) >> 5) & 0xFFFF;
    V.bend = bo;
    set_pitch(v, S, p + bo);  // (15)
    u8 fl = 0x80;
    if (excl) {
        V.group = rd8(S + 0x2D);
        V.member = rd8(S + 0x2E);
        set_rr(v, 31);  // (16)
        if (!(rd8(S + 1) & 2))
            fl |= 0x20;  // no loop: keyed off by the driver at its end
    }
    V.end_seen = 0;
    if (C.sustain & 0x40)
        fl |= 0x40;
    V.flags = fl;
    V.age = 1;
    V.rel = 0;
    u32 k = img0 & 0x7FF;
    if (V.delay == 0)
        k |= 0xC000;
    s.lfore = false;
    synth_.lfo_changed(v);  // (17)
    set_r00(v, k);          // (18) key on now, or at the delay's end
}

// 0x16E8: voice allocation (a free voice, else one to steal).
bool Driver::alloc(unsigned prio, unsigned& out) {
    if (nmidi_ == 0)
        return false;
    int best = -1;
    u8 best_rel = 0, best_prio = 0;
    u32 best_age = 0;
    for (unsigned v = 0; v < nmidi_; ++v) {
        const Voice& V = voices_[v];
        if (V.free()) {
            out = v;
            return true;
        }
        if (prio && prio < V.prio)
            continue;
        bool take = false;
        if (best < 0) {
            take = true;
        } else if (best_age != 0) {
            if ((V.flags & 0xE0) == 0)
                take = true;
            else if (best_prio < V.prio)
                continue;
            else if (static_cast<s32>(V.age) < static_cast<s32>(best_age))
                continue;
            else
                take = true;
        } else {
            if (V.flags & 0xE0)
                continue;
            if (best_rel == V.rel) {
                if (V.prio < best_prio)
                    continue;
                take = true;
            } else if (best_rel < V.rel) {
                continue;
            } else {
                take = true;
            }
        }
        if (take) {
            best = static_cast<int>(v);
            best_rel = V.rel;
            best_prio = V.prio;
            best_age = V.age;
        }
    }
    if (best < 0)
        return false;
    out = static_cast<unsigned>(best);
    return true;
}

// 0x1818: the channel's voices of the same exclusive member cut.
void Driver::excl_cut(unsigned port, unsigned ch, u8 member) {
    for (unsigned v = 0; v < nmidi_; ++v) {
        Voice& V = voices_[v];
        if (V.port == port && V.ch == ch && V.member == member) {
            set_r00(v, (r00(v) & ~0x4000u) | 0x8000);
            V.flags = 0;
        }
    }
}

// 0x18B4: program change.
void Driver::program_change(Chan& C, unsigned prog) {
    const u32 saved = C.bank_addr;
    u32 b;
    if (C.drum & 1) {
        b = rd32(kSmdb + C.bank * 8u);
        if (!b)
            b = kBuiltinSmdb;
    } else {
        b = rd32(kSmpb + C.bank * 8u);
        if (!b)
            b = kBuiltinSmpb;
    }
    C.bank_addr = b;
    if (rd32(b) != fourcc("SMDB") && rd32(b) != fourcc("SMPB")) {
        C.bank_addr = saved;
        return;
    }
    if (prog >= rd32(b + 0x14)) {
        C.bank_addr = saved;
        return;
    }
    C.program = static_cast<u8>(prog);
    const u32 e = rd32(b + rd32(b + 0x10) + prog * 4);
    if (e == 0) {
        C.bank_addr = saved;
        return;
    }
    C.prog_addr = b + e;
    C.curve_addr = b + rd32(b + 0x18);
}

// 0x294C: bank select (CC32).
void Driver::bank_select(Chan& C, unsigned v) {
    if (C.drum & 1) {
        if (v >= 2)
            return;
        if (rd32(kSmdb + v * 8) && rd32(kSmdb + 4 + v * 8)) {
            C.bank = static_cast<u8>(v);
            C.bank_addr = rd32(kSmdb + v * 8);
            return;
        }
        C.bank_addr = kBuiltinSmdb;
        err(1);
        return;
    }
    if (v >= 16)
        return;
    C.bank = static_cast<u8>(v);
    if (!(rd32(kSmpb + v * 8) && rd32(kSmpb + 4 + v * 8)))
        err(1);
}

// 0x199C: pitch bend.
void Driver::pitch_bend(unsigned port, unsigned ch, u32 w) {
    const s32 b = static_cast<s32>(((w & 0x7F) << 7) | ((w >> 8) & 0x7F)) - 8192;
    chan(port, ch).bend = b;
    for (unsigned v = 0; v < nmidi_; ++v) {
        Voice& V = voices_[v];
        if (V.free() || V.port != port || V.ch != ch)
            continue;
        const u32 rng = (static_cast<u32>(b) & 0xFFFF0000u) ? V.bend_dn : V.bend_up;
        const u32 bo = (static_cast<u32>(b * static_cast<s32>(rng)) >> 5) & 0xFFFF;
        V.bend = bo;
        set_pitch(v, V.split, V.tune + bo + (ports_[V.pstat].pitch - 0x8000u));
    }
}

// 0x1C60: equal temperament at 1/128 semitone through the driver's two tables.
void Driver::set_pitch(unsigned v, u32 S, u32 p) {
    p += static_cast<u32>(static_cast<s32>(static_cast<s8>(rd8(S + 0x27))));
    const u32 i = ((p >> 8) + voices_[v].note + 96) & 0xFF;
    s32 j = static_cast<s32>(i) - (rd8(S + 0x26) & 0x7F);
    if (j < 0)
        j = 0;
    const u8 t = rd8(kOctave + (static_cast<u32>(j) & 0xFF));
    const u32 oct = (static_cast<u32>(t) << 7) & 0x7800;
    const u32 k = (((static_cast<u32>(t) << 8) & 0xF00) | (p & 0xFE)) >> 1;
    u32 fns = rd8(kFraction + k);
    if (k >= 0x4D9)
        fns += 256;
    if (k >= 0x383)
        fns += 256;
    if (k >= 0x1EF)
        fns += 256;
    const u32 r = oct | fns;
    const Settings& s = synth_.settings(v);
    if ((static_cast<u32>(s.oct) << 11 | s.fns) != r)
        set_pitch_reg(v, r);
}

u32 Driver::flv(u32 f, u32 o, s32 cut) const {
    s32 x = static_cast<s32>(f) + (static_cast<s32>(o) - 0x2000) + cut;
    if (x < 8)
        x = 8;
    if ((x + 8) & 0xE000)
        x = 0x1FF8;
    return static_cast<u32>(x);
}

void Driver::update_flv3(unsigned port, unsigned ch) {
    Chan& C = chan(port, ch);
    for (unsigned v = 0; v < nmidi_; ++v) {
        const Voice& V = voices_[v];
        if (V.free() || V.port != port || V.ch != ch)
            continue;
        synth_.settings(v).flv[3] = static_cast<u16>(flv(rd16(V.split + 0x1C), C.flv_off[3], C.cutoff));
        synth_.filter_changed(v);
    }
}

// CC121's values; of the tuning word, only the low byte is reset.
void Driver::reset_controllers(Chan& C) {
    C.mod = 0;
    C.rpn_range = 0;
    C.bend = 0;
    C.cc7 = 100;
    C.cc11 = 127;
    C.vol = 100;
    C.pan = 64;
    C.tune = static_cast<s32>(static_cast<u32>(C.tune) & ~0xFFu);
    C.sustain = 0;
    C.reso = 64;
    for (auto& x : C.flv_off) x = 0x2000;
    C.far_off = C.fd1r_off = C.fd2r_off = C.frr_off = 32;
    C.fx_send = 0;
    C.ar_off = C.d1r_off = C.dl_off = C.d2r_off = C.rr_off = 32;
    C.filter = 0;
    C.cutoff = 0;
    C.coarse = C.fine = 64;
    C.isel = 127;
}

// 0x2190 and its handlers: control change.
void Driver::control(unsigned port, unsigned ch, unsigned cc, unsigned val) {
    Chan& C = chan(port, ch);
    Port& P = ports_[port];
    auto voices_of = [&](bool in_use, auto&& f) {
        for (unsigned v = 0; v < nmidi_; ++v) {
            Voice& V = voices_[v];
            if (V.port != port || V.ch != ch || (in_use && V.free()))
                continue;
            f(v, V);
        }
    };
    auto level_update = [&]() {
        C.vol = static_cast<u8>(((C.cc7 + 1) * (C.cc11 + 1) - 1) >> 7);
        const int pv = P.vol * 2 - 256;
        voices_of(true, [&](unsigned v, Voice& V) {
            const u32 cv = rd8(C.curve_addr + V.curve * 128u + V.vel);
            const u32 lev = (((cv + 1) * (C.vol + 1u)) * ((rd8(V.split + 0x15) ^ 0xFFu) + 1) - 1) >> 14 & 0xFF;
            V.level = static_cast<u8>(lev);
            synth_.settings(v).tl = rd8(kTl + static_cast<u32>(clamp_lev(pv + static_cast<int>(lev))));
            synth_.mix_changed(v);
        });
    };
    switch (cc) {
        case 1: {  // modulation: LFO depths, on every voice of the channel
            C.mod = static_cast<u8>(val);
            const u32 m = val + 1;
            voices_of(false, [&](unsigned v, Voice& V) {
                const u32 lfo = rd16(V.split + 0xE);
                const u32 r = ((lfo & 0xFF18u) | (((((((lfo >> 5) & 7) + 1) * m - 1) >> 7) << 5) & 0xE0) |
                               (((((lfo & 7) + 1) * m - 1) >> 7) & 7)) & ~0x8000u;
                Settings& s = synth_.settings(v);
                s.alfos = r & 7;
                s.alfows = (r >> 3) & 3;
                s.plfos = (r >> 5) & 7;
                s.plfows = (r >> 8) & 3;
                s.lfof = (r >> 10) & 31;
                s.lfore = false;
                synth_.lfo_changed(v);
            });
            break;
        }
        case 7:
            C.cc7 = static_cast<u8>(val);
            level_update();
            break;
        case 11:
            C.cc11 = static_cast<u8>(val);
            level_update();
            break;
        case 10:
            C.pan = static_cast<u8>(val);
            voices_of(true, [&](unsigned v, Voice& V) {
                V.pan = static_cast<u8>(clamp127(rd8(kPan5 + (rd8(V.split + 0x12) & 31u)) + static_cast<int>(val) - 64));
                synth_.settings(v).dipan = rd8(kPan + static_cast<u32>(clamp127(V.pan + P.pan - 128)));
                synth_.mix_changed(v);
            });
            break;
        case 32: bank_select(C, val); break;
        case 64:
            C.sustain = val & 0x40;
            if (val & 0x40) {
                voices_of(true, [&](unsigned, Voice& V) { V.flags |= 0x40; });
            } else {
                voices_of(true, [&](unsigned v, Voice& V) {
                    V.flags &= static_cast<u8>(~0x40);
                    if (V.flags & 0x80)
                        return;
                    V.age = 0;
                    set_r00(v, (r00(v) & 0x3FFu) | 0x8000);
                });
            }
            break;
        case 71:
        case 85:
            C.reso = static_cast<u8>(val);
            voices_of(true, [&](unsigned v, Voice& V) {
                Settings& s = synth_.settings(v);
                s.q = static_cast<u8>(clamp31((rd8(V.split + 0x14) & 31) + static_cast<int>(val) - 64));
                synth_.filter_changed(v);
            });
            break;
        case 72: C.rr_off = static_cast<u8>(((val ^ 0xFF) >> 1) & 63); break;
        case 86: C.rr_off = static_cast<u8>((val >> 1) & 63); break;
        case 73: C.ar_off = static_cast<u8>(((val ^ 0xFF) >> 1) & 63); break;
        case 87: C.ar_off = static_cast<u8>((val >> 1) & 63); break;
        case 88: C.dl_off = static_cast<u8>((val >> 1) & 63); break;
        case 89: C.d1r_off = static_cast<u8>((val >> 1) & 63); break;
        case 90: C.d2r_off = static_cast<u8>((val >> 1) & 63); break;
        case 75: C.cutoff_scale = static_cast<u8>(std::min(val, 8u) * 4); break;
        case 74:
        case 110:
        case 111:
        case 112:
        case 113:
        case 114:
        case 115:
        case 116:
        case 117: {
            // CC74 picks its scale from CC75 (x32 by default); 110-117 are fixed scales x1..x128.
            static constexpr int kFor75[9] = {32, 1, 2, 4, 8, 16, 32, 64, 128};
            static constexpr int kFixed[8] = {1, 2, 4, 8, 16, 32, 64, 128};
            const int k = cc == 74 ? kFor75[std::min(C.cutoff_scale / 4u, 8u)] : kFixed[cc - 110];
            C.cutoff = static_cast<s32>(val) * k - 64 * k;
            update_flv3(port, ch);
            break;
        }
        case 20:
        case 21:
        case 22:
        case 23:
        case 24:
        case 52:
        case 53:
        case 54:
        case 55:
        case 56: {
            const unsigned i = cc < 52 ? cc - 20 : cc - 52;
            u16& o = C.flv_off[i];
            o = cc < 52 ? static_cast<u16>((o & 0x7F) | val << 7) : static_cast<u16>((o & ~0x7Fu) | val);
            C.filter = 127;
            if (i == 3)
                update_flv3(port, ch);
            break;
        }
        case 25: C.far_off = static_cast<u8>((val >> 1) & 63); C.filter = 127; break;
        case 26: C.fd1r_off = static_cast<u8>((val >> 1) & 63); C.filter = 127; break;
        case 27: C.fd2r_off = static_cast<u8>((val >> 1) & 63); C.filter = 127; break;
        case 28: C.frr_off = static_cast<u8>((val >> 1) & 63); C.filter = 127; break;
        case 48: P.cc48 = static_cast<u8>(val); break;
        case 70: C.filter = static_cast<u8>(val); break;
        case 76: C.isel = static_cast<u8>((val & 0x70) ? 127 : (val & 15)); break;
        case 91:
            if (val) {
                C.fx_send = static_cast<u8>(val);
                const u8 imxl = rd8(kLevel + static_cast<u32>(clamp63(((P.fx >> 1) - 64) + static_cast<int>(val >> 1))));
                voices_of(true, [&](unsigned v, Voice& V) {
                    Settings& s = synth_.settings(v);
                    s.isel = rd8(V.split + 0x10) & 15;
                    s.imxl = imxl;
                    synth_.mix_changed(v);
                });
            } else {
                C.fx_send = 0;
                voices_of(true, [&](unsigned v, Voice& V) {
                    Settings& s = synth_.settings(v);
                    const u8 sp = rd8(V.split + 0x10);
                    s.isel = sp & 15;
                    s.imxl = rd8(kLevel + static_cast<u32>(clamp63(((sp >> 4) & 15) * 4 + ((P.fx >> 1) & 0x7F) - 64)));
                    synth_.mix_changed(v);
                });
            }
            break;
        case 120:  // all sound off: RR 31, key off
            voices_of(true, [&](unsigned v, Voice& V) {
                V.flags &= 0x1F;
                V.age = 0;
                V.delay = 0;
                set_rr(v, 31);
                set_r00(v, (r00(v) & 0xBFFFu) | 0x8000);
            });
            break;
        case 121:  // reset controllers
            reset_controllers(C);
            voices_of(true, [&](unsigned v, Voice& V) {
                V.flags &= static_cast<u8>(~0x60);
                if (V.flags & 0x80)
                    return;
                V.age = 0;
                V.delay = 0;
                set_r00(v, (r00(v) & 0xBFFFu) | 0x8000);
            });
            break;
        case 123:  // all notes off
            voices_of(true, [&](unsigned v, Voice& V) {
                if (!(V.flags & 0x80))
                    return;
                V.flags &= 0x7F;
                if (V.flags & 0x60)
                    return;
                V.age = 0;
                V.delay = 0;
                V.rel = 0xFF;
                set_r00(v, (r00(v) & 0xBFFFu) | 0x8000);
            });
            break;
        case 6:
        case 96:
        case 97: {  // data entry and increment / decrement
            const int rel = cc == 96 ? 1 : cc == 97 ? -1 : 0;
            if (C.nrpn_data & 0x80) {
                C.nrpn_data = static_cast<u8>(val | 0x80);
                break;
            }
            if (!(C.rpn_data & 0x80))
                break;
            C.rpn_data = static_cast<u8>(val | 0x80);
            if (C.rpn_msb & 0x7F) {
                if (C.rpn_msb == 0x7F && C.rpn_lsb == 0x7F)
                    C.rpn_data &= 0x7F;
                break;
            }
            switch (C.rpn_lsb & 0x7F) {
                case 0:
                    C.rpn_range = static_cast<u8>((rel ? clamp127((C.rpn_range & 0x7F) + rel) : static_cast<int>(val)) | 0x80);
                    break;
                case 1:
                    C.fine = static_cast<u8>(rel ? clamp127(C.fine + rel) : static_cast<int>(val));
                    C.tune = C.fine * 2 - 128 + ((C.coarse - 64) << 8);
                    break;
                case 2:
                    C.coarse = static_cast<u8>(rel ? clamp127(C.coarse + rel) : static_cast<int>(val));
                    C.tune = ((C.coarse - 64) << 8) + C.fine * 2 - 128;
                    break;
                default: break;
            }
            break;
        }
        case 98: C.nrpn_lsb = static_cast<u8>(val); C.nrpn_data |= 0x80; C.rpn_data &= 0x7F; break;
        case 99: C.nrpn_msb = static_cast<u8>(val); C.nrpn_data |= 0x80; C.rpn_data &= 0x7F; break;
        case 100: C.rpn_lsb = static_cast<u8>(val); C.rpn_data |= 0x80; C.nrpn_data &= 0x7F; break;
        case 101: C.rpn_msb = static_cast<u8>(val); C.rpn_data |= 0x80; C.nrpn_data &= 0x7F; break;
        case 77:
        case 78:
        case 79:
        case 80:
        case 81:
            // effects controls on port 7: Rez's songs do not use them
            break;
        default: break;
    }
}

}  // namespace rez::snd
