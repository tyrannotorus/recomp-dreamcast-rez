// The native sound driver: what Rez's Manatee driver does with the command packets the sd library
// leaves in sound RAM, in C++, playing through the synth (native_synth.h). Sound RAM holds what the
// game writes (command slots, doorbell, memory map, banks); the driver keeps its own channel, voice
// and port state. Its main loop runs on the ARM7's clock of 512 cycles per output sample, each step
// costing what it cost there, with the timer interrupt every 44 samples, so batches, key-ons and
// voice ends keep the original's timing.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>

#include "native_synth.h"

namespace rez::snd {

using s8 = std::int8_t;
using u64 = std::uint64_t;

class Driver {
public:
    Driver(u8* sound_ram, Synth& synth);

    // The SH-4 holds the driver in reset or lets it run (the AICA's ARM reset register).
    void hold();
    void release();
    bool running() const { return running_; }

    // One output sample's worth of driver time; called before the synth renders it.
    void sample();

    // The driver's MIDI channel, voice, port and stream records.
    struct Chan {
        u8 program = 0, bank = 0, mod = 0, rpn_range = 0;
        s32 bend = 0;
        u8 cc7 = 100, cc11 = 127, vol = 100, pan = 64;
        u32 bank_addr = 0, prog_addr = 0, curve_addr = 0;
        s32 tune = 0;
        u8 rpn_msb = 0, rpn_lsb = 0, nrpn_msb = 0, nrpn_lsb = 0, nrpn_data = 0, rpn_data = 0;
        u8 sustain = 0, reso = 64;
        u16 flv_off[5] = {0x2000, 0x2000, 0x2000, 0x2000, 0x2000};
        u8 far_off = 32, fd1r_off = 32, fd2r_off = 32, frr_off = 32;
        u8 fx_send = 0;
        u8 ar_off = 32, d1r_off = 32, dl_off = 32, d2r_off = 32, rr_off = 32;
        u8 drum = 0, filter = 0, cutoff_scale = 0, mute = 0;
        s32 cutoff = 64;
        u8 coarse = 64, fine = 64, isel = 127;
    };
    struct Voice {
        u8 flags = 0, rel = 0, port = 0, ch = 0, note = 0, vel = 0, level = 0, pan = 0;
        u8 bend_up = 0, bend_dn = 0;
        u32 bend = 0, split = 0;
        u32 tune = 0;
        unsigned pstat = 0;  // port
        u8 curve = 0, end_seen = 0, group = 0, member = 0, prio = 0;
        u32 delay = 0, age = 0;
        bool free() const { return flags == 0 && rel == 0; }
    };
    struct Port {
        u8 flags = 0, vol = 0x80, pan = 0x80, fx = 0x80, direct = 0x80, cc48 = 0;
        u32 pitch = 0x8000, speed = 0x8000;
    };
    struct Stream {
        u8 flags = 0, prio = 0, err = 0, level = 0, vol_off = 0x80, pan = 0, pan_off = 0x80;
        u8 isel_over = 0xFF, send_over = 0xFF, send_off = 0x80, direct = 0, direct_off = 0x80;
        u8 root = 0, ring = 0, q = 0, q_off = 0x80, fmt4 = 0, send = 0, isel = 0;
        s8 fine = 0;
        u32 pitch1 = 0x8000, pitch2 = 0x8000, loops = 0, ca = 0, total = 0, lea = 0, flv = 0;
        u32 flv_off = 0x8000, bytes = 0;
    };

private:
    // ---- sound RAM ----
    u8 rd8(u32 a) const { return ram_[a & 0x1FFFFF]; }
    u16 rd16(u32 a) const { return static_cast<u16>(rd8(a) | rd8(a + 1) << 8); }
    u32 rd32(u32 a) const { return rd16(a) | static_cast<u32>(rd16(a + 2)) << 16; }
    void wr8(u32 a, u8 v) {
        ram_[a & 0x1FFFFF] = v;
        synth_.ram_written(a & 0x1FFFFF, 1);  // the sound thread's copy of sound RAM follows
    }
    void wr32(u32 a, u32 v);

    // ---- what the driver programmed into each voice of the synth ----
    u16 r00(unsigned v) const;
    void set_r00(unsigned v, u32 value);  // sample fields and key; KYONEX applies every voice's key
    void set_pitch_reg(unsigned v, u32 value);
    void set_rr(unsigned v, unsigned rr);

    void clear_status();

    // The work a MIDI word will make the driver do, counted before it is done (nothing changes):
    // for a note-on the layers and splits it walks, the voices the search scans, the exclusive
    // cuts (each a scan of every voice) and the voices it programs; for a note-off the voices it
    // releases; for a controller the channel's voices it updates. Its cost follows from these.
    struct Work {
        unsigned layers = 0, splits = 0, scanned = 0, cuts = 0, programmed = 0, voices = 0;
    };
    Work estimate(u32 w) const;
    u32 word_cost(u32 w) const;

    // ---- the main loop and its timing ----
    // The loop is a sequence of steps; each takes its cost in cycles and acts when it ends.
    enum class Stage : u8 { Advance, Pop, Monitor, Flags, Runner, Tick, Load, After };
    void interrupt();
    void begin(Stage s, u32 cost);
    void step();
    void monitor_advance();
    void monitor_read();
    void voice_tick();
    void fx_rbp();

    // ---- commands ----
    void run_commands();
    void command(u32 pkt);
    void port_reset(unsigned port);
    void ports_and_channels();
    void stream_play(u32 pkt);
    void stream_stop(u32 pkt);
    void stream_pitch(unsigned n);
    void stream_reset(unsigned n);
    void fx_output(u8 entry);
    void fx_select(u8 k);
    u32 fx_load();
    void dsp_clear();
    u8 err(u8 bit);

    // ---- MIDI ----
    void midi(u32 w);
    void note_off(unsigned port, unsigned ch, unsigned note);
    void note_on(u32 w);
    void program_voice(unsigned v, unsigned port, unsigned ch, unsigned prio, unsigned note, unsigned vel, u32 S,
                       u32 delay, u8 up, u8 dn);
    bool alloc(unsigned prio, unsigned& v);
    void excl_cut(unsigned port, unsigned ch, u8 member);
    void program_change(Chan& C, unsigned prog);
    void bank_select(Chan& C, unsigned v);
    void pitch_bend(unsigned port, unsigned ch, u32 w);
    void set_pitch(unsigned v, u32 S, u32 p);
    void control(unsigned port, unsigned ch, unsigned cc, unsigned val);
    void reset_controllers(Chan& C);
    void update_flv3(unsigned port, unsigned ch);
    u32 flv(u32 f, u32 o, s32 cut) const;

    Chan& chan(unsigned port, unsigned ch) { return chans_[(port & 7) * 16 + (ch & 15)]; }

    u8* ram_;
    Synth& synth_;
    bool running_ = false;

    // the driver's state
    std::array<Chan, 128> chans_{};
    std::array<Voice, 64> voices_{};
    std::array<u8, 64> tag_{};
    std::array<Port, 8> ports_{};
    std::array<Stream, 16> streams_{};
    std::deque<u32> ring_;  // the MIDI word ring
    unsigned nmidi_ = 48, nshot_ = 8, nstream_ = 8;
    u8 cursor_ = 0;
    u8 err_ = 0;
    u8 mono_ = 0, master_ = 15;
    std::array<u8, 18> out_level_{}, out_pan_{};  // the effects outputs as programmed
    std::array<u8, 32> out_off_{};                // per-output offsets (cmd 0x83), 0x80 neutral
    u32 sfob_entry_ = 0, fx_prog_ = 0, fx_rbp_ = 0, fx_status_rbp_ = 0;
    bool fx_pending_ = false, rbp_changed_ = false;
    u8 fx_index_ = 0;

    // time
    u64 samples_ = 0;     // since release
    u64 arm_ = 0;         // the main loop's clock, cycles since release
    u64 next_int_ = 0;    // the sample of the next timer interrupt
    Stage stage_ = Stage::Advance;
    u64 stage_end_ = 0;   // when the current step ends and acts
    unsigned pops_ = 0;   // MIDI words taken this pass
    unsigned drop_ = 441, sub_ = 0;
    bool flag_1ms_ = false, flag_4ms_ = false;
    u32 ticks_ = 0;

    // A program load in progress: the step it is at (the main loop waits for it).
    int load_step_ = -1;
    std::array<u8, 16> load_saved_level_{}, load_saved_pan_{};
};

}  // namespace rez::snd
