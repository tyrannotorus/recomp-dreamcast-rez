// Native gdFs: Sega's Katana file library ("gdFs Ver 2.03" at 0x8C010220-0x8C012166, its lower
// layer at 0x8C031BC8-0x8C032656), ported function by function. Its structures stay in guest
// memory, filled as gdFs fills them, since the game and CRI's gdCi read them directly. Requests go
// to Drive, a native GD-ROM drive on the launcher's disc that keeps the BIOS's request protocol and
// the drive's timing; gdFs's own CPU time is not reproduced.
#include "native_gdfs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/sched/scheduler.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/cost.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"
#include "dreamcomp/hook.h"

namespace rez {
namespace {

using dream::Memory;
using dream::sh4::Ctx;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

dream::System* g_sys = nullptr;
bool g_on = true;

// ---- gdFs's memory -------------------------------------------------------------------------------

constexpr u32 kWorkPtr = 0x8C10EE88u;   // GDFS_WORK*, 0 = not initialised
constexpr u32 kChainVblank = 0x8C10EE8Cu, kChainDma = 0x8C10EE90u;  // syChain handles
constexpr u32 kApiPtr = 0x8C10EE94u;    // the API table pointer the public stubs jump through
constexpr u32 kApiTable = 0x8C0E7EBCu;
constexpr u32 kDriveTable = 0x8C0E978Cu;
constexpr u32 kModeDefault = 0x8C0E7F40u;  // s16 1: the transfer mode Open gives a handle
constexpr u32 kVblankHandler = 0x8C010568u, kDmaEndHandler = 0x8C01054Eu;
constexpr u32 kChainAdd = 0x8C0326B0u, kChainDel = 0x8C032A68u;  // Shinobi syChain
constexpr u32 kEvVblank = 0x0900, kEvGdDma = 0x0A40;             // syChain events
constexpr u32 kVersBuf = 0x8C11FE20u;                            // GET_VERS reply
constexpr u32 kTitleCtx = 0x8C11FE40u, kTitleCtxPtr = 0x8C11FE58u;  // multi-title context
constexpr u32 kReqStat1 = 0x8C11FE5Cu, kReqStat2 = 0x8C11FE60u, kReqStat4 = 0x8C11FE64u;
constexpr u32 kReplayMode = 0x8C11FE68u, kReplayStart = 0x8C11FE6Cu, kReplayEnd = 0x8C11FE70u,
              kReplayRepeat = 0x8C11FE74u, kReqStatPlay = 0x8C11FE78u, kReqStatPause = 0x8C11FE7Cu;
constexpr u32 kIpTitleFlag = 0x8C0080FCu, kIpTitleOfs = 0x8C0080FDu, kIpTitleName = 0x8C0080F0u;
constexpr u32 kPvdFad = 45166, kHdFad = 45150, kFadOffset = 150;
constexpr unsigned kSector = 2048;

// Handle (GDFS) fields; +72 to +82 are s16.
enum : u32 {
    H_WORK = 0, H_IDX = 4, H_FAD = 8, H_SIZE = 12, H_SCT = 16, H_POS = 20, H_TRANS = 24, H_NSCT = 28,
    H_STRM = 32, H_RDEND = 36, H_RDOBJ = 40, H_TREND = 44, H_TROBJ = 48, H_ERRF = 52, H_ERROBJ = 56,
    H_REQ = 60, H_ST3 = 64, H_ASC = 68, H_CMD = 72, H_TSTAT = 74, H_INUSE = 76, H_MODE = 78,
    H_STAT = 80, H_ERR = 82, kHandleSize = 84
};
// Work struct (GDFS_WORK) fields; +56 and +58 are s16. W+64 is the sector buffer.
enum : u32 {
    W_MAX = 0, W_PTFAD = 4, W_PTSIZE = 8, W_DRIVE = 12, W_CURDIR = 16, W_INTH = 20, W_USERH = 24,
    W_BUSY = 28, W_SERVING = 32, W_ERRF = 36, W_ERROBJ = 40, W_VERS = 44, W_LASTERR = 48,
    W_CHANGED = 52, W_MOUNTED = 56, W_REPLAY = 58, W_API = 60, W_BUF = 64, W_PTBUF = 128,
    W_HANDLES = 4224
};
// Status (+80) and command type (+72).
enum : s32 { ST_IDLE = 0, ST_DONE = 1, ST_READ = 2, ST_SEEK = 3, ST_OTHER = 4, ST_ERROR = 5 };
enum : s32 {
    T_NONE = 0, T_READ = 1, T_SEEK = 2, T_ABORT = 3, T_STREAM = 4, T_CDDA = 6, T_SUBCODE = 7,
    T_REPLAY = 8, T_DRVSTAT = 10
};
// The drive layer's status codes (gdFs's translation of CHECK_COMMAND).
enum : s32 { G_NONE = 0, G_DONE = 1, G_PROC = 4, G_ERROR = 5, G_STREAM = 65, G_BUSY = 67 };
// Error codes (Rez's GDD_ERR_* table at 0x8C0F3C24).
enum : s32 {
    E_OK = 0, E_RESET = -2, E_MOUNT = -4, E_DISC = -5, E_NOTFOUND = -8, E_NOHNDL = -9,
    E_ILLHNDL = -10, E_NOTDIR = -11, E_DIROVER = -12, E_BUSY = -13, E_32ALIGN = -14, E_SIZE = -15,
    E_SEEK = -16, E_OFS = -17, E_ILLTMODE = -18, E_READ = -19, E_NOTREAD = -20, E_TOUT = -21,
    E_TRAYOPEN = -23, E_SIZEOVER = -24, E_FATAL = -25, E_RECOVER = -28, E_NOTREADY = -29,
    E_UNITATTENT = -33, E_NOREADABLE = -32, E_TOCMISMATCH = -51
};

// Guest time. TMU0 counts at 0.32 us, i.e. 64 guest cycles a count.
constexpr u64 kTimeout = 15'625'000ull * 64;  // gdFs's 5 s waits
constexpr u64 kStopSettle = 1'563ull * 64;    // gdFsStop's 0.5 ms busy wait
// The period of gdFs's wait loop: one server call per 770 guest cycles.
constexpr u64 kPollCycles = 770;
// gdFsRead's busy delay between refused tries (about 50,000 loop iterations).
constexpr u64 kRetryCycles = 150'000;


u32 work(Memory& m) { return m.read32(kWorkPtr); }
s32 rs16(Memory& m, u32 a) { return static_cast<s16>(m.read16(a)); }
void w16(Memory& m, u32 a, s32 v) { m.write16(a, static_cast<std::uint16_t>(v)); }
bool valid(Memory& m, u32 h) { return h && m.read16(h + H_INUSE) != 0; }
u32 le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }

std::string guest_string(Memory& m, u32 a, unsigned max = 256) {
    std::string s;
    for (unsigned i = 0; a && i < max; ++i) {
        const char ch = static_cast<char>(m.read8(a + i));
        if (!ch)
            break;
        s += ch;
    }
    return s;
}

bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

// ---- guest calls and time ------------------------------------------------------------------------

// Calls a guest function (a callback, Shinobi's syChain) with up to four arguments, as a jsr from
// `from` would, and leaves pr as it was.
u32 call_guest(Ctx& c, Memory& m, u32 fn, u32 from, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0) {
    const u32 pr = c.pr;
    c.r[4] = a0;
    c.r[5] = a1;
    c.r[6] = a2;
    c.r[7] = a3;
    c.pr = from;
    c.pc = from;
    dream::sh4::call_indirect(c, m, fn);
    c.pr = pr;
    return c.r[0];
}

// Guest time passing inside a blocking call: `cycles`, then whatever interrupts are due (unless
// masked: the scheduler still runs, the drive still progresses). Loop work goes through
// dream::sh4::cost (kPollCycles, kRetryCycles); timed waits (kStopSettle, kTimeout) are charged
// unscaled.
void spend(Ctx& c, Memory& m, u32 pc, u64 cycles) {
    c.cycles += cycles;
    if (c.cycles >= c.next_event) {
        c.pc = pc;
        dream::sh4::deliver_irq(c, m);
    }
}

// SR.IMASK = 15 for a scope, restored afterwards (gdFs's MASK/UNMASK).
struct Masked {
    Ctx& c;
    u32 saved;
    explicit Masked(Ctx& ctx) : c(ctx), saved(dream::sh4::read_sr(ctx)) {
        dream::sh4::write_sr(c, saved | 0xF0u);
    }
    ~Masked() { dream::sh4::write_sr(c, (dream::sh4::read_sr(c) & ~0xF0u) | (saved & 0xF0u)); }
};

// ---- the drive -----------------------------------------------------------------------------------

// What the BIOS's CHECK_COMMAND answers for a request.
enum : s32 { B_FAILED = -1, B_NONE = 0, B_PROCESSING = 1, B_COMPLETE = 2, B_STREAM = 3 };
// The drive commands gdFs sends (SEND_COMMAND codes).
enum : u32 {
    C_PIOREAD = 16, C_DMAREAD = 17, C_GETTOC2 = 19, C_PLAY = 20, C_PLAY2 = 21, C_PAUSE = 22,
    C_RELEASE = 23, C_INIT = 24, C_SEEK = 27, C_STREAM = 28, C_GETSCD = 34, C_REQ_STAT = 36,
    C_GET_VERS = 40
};
// Drive states as CHECK_DRIVE reports them (the hardware's numbering).
enum : u32 { D_PAUSE = 1, D_PLAY = 3 };
// SCSI sense keys the drive reports with a failed command.
enum : u32 { SK_MEDIUM = 3, SK_ILLEGAL = 5 };
// The audio status byte of a subcode reply.
enum : u8 { AS_PLAYING = 0x11, AS_PAUSED = 0x12, AS_DONE = 0x13, AS_NONE = 0x15 };
// The GD-ROM's transfer rate, as Flycast models it: a 5-sector batch per million guest cycles
// (1.8 MB/s); the last, short batch at 2 cycles a byte (at least 1,000), which is also the rate of
// a DMA transfer out of the drive's buffer.
constexpr u64 kBatchCycles = 1'000'000, kSectorCycles = kBatchCycles / 5, kDmaCyclesPerByte = 2;

class Drive {
public:
    void start(dream::System& sys, bool cdda) {
        sys_ = &sys;
        event_ = sys.sched.add("native gd-rom", [this](u64, u64) { tick(); });
        if (cdda && sys.aica)
            sys.aica->mixer.cdda = [this](s16& l, s16& r) { cdda_sample(l, r); };
    }

    // BIOS INIT_SYSTEM: the drive and its request numbering start over.
    void init_system() {
        sys_->sched.cancel(event_);
        state_ = State::Idle;
        next_ = 1;
        last_ = 0;
        stop_audio();
    }

    // ---- the drive table (gdFs's W+12), slot by slot ----

    // +4: a read of `nsct` sectors at `fad` into `buf`; mode 1 DMA, 0 PIO, anything else -18.
    u32 read(u32 fad, s32 nsct, u32 buf, s32 mode) {
        if (mode != 0 && mode != 1)
            return static_cast<u32>(E_ILLTMODE);
        if (nsct <= 0)  // gdFs passes a negative count through; the drive refuses it
            return 0;
        const u32 addr = (buf & 0x1FFFFFFFu) | (mode == 0 ? 0xA0000000u : 0u);
        return request(mode == 1 ? C_DMAREAD : C_PIOREAD, {fad, static_cast<u32>(nsct), addr, 0});
    }
    // +8: a stream of `nsct` sectors at `fad`, transferred later by dma_trans.
    u32 stream(u32 fad, s32 nsct) {
        if (nsct <= 0)
            return 0;
        return request(C_STREAM, {fad, static_cast<u32>(nsct), 0, 0});
    }
    // +12: REQ_DMA_TRANS: `n` bytes of the stream into `buf`. 0 accepted, -1 more than the stream
    // has left (gdFs: -24), 1 not now (gdFs: -13).
    s32 dma_trans(Ctx& c, u32 req, u32 buf, u32 n) {
        sync(c);
        if (req != last_ || state_ != State::Busy || cmd_ != C_STREAM || xfer_)
            return 1;
        if (n > s_total_ - s_done_)
            return -1;
        xfer_ = true;
        x_buf_ = buf;
        x_len_ = n;
        x_start_ = now();
        // The transfer waits for the drive to have read what it covers, then moves at DMA speed.
        const u64 ready = s_start_ + ((s_done_ + n + kSector - 1) / kSector) * kSectorCycles;
        x_done_ = std::max(x_start_, ready) + static_cast<u64>(n) * kDmaCyclesPerByte;
        schedule(x_done_ - x_start_);
        return 0;
    }
    // +16: CHECK_COMMAND and gdFs's translation of it: the gdd status, out[] filled.
    s32 check(Ctx& c, u32 req, s32 out[3]) {
        sync(c);
        if (req != last_)
            return G_NONE;
        switch (state_) {
            case State::Busy:
                if (cmd_ == C_STREAM)
                    return G_STREAM;
                out[0] = static_cast<s32>(done_bytes_);
                // status[3]: gdFs's Stop waits for 1 before it aborts; this drive can always abort.
                out[2] = 1;
                return G_PROC;
            case State::Complete:
                state_ = State::Idle;
                out[0] = static_cast<s32>(done_bytes_);
                return G_DONE;
            case State::Failed:
                state_ = State::Idle;
                out[1] = static_cast<s32>(asc_);
                out[0] = map_sense(sense_);
                return G_ERROR;
            default:
                return G_NONE;
        }
    }
    // +20: ABORT_COMMAND. The aborted command reports complete, as gdFs's abort path expects.
    void abort(Ctx& c, u32 req) {
        sync(c);
        if (req != last_ || state_ != State::Busy)
            return;
        sys_->sched.cancel(event_);
        xfer_ = false;
        state_ = State::Complete;
    }
    // +24 SEEK, +28 INIT.
    u32 seek(u32 fad) { return request(C_SEEK, {fad, 0, 0, 0}); }
    u32 init_cmd() { return request(C_INIT, {0, 0, 0, 0}); }
    // +32: CHECK_DRIVE: {state, disc type}; false if the BIOS were busy (never).
    bool check_drive(Ctx& c, u32 out[2]) {
        sync(c);
        out[0] = playing_ && !paused_ ? D_PLAY : D_PAUSE;
        out[1] = sys_->disc ? static_cast<u32>(sys_->disc->type()) : 0;
        return true;
    }
    // +36: CHECK_DMA_TRANS: 1 with the bytes so far while a transfer runs, else 0 with the bytes
    // the stream has left (as gdFs reads the two answers).
    s32 check_dma_trans(Ctx& c, u32 req, s32& x) {
        sync(c);
        if (req != last_ || cmd_ != C_STREAM) {
            x = 0;
            return 0;
        }
        if (xfer_) {
            const u64 span = x_done_ - x_start_, t = std::min(now(), x_done_) - x_start_;
            x = static_cast<s32>(span ? x_len_ * t / span : 0);
            return 1;
        }
        x = static_cast<s32>(s_total_ - s_done_);
        return 0;
    }
    // +40 PLAY (tracks), +44 PLAY2 (FADs), +48 PAUSE, +52 RELEASE.
    u32 play(u32 first, u32 last, u32 repeat) { return request(C_PLAY, {first, last, repeat, 0}); }
    u32 play2(u32 start, u32 end, u32 repeat) { return request(C_PLAY2, {start, end, repeat, 0}); }
    u32 pause() { return request(C_PAUSE, {0, 0, 0, 0}); }
    u32 release(u32 p) { return request(C_RELEASE, {p, 0, 0, 0}); }
    // +56 GETTOC2, +60 GETSCD.
    u32 toc(u32 area, u32 buf) { return request(C_GETTOC2, {area, buf, 0, 0}); }
    u32 subcode(u32 fmt, u32 size, u32 buf) { return request(C_GETSCD, {fmt, size, buf, 0}); }
    // +64: EXEC_SERVER: a submitted command starts (reads, streams) or is carried out.
    void exec_server(Ctx& c) {
        sync(c);
        if (state_ == State::Busy && !started_)
            execute();
    }
    // +72 REQ_STAT (four output pointers), +76 GET_VERS.
    u32 req_stat(u32 a, u32 b, u32 p, u32 d) { return request(C_REQ_STAT, {a, b, p, d}); }
    u32 version(u32 buf) { return request(C_GET_VERS, {buf, 0, 0, 0}); }

    // BIOS G1_DMA_END, from gdFs's GD-DMA-end handler: acknowledges the interrupt and calls the
    // transfer-end callback gdFs passes on every DMA end.
    void g1_dma_end(Ctx& c, Memory& m, u32 func, u32 obj) {
        sys_->holly.clear(dream::holly::Irq::GdromDma);
        if (func)
            call_guest(c, m, func, kDmaEndHandler, obj);
    }

    // The CD-DA output, one stereo sample per call from the sound chip's mixer: the current sector
    // of the audio track at 44.1 kHz (588 samples a sector), silence when not playing.
    void cdda_sample(s16& l, s16& r) {
        if (!playing_ || paused_)
            return;
        if (frame_ == 0 && !load_audio_sector()) {
            finish_play();
            return;
        }
        const u8* p = audio_ + frame_ * 4;
        l = static_cast<s16>(p[0] | (p[1] << 8));
        r = static_cast<s16>(p[2] | (p[3] << 8));
        if (++frame_ == 588) {
            frame_ = 0;
            if (++cd_fad_ > cd_end_) {
                if (repeat_ == 15 || repeats_left_ > 0) {
                    if (repeat_ != 15)
                        --repeats_left_;
                    cd_fad_ = cd_start_;
                } else {
                    finish_play();
                }
            }
            head_fad_ = cd_fad_;
        }
    }

    bool playing() const { return playing_ && !paused_; }

private:
    enum class State { Idle, Busy, Complete, Failed };

    u64 now() const { return sys_->sched.now(); }
    void sync(Ctx& c) {
        if (sys_->sched.now() < c.cycles)
            sys_->sched.advance_to(c.cycles);
    }
    void schedule(u64 cycles) { sys_->sched.request(event_, cycles ? cycles : 1); }

    // One command at a time; 0 while one is pending (the BIOS's "try again").
    u32 request(u32 cmd, std::array<u32, 4> p) {
        if (state_ != State::Idle)
            return 0;
        cmd_ = cmd;
        p_ = p;
        started_ = false;
        done_bytes_ = 0;
        sense_ = asc_ = 0;
        state_ = State::Busy;
        last_ = next_++;
        return last_;
    }
    void complete() { state_ = State::Complete; }
    void fail(u32 sense, u32 asc) {
        sense_ = sense;
        asc_ = asc;
        state_ = State::Failed;
    }
    static s32 map_sense(u32 key) {
        switch (key) {
            case 0: return -27;
            case 1: return -28;
            case 2: return -29;
            case 3: return -30;
            case 4: return -31;
            case 5: return -32;
            case 6: return -33;
            case 7: return -34;
            case 11: return -35;
            case 16: return -36;
            case 32: return -38;
            default: return -26;
        }
    }

    void execute() {
        started_ = true;
        Memory& m = sys_->memory;
        switch (cmd_) {
            case C_DMAREAD:
            case C_PIOREAD:
                stop_audio();
                rd_fad_ = p_[0] & 0xFFFFFFu;
                rd_left_ = rd_total_ = p_[1];
                rd_dest_ = p_[2];
                // The image may unpack them meanwhile, off the guest's thread (Disc::prefetch).
                if (sys_->disc && rd_fad_ >= kFadOffset)
                    sys_->disc->prefetch(rd_fad_ - kFadOffset, rd_total_);
                schedule(batch_cycles(rd_left_));
                return;  // completes from the scheduler
            case C_STREAM:
                stop_audio();
                s_fad_ = p_[0] & 0xFFFFFFu;
                s_total_ = p_[1] * kSector;
                s_done_ = 0;
                s_start_ = now();
                xfer_ = false;
                return;  // transferred by dma_trans
            case C_GETTOC2: {
                u32 t[102];
                sys_->disc->toc(p_[0], t);
                for (unsigned i = 0; i < 102; ++i) m.write32(p_[1] + 4 * i, t[i]);
                done_bytes_ = sizeof t;
                complete();
                return;
            }
            case C_INIT:
                stop_audio();
                complete();
                return;
            case C_SEEK:
                stop_audio();
                head_fad_ = p_[0] & 0xFFFFFFu;
                complete();
                return;
            case C_GET_VERS: {
                // "GDC Version 1.10" with byte 15 = 0x02: gdFs's W+44 comes out negative, which
                // keeps its CD-DA replay bookkeeping off.
                static const char kVers[] = "GDC Version 1.10";
                for (unsigned i = 0; i < 16; ++i) m.write8(p_[0] + i, static_cast<u8>(kVers[i]));
                m.write8(p_[0] + 15, 0x02);
                complete();
                return;
            }
            case C_GETSCD:
                write_subcode(m, p_[0], p_[1], p_[2]);
                complete();
                return;
            case C_PLAY:
            case C_PLAY2:
                if (start_play(cmd_ == C_PLAY2))
                    complete();
                else
                    fail(SK_ILLEGAL, 0x64);  // ILLEGAL REQUEST, "illegal mode for this track"
                return;
            case C_PAUSE:
                if (playing_)
                    paused_ = true;
                complete();
                return;
            case C_RELEASE:
                if (playing_)
                    paused_ = false;
                complete();
                return;
            case C_REQ_STAT:
                // The four words' meaning is unknown; gdFs only compares the third between a play
                // and a pause. This drive answers state, track, position and repeat count.
                m.write32(p_[0], playing() ? D_PLAY : D_PAUSE);
                m.write32(p_[1], track_of(head_fad_));
                m.write32(p_[2], head_fad_);
                m.write32(p_[3], repeat_);
                complete();
                return;
            default:
                fail(SK_ILLEGAL, 0x20);  // invalid command
                return;
        }
    }

    static u64 batch_cycles(u32 left) {
        const u64 len = static_cast<u64>(left) * kSector;
        return len > 5 * kSector ? kBatchCycles : std::max<u64>(len * kDmaCyclesPerByte, 1000);
    }

    // The scheduler event: a read's next batch, or the end of a stream transfer.
    void tick() {
        if (state_ != State::Busy)
            return;
        Memory& m = sys_->memory;
        if (cmd_ == C_STREAM) {
            if (!xfer_)
                return;
            copy_stream(m, x_buf_, s_done_, x_len_);
            s_done_ += x_len_;
            done_bytes_ = x_len_;
            xfer_ = false;
            sys_->holly.raise(dream::holly::Irq::GdromDma);
            if (s_done_ >= s_total_)
                complete();
            return;
        }
        if (cmd_ != C_DMAREAD && cmd_ != C_PIOREAD)
            return;
        const u32 batch = std::min<u32>(rd_left_, 5);
        u8 sec[kSector];
        for (u32 i = 0; i < batch; ++i) {
            if (!read_user(rd_fad_, sec)) {
                fail(SK_MEDIUM, 0x11);  // MEDIUM ERROR, unrecovered read error
                return;
            }
            write_sector(m, rd_dest_, sec);
            ++rd_fad_;
            rd_dest_ += kSector;
        }
        head_fad_ = rd_fad_;
        rd_left_ -= batch;
        done_bytes_ = (rd_total_ - rd_left_) * kSector;
        if (rd_left_ == 0) {
            complete();
            if (cmd_ == C_DMAREAD)
                sys_->holly.raise(dream::holly::Irq::GdromDma);
        } else {
            schedule(batch_cycles(rd_left_));
        }
    }

    bool read_user(u32 fad, u8* out) {
        return sys_->disc && fad >= kFadOffset && sys_->disc->read_user(fad - kFadOffset, out);
    }
    static void write_sector(Memory& m, u32 dest, const u8* sec) {
        for (u32 k = 0; k < kSector; k += 4) m.write32(dest + k, le32(sec + k));
    }
    // Bytes [from, from + n) of the stream into `dest`.
    void copy_stream(Memory& m, u32 dest, u32 from, u32 n) {
        u8 sec[kSector];
        u32 cached = ~0u;
        for (u32 i = 0; i < n; ++i) {
            const u32 at = from + i, s = at / kSector;
            if (s != cached) {
                if (!read_user(s_fad_ + s, sec))
                    std::memset(sec, 0, sizeof sec);
                cached = s;
            }
            m.write8(dest + i, sec[at % kSector]);
        }
        head_fad_ = s_fad_ + (from + n) / kSector;
    }

    // ---- CD-DA ----

    const dream::gdrom::Track* track_at(u32 fad) const {
        return sys_->disc && fad >= kFadOffset ? sys_->disc->track_for(fad - kFadOffset) : nullptr;
    }
    u32 track_of(u32 fad) const {
        const auto* t = track_at(fad);
        return t ? t->number : 0;
    }
    const dream::gdrom::Track* track_number(u32 n) const {
        for (const auto& t : sys_->disc->tracks())
            if (t.number == n)
                return &t;
        return nullptr;
    }
    // PLAY {first, last, repeat} or PLAY2 {start, end, repeat}: only audio may be played. The
    // range ends after `last`'s final sector, or at `end` inclusive; repeat 15 is endless.
    bool start_play(bool by_fad) {
        u32 start, end;
        if (by_fad) {
            start = p_[0] & 0xFFFFFFu;
            end = p_[1] & 0xFFFFFFu;
        } else {
            const auto* a = track_number(p_[0]);
            const auto* b = track_number(std::max(p_[1], p_[0]));
            if (!a)
                return false;
            start = a->lba + kFadOffset;
            end = (b ? b : a)->end_lba() + kFadOffset - 1;
        }
        const auto* t = track_at(start);
        if (!t || t->data || end < start)
            return false;
        cd_start_ = cd_fad_ = head_fad_ = start;
        cd_end_ = end;
        repeat_ = p_[2] & 15;
        repeats_left_ = repeat_;
        frame_ = 0;
        playing_ = true;
        paused_ = false;
        audio_status_ = AS_PLAYING;
        return true;
    }
    bool load_audio_sector() {
        const auto* t = track_at(cd_fad_);
        if (!t || t->data || !sys_->disc->read_raw(cd_fad_ - kFadOffset, audio_))
            return false;
        return true;
    }
    void finish_play() {
        playing_ = paused_ = false;
        audio_status_ = AS_DONE;
    }
    void stop_audio() {
        if (playing_)
            audio_status_ = AS_NONE;
        playing_ = paused_ = false;
    }

    // GETSCD {format, size, buf}: format 0 is the raw P-W subcode (100 bytes, the Q channel in bit
    // 6 of each byte), format 1 the Q data (14 bytes); both behind a 4-byte header (reserved, the
    // audio status, the length big-endian). Q is the current position: control/ADR, track, index,
    // relative and absolute times in BCD, CRC-16/CCITT inverted.
    void write_subcode(Memory& m, u32 fmt, u32 size, u32 buf) {
        const u32 fad = head_fad_;
        const auto* t = track_at(fad);
        const u32 track_start = t ? t->lba + kFadOffset : kFadOffset;
        u8 q[12] = {};
        q[0] = static_cast<u8>((t && t->data ? 0x40 : 0x00) | 0x01);
        q[1] = bcd(t ? t->number : 1);
        q[2] = bcd(1);
        msf(fad >= track_start ? fad - track_start : 0, q + 3);
        q[6] = 0;
        msf(fad, q + 7);
        std::uint16_t crc = 0;
        for (unsigned i = 0; i < 10; ++i) crc = crc16_step(crc, q[i]);
        crc = static_cast<std::uint16_t>(~crc);
        q[10] = static_cast<u8>(crc >> 8);
        q[11] = static_cast<u8>(crc);
        u8 out[100] = {};
        unsigned len;
        if (fmt == 0) {
            len = 100;
            for (unsigned bit = 0; bit < 96; ++bit)
                if (q[bit / 8] & (0x80 >> (bit % 8)))
                    out[4 + bit] = 0x40;
        } else {
            len = 14;
            std::memcpy(out + 4, q, 10);
        }
        out[1] = playing_ ? static_cast<u8>(paused_ ? AS_PAUSED : AS_PLAYING) : audio_status_;
        out[2] = static_cast<u8>(len >> 8);
        out[3] = static_cast<u8>(len);
        if (audio_status_ == AS_DONE && !playing_)
            audio_status_ = AS_NONE;  // "completed" is reported once
        const u32 n = std::min(size, len);
        for (u32 i = 0; i < n; ++i) m.write8(buf + i, out[i]);
        done_bytes_ = n;
    }
    static u8 bcd(u32 v) { return static_cast<u8>(((v / 10) % 10) << 4 | (v % 10)); }
    static void msf(u32 frames, u8* out) {
        out[0] = bcd(frames / (75 * 60));
        out[1] = bcd((frames / 75) % 60);
        out[2] = bcd(frames % 75);
    }
    static std::uint16_t crc16_step(std::uint16_t crc, u8 b) {
        crc ^= static_cast<std::uint16_t>(b) << 8;
        for (int i = 0; i < 8; ++i) crc = static_cast<std::uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
        return crc;
    }

    dream::System* sys_ = nullptr;
    int event_ = -1;
    State state_ = State::Idle;
    u32 next_ = 1, last_ = 0;
    u32 cmd_ = 0;
    std::array<u32, 4> p_{};
    bool started_ = false;
    u32 done_bytes_ = 0, sense_ = 0, asc_ = 0;
    u32 head_fad_ = kHdFad;
    // read
    u32 rd_fad_ = 0, rd_left_ = 0, rd_total_ = 0, rd_dest_ = 0;
    // stream
    u32 s_fad_ = 0, s_total_ = 0, s_done_ = 0;
    u64 s_start_ = 0;
    bool xfer_ = false;
    u32 x_buf_ = 0, x_len_ = 0;
    u64 x_start_ = 0, x_done_ = 0;
    // CD-DA
    bool playing_ = false, paused_ = false;
    u32 cd_start_ = 0, cd_end_ = 0, cd_fad_ = 0, repeat_ = 0, repeats_left_ = 0;
    unsigned frame_ = 0;
    u8 audio_[2352] = {};
    u8 audio_status_ = AS_NONE;
};

Drive g_drive;

// ---- gdFs: handles -------------------------------------------------------------------------------

// 0x8C010D44: initialises a free handle (0 unless +76 == 0); used by Open and init.
u32 init_handle(Memory& m, u32 h, u32 idx, u32 fad, u32 nbytes) {
    if (m.read16(h + H_INUSE) != 0)
        return 0;
    m.write32(h + H_IDX, idx);
    m.write32(h + H_FAD, fad);
    m.write32(h + H_SIZE, nbytes);
    m.write32(h + H_SCT, static_cast<u32>(static_cast<s32>(nbytes + kSector - 1) >> 11));
    for (u32 off : {H_POS, H_TRANS, H_NSCT, H_STRM, H_RDEND, H_TREND, H_ERRF, H_REQ, H_ST3, H_ASC}) m.write32(h + off, 0);
    for (u32 off : {H_CMD, H_TSTAT, H_STAT, H_ERR}) w16(m, h + off, 0);
    w16(m, h + H_INUSE, 1);
    w16(m, h + H_MODE, rs16(m, kModeDefault));
    return h;
}

// 0x8C011662 (iopen): (re)initialises a handle whether or not it is in use, leaving its callbacks,
// +64 and transfer mode alone; 0 if the handle owns the command in flight.
u32 iopen(Memory& m, u32 h, u32 idx, u32 fad, u32 nbytes) {
    if (!h || m.read32(m.read32(h + H_WORK) + W_BUSY) == h)
        return 0;
    m.write32(h + H_IDX, idx);
    m.write32(h + H_FAD, fad);
    m.write32(h + H_SIZE, nbytes);
    m.write32(h + H_SCT, static_cast<u32>(static_cast<s32>(nbytes + kSector - 1) >> 11));
    for (u32 off : {H_POS, H_TRANS, H_NSCT, H_STRM, H_REQ, H_ASC}) m.write32(h + off, 0);
    for (u32 off : {H_CMD, H_TSTAT, H_STAT, H_ERR}) w16(m, h + off, 0);
    w16(m, h + H_INUSE, 1);
    return h;
}

u32 internal_handle(Memory& m) { return m.read32(work(m) + W_INTH); }

// ---- gdFs: the server ----------------------------------------------------------------------------

void error_tail(Ctx& c, Memory& m, u32 w, u32 h, u32 from) {
    const s32 e = rs16(m, h + H_ERR);
    if (e == E_TRAYOPEN || e == E_UNITATTENT)
        m.write32(w + W_CHANGED, 1);
    if (const u32 fn = m.read32(w + W_ERRF))
        call_guest(c, m, fn, from, m.read32(w + W_ERROBJ), static_cast<u32>(e));
    if (const u32 fn = m.read32(h + H_ERRF))
        call_guest(c, m, fn, from, m.read32(h + H_ERROBJ), static_cast<u32>(e));
}

s32 drive_stat(Ctx& c);
bool parse_subcode(Memory& m, u32 buf, s32 o[6]);

// 0x8C010272: the per-request handler, run when W+28 holds a handle.
s32 request_handler(Ctx& c, Memory& m, u32 from) {
    const u32 w = work(m);
    m.write32(w + W_SERVING, 1);
    const u32 h = m.read32(w + W_BUSY);
    s32 out[3] = {};
    const s32 st = h ? g_drive.check(c, m.read32(h + H_REQ), out) : G_NONE;
    if (!h) {
        m.write32(w + W_SERVING, 0);
        return st;
    }
    auto read_err = [&] {
        w16(m, h + H_STAT, ST_ERROR);
        w16(m, h + H_ERR, E_READ);
        m.write32(h + H_ASC, 0);
        error_tail(c, m, w, h, from);
    };
    switch (st) {
        case G_NONE:
        case G_DONE: {
            const s32 old = rs16(m, h + H_CMD);
            m.write32(w + W_BUSY, 0);
            w16(m, h + H_STAT, st);
            m.write32(h + H_REQ, 0);
            w16(m, h + H_CMD, T_NONE);
            if (st != G_DONE)
                break;
            switch (old) {
                case T_READ:
                    m.write32(h + H_TRANS, static_cast<u32>(out[0]));
                    w16(m, h + H_TSTAT, 2);
                    if (m.read32(h + H_NSCT) * kSector == m.read32(h + H_TRANS)) {
                        m.write32(h + H_POS, m.read32(h + H_POS) + m.read32(h + H_NSCT));
                        if (const u32 fn = m.read32(h + H_RDEND))
                            call_guest(c, m, fn, from, m.read32(h + H_RDOBJ));
                    } else {
                        read_err();
                    }
                    break;
                case T_ABORT:
                    w16(m, h + H_TSTAT, 2);
                    break;
                case T_STREAM:
                    if (static_cast<u32>(out[0]) == m.read32(h + H_STRM)) {
                        m.write32(h + H_TRANS, m.read32(h + H_STRM));
                        m.write32(h + H_POS, m.read32(h + H_POS) + m.read32(h + H_NSCT));
                        w16(m, h + H_TSTAT, 2);
                    } else {
                        read_err();
                    }
                    break;
                case T_SUBCODE: {
                    s32 q[6];
                    if (!parse_subcode(m, w + W_BUF, q)) {
                        read_err();
                        break;
                    }
                    const u32 o = m.read32(h + H_FAD);
                    for (unsigned i = 0; i < 4; ++i) m.write32(o + 4 * i, static_cast<u32>(q[i]));
                    m.write32(o + 16, static_cast<u32>(q[4]) & 0xFFFFFFu);
                    break;
                }
                case T_REPLAY: {
                    const u32 req = g_drive.req_stat(kReqStat1, kReqStat2, m.read32(h + H_IDX), kReqStat4);
                    if (req) {
                        m.write32(h + H_REQ, req);
                        w16(m, h + H_STAT, ST_OTHER);
                        w16(m, h + H_CMD, T_CDDA);
                        m.write32(w + W_BUSY, h);
                    } else {
                        w16(m, h + H_STAT, ST_ERROR);
                        w16(m, h + H_ERR, E_RECOVER);
                        m.write32(h + H_ASC, 0);
                        m.write32(m.read32(h + H_IDX), 0);
                        error_tail(c, m, w, h, from);
                    }
                    break;
                }
                default:  // TOC, INIT, GET_VERS, seek, CD-DA, ReqDrvStat: nothing more
                    break;
            }
            break;
        }
        case G_STREAM: {
            s32 x = 0;
            const s32 r = g_drive.check_dma_trans(c, m.read32(h + H_REQ), x);
            if (r == 0) {
                w16(m, h + H_TSTAT, x > 0 ? 0 : 2);
                m.write32(h + H_TRANS, m.read32(h + H_STRM));
            } else if (r == 1) {
                m.write32(h + H_TRANS, static_cast<u32>(x));
            }
            break;
        }
        case G_PROC:
            m.write32(h + H_ST3, static_cast<u32>(out[2]));
            if (rs16(m, h + H_CMD) == T_READ)
                m.write32(h + H_TRANS, static_cast<u32>(out[0]));
            break;
        case G_BUSY:
            break;
        case G_ERROR:
        case 64: {
            w16(m, h + H_STAT, st);
            w16(m, h + H_ERR, out[0]);
            m.write32(h + H_ASC, static_cast<u32>(out[1]));
            m.write32(h + H_REQ, 0);
            m.write32(w + W_BUSY, 0);
            if (rs16(m, h + H_ERR) == E_NOTREADY) {
                if (m.read32(h + H_ASC) == 0x3A) {
                    const s32 d = drive_stat(c);
                    if (d == 6 || d == 0)
                        w16(m, h + H_ERR, E_TRAYOPEN);
                } else if (m.read32(h + H_ASC) == 0x104) {
                    w16(m, h + H_ERR, -37);
                }
            }
            error_tail(c, m, w, h, from);
            break;
        }
        default:
            w16(m, h + H_STAT, st & 0xFF);
            m.write32(h + H_REQ, 0);
            m.write32(w + W_BUSY, 0);
            break;
    }
    m.write32(w + W_SERVING, 0);
    return st;
}

// 0x8C010EA2, the gdFsServer slot: the drive's server, then the request handler if a request is
// in flight.
s32 server(Ctx& c, Memory& m, u32 from) {
    g_drive.exec_server(c);
    if (m.read32(work(m) + W_BUSY))
        return request_handler(c, m, from);
    return 1;
}

s32 get_stat(Memory& m, u32 h) { return valid(m, h) ? rs16(m, h + H_STAT) : E_ILLHNDL; }

// 0x8C0115D0: runs the server until the handle settles; 5 s without progress -21.
s32 wait(Ctx& c, Memory& m, u32 h, u32 pc) {
    u64 since = c.cycles;
    u32 trans = m.read32(h + H_TRANS);
    for (;;) {
        server(c, m, pc);
        switch (get_stat(m, h)) {
            case ST_IDLE: return E_NOREADABLE;
            case ST_DONE: return E_OK;
            case ST_READ:
            case ST_SEEK:
            case ST_OTHER: break;
            case ST_ERROR:
            case 64: return rs16(m, h + H_ERR);
            default: return E_FATAL;
        }
        if (m.read32(h + H_TRANS) != trans) {
            trans = m.read32(h + H_TRANS);
            since = c.cycles;
        }
        if (c.cycles - since > kTimeout)
            return E_TOUT;
        spend(c, m, pc, dream::sh4::cost(kPollCycles));
    }
}

// ---- gdFs: reading -------------------------------------------------------------------------------

// 0x8C011964 gdFsReqRd32 (check_align, mode +78) and 0x8C01188C, the PIO path (mode 0): a read of
// up to `nsct` sectors at the handle's position.
s32 req_read(Ctx& c, Memory& m, u32 h, s32 nsct, u32 buf, bool pio) {
    if (!valid(m, h))
        return E_ILLHNDL;
    const u32 w = m.read32(h + H_WORK);
    if (m.read32(w + W_BUSY))
        return 0;
    if (!pio && (buf & 31))
        return E_32ALIGN;
    const s32 cur = static_cast<s32>(m.read32(h + H_FAD) + m.read32(h + H_POS));
    const s32 end = static_cast<s32>(m.read32(h + H_FAD) + m.read32(h + H_SCT));
    if (cur >= end || nsct == 0) {
        m.write32(h + H_TRANS, 0);
        w16(m, h + H_STAT, ST_DONE);
        return 0;
    }
    if (cur + nsct > end)
        nsct = end - cur;
    const u32 req = g_drive.read(static_cast<u32>(cur), nsct, buf, pio ? 0 : rs16(m, h + H_MODE));
    if (!req) {
        w16(m, h + H_STAT, ST_DONE);
        return 0;
    }
    m.write32(h + H_REQ, req);
    m.write32(h + H_NSCT, static_cast<u32>(nsct));
    w16(m, h + H_STAT, ST_READ);
    w16(m, h + H_TSTAT, 1);
    w16(m, h + H_CMD, T_READ);
    m.write32(h + H_TRANS, 0);
    m.write32(w + W_BUSY, h);
    g_drive.exec_server(c);
    return nsct;
}

// 0x8C0117FE gdFsRead: a blocking read at the position, retried while the drive is busy.
s32 read_sync(Ctx& c, Memory& m, u32 h, s32 nsct, u32 buf, u32 pc) {
    s32 r = 0;
    unsigned tries = 0;
    for (;;) {
        r = req_read(c, m, h, nsct, buf, (buf & 31) != 0);
        if (r != 0 || nsct == 0 || m.read32(h + H_POS) >= m.read32(h + H_SCT))
            break;
        if (++tries == 20000)
            return E_TOUT;
        spend(c, m, pc, dream::sh4::cost(kRetryCycles));
    }
    if (r < 0)
        return r;
    return wait(c, m, h, pc);
}

// 0x8C010510: a synchronous read of `nsct` sectors at `fad` through the internal handle.
s32 read_fad(Ctx& c, Memory& m, u32 fad, s32 nsct, u32 buf, u32 pc) {
    const u32 h = iopen(m, internal_handle(m), 0, fad, static_cast<u32>(nsct) * kSector);
    if (!h)
        return E_BUSY;
    return read_sync(c, m, h, nsct, buf, pc);
}

// 0x8C011A4E: starts a stream of `nsct` sectors at the position.
s32 stream_start(Ctx& c, Memory& m, u32 h, s32 nsct) {
    if (!valid(m, h))
        return E_ILLHNDL;
    const u32 w = m.read32(h + H_WORK);
    if (m.read32(w + W_BUSY))
        return 0;
    const s32 cur = static_cast<s32>(m.read32(h + H_FAD) + m.read32(h + H_POS));
    const s32 end = static_cast<s32>(m.read32(h + H_FAD) + m.read32(h + H_SCT));
    if (cur >= end || (cur + nsct > end ? end - cur : nsct) == 0) {
        m.write32(h + H_TRANS, 0);
        w16(m, h + H_STAT, ST_DONE);
        return 0;
    }
    if (cur + nsct > end)
        nsct = end - cur;
    const u32 req = g_drive.stream(static_cast<u32>(cur), nsct);
    if (!req) {
        w16(m, h + H_STAT, ST_DONE);
        return 0;
    }
    m.write32(w + W_BUSY, h);
    m.write32(h + H_REQ, req);
    m.write32(h + H_NSCT, static_cast<u32>(nsct));
    w16(m, h + H_STAT, ST_READ);
    w16(m, h + H_TSTAT, 1);
    w16(m, h + H_CMD, T_STREAM);
    m.write32(h + H_STRM, 0);
    m.write32(h + H_TRANS, 0);
    g_drive.exec_server(c);
    return nsct;
}

// 0x8C011B2C gdFsTrans32: the next `nbytes` of a stream into `buf`.
s32 trans32(Ctx& c, Memory& m, u32 h, u32 nbytes, u32 buf) {
    if (!valid(m, h))
        return E_ILLHNDL;
    if (buf & 31)
        return E_32ALIGN;
    if (nbytes & 31)
        return E_SIZE;
    if (rs16(m, h + H_CMD) != T_STREAM)
        return E_NOTREAD;
    const s32 r = g_drive.dma_trans(c, m.read32(h + H_REQ), buf, nbytes);
    if (r == 0) {
        w16(m, h + H_TSTAT, 1);
        m.write32(h + H_STRM, nbytes);
        m.write32(h + H_TRANS, 0);
        g_drive.exec_server(c);
        return 0;
    }
    return r == -1 ? E_SIZEOVER : E_BUSY;
}

// 0x8C011BF2: a physical seek to the position, waited for.
s32 phys_seek(Ctx& c, Memory& m, u32 h, u32 pc) {
    if (!valid(m, h))
        return E_ILLHNDL;
    const u32 w = m.read32(h + H_WORK);
    if (m.read32(w + W_BUSY))
        return E_BUSY;
    const u32 req = g_drive.seek(m.read32(h + H_FAD) + m.read32(h + H_POS));
    if (!req)
        return E_BUSY;
    m.write32(h + H_REQ, req);
    w16(m, h + H_STAT, ST_SEEK);
    w16(m, h + H_CMD, T_SEEK);
    m.write32(w + W_BUSY, h);
    g_drive.exec_server(c);
    return wait(c, m, h, pc);
}

// 0x8C011CB2 gdFsStop.
s32 stop(Ctx& c, Memory& m, u32 h, u32 pc) {
    if (!valid(m, h))
        return E_ILLHNDL;
    auto active = [&] {
        const s32 s = rs16(m, h + H_STAT);
        return s == ST_READ || s == ST_OTHER;
    };
    if (!active()) {
        w16(m, h + H_STAT, ST_IDLE);
        return 0;
    }
    const u32 w = m.read32(h + H_WORK);
    if (m.read32(w + W_BUSY) != h)
        return E_BUSY;
    const u64 t0 = c.cycles;
    while (m.read32(h + H_ST3) != 1) {
        if (!active()) {
            w16(m, h + H_STAT, ST_IDLE);
            return 0;
        }
        if (c.cycles - t0 > kTimeout)
            return E_TOUT;
        server(c, m, pc);
        spend(c, m, pc, dream::sh4::cost(kPollCycles));
    }
    spend(c, m, pc, kStopSettle);
    if (!active()) {
        w16(m, h + H_STAT, ST_IDLE);
        return 0;
    }
    if (m.read32(w + W_BUSY) != h)
        return E_BUSY;
    g_drive.abort(c, m.read32(h + H_REQ));
    w16(m, h + H_CMD, T_ABORT);
    return wait(c, m, h, pc);
}

// 0x8C0116D4 gdFsClose: Stop for a request in flight (result ignored), then free.
void close(Ctx& c, Memory& m, u32 h, u32 pc) {
    if (!valid(m, h))
        return;
    const s32 s = rs16(m, h + H_STAT);
    if (s == ST_READ || s == ST_SEEK || s == ST_OTHER)
        stop(c, m, h, pc);
    w16(m, h + H_INUSE, 0);
}

// ---- gdFs: directories ---------------------------------------------------------------------------

std::uint8_t name_hash(const char* name) {
    unsigned sum = 218;
    for (const char* p = name; *p; ++p) sum += static_cast<unsigned>(std::toupper(static_cast<unsigned char>(*p)));
    return static_cast<std::uint8_t>(sum >> 2);
}

u32 create_dirhn(Memory& m, u32 buf, u32 max) {
    m.write32(buf + 0, 0);
    m.write32(buf + 4, max);
    m.write32(buf + 8, 0);
    return buf;
}

// One sector into the work struct's buffer at `at` through the internal handle (0x8C010510).
bool read_meta(Ctx& c, Memory& m, u32 fad, u8* out, u32 pc, u32 at) {
    const u32 buf = work(m) + at;
    if (read_fad(c, m, fad, 1, buf, pc) != 0)
        return false;
    for (u32 k = 0; k < kSector; ++k) out[k] = m.read8(buf + k);
    return true;
}

// 0x8C0322BE: loads the directory at `fad` into `dirhn` (cached by FAD), a sector at a time;
// the "." record's size replaces `size`; -12 past the handle's capacity (first `max` kept).
s32 load_dir(Ctx& c, Memory& m, u32 fad, u32 size, u32 dirhn, u32 pc) {
    if (m.read32(dirhn + 8) == fad)
        return 0;
    m.write32(dirhn + 8, fad);
    const u32 max = m.read32(dirhn + 4);
    u32 count = 0;
    bool first = true, more = false;
    u8 sec[kSector];
    for (u32 s = 0; s * kSector < size && !more; ++s) {
        if (!read_meta(c, m, fad + s, sec, pc, W_BUF)) {
            m.write32(dirhn + 8, 0);
            return E_READ;
        }
        for (u32 off = 0; off + 34 <= kSector;) {
            const u32 len = sec[off];
            if (len < 34)
                break;
            if (first) {
                size = le32(sec + off + 10);
                first = false;
            }
            if (count >= max) {
                more = true;
                break;
            }
            const u32 e = dirhn + 16 + 44 * count;
            const u8 flags = sec[off + 25];
            const unsigned nlen = sec[off + 32];
            char name[34] = {};
            if (nlen == 1 && sec[off + 33] == 0) {
                std::strcpy(name, ".");
            } else if (nlen == 1 && sec[off + 33] == 1) {
                std::strcpy(name, "..");
            } else {
                unsigned keep = std::min<unsigned>(nlen, 33);
                if (!(flags & 2))
                    keep = keep >= 2 ? keep - 2 : 0;  // files: the ";1"
                std::memcpy(name, sec + off + 33, keep);
            }
            m.write32(e + 0, le32(sec + off + 2));
            m.write32(e + 4, le32(sec + off + 10));
            m.write8(e + 8, flags);
            m.write8(e + 9, name_hash(name));
            for (unsigned i = 0; i < 34; ++i) m.write8(e + 10 + i, static_cast<u8>(name[i]));
            ++count;
            off += len;
        }
    }
    m.write32(dirhn + 0, count);
    if (more) {
        m.write32(dirhn + 8, 0);
        return E_DIROVER;
    }
    return 0;
}

// 0x8C03238E: `name` in `dirhn`, case-insensitively: the 1-based entry index, 0 if absent.
u32 lookup(Memory& m, u32 dirhn, const std::string& name) {
    const u32 count = m.read32(dirhn);
    for (u32 i = 0; i < count; ++i)
        if (same_name(guest_string(m, dirhn + 16 + 44 * i + 10, 34), name))
            return i + 1;
    return 0;
}

// 0x8C0323FE (and, with a title directory, 0x8C0324FE): an absolute path through the path table,
// read a sector at a time into W+128: the directory's FAD, or 0.
u32 path_table_lookup(Ctx& c, Memory& m, const std::string& path, const std::string& title, u32 pc) {
    const u32 w = work(m);
    const u32 pt_fad = m.read32(w + W_PTFAD), pt_size = m.read32(w + W_PTSIZE);
    std::vector<u8> table((pt_size + kSector - 1) / kSector * kSector);
    for (u32 s = 0; s * kSector < pt_size; ++s)
        if (!read_meta(c, m, pt_fad + s, table.data() + s * kSector, pc, W_PTBUF))
            return 0;
    struct Rec {
        std::string name;
        u32 extent;
        unsigned parent;
    };
    std::vector<Rec> recs;
    for (u32 off = 0; off + 8 <= pt_size;) {
        const unsigned nlen = table[off];
        if (!nlen)
            break;
        recs.push_back({std::string(reinterpret_cast<const char*>(table.data() + off + 8), nlen),
                        le32(table.data() + off + 2), static_cast<unsigned>(table[off + 6] | (table[off + 7] << 8))});
        off += 8 + nlen + (nlen & 1);
    }
    std::vector<std::string> comps;
    if (!title.empty())
        comps.push_back(title);
    for (std::size_t i = 0; i < path.size();) {
        while (i < path.size() && (path[i] == '/' || path[i] == '\\')) ++i;
        std::size_t j = i;
        while (j < path.size() && path[j] != '/' && path[j] != '\\') ++j;
        if (j > i)
            comps.push_back(path.substr(i, j - i));
        i = j;
    }
    unsigned dir = 1;  // the root is record 1
    for (const auto& comp : comps) {
        unsigned found = 0;
        for (unsigned k = 0; k < recs.size() && !found; ++k)
            if (recs[k].parent == dir && same_name(recs[k].name, comp))
                found = k + 1;
        if (!found)
            return 0;
        dir = found;
    }
    return dir >= 1 && dir <= recs.size() ? recs[dir - 1].extent + kFadOffset : 0;
}

// The multi-title context, set up by mount: active when its first s16 is 1.
bool multi_title(Memory& m) {
    const u32 ctx = m.read32(kTitleCtxPtr);
    return ctx && rs16(m, ctx) == 1;
}

// 0x8C010588: a title directory's ".." gets "."'s extent.
void patch_dotdot(Memory& m, u32 d) {
    if (d && m.read32(d) >= 2)
        m.write32(d + 60, m.read32(d + 16));
}

// 0x8C010B82 gdFsLoadDir (name, dirhn or 0).
s32 load_dir_named(Ctx& c, Memory& m, u32 name_ptr, u32 dirhn, u32 pc) {
    const u32 cur = m.read32(work(m) + W_CURDIR);
    if (!dirhn)
        dirhn = cur;
    const std::string name = guest_string(m, name_ptr);
    const bool title = multi_title(m);
    const u32 ctx = m.read32(kTitleCtxPtr);
    s32 r;
    u32 fad;
    if (!name.empty() && (name[0] == '/' || name[0] == '\\')) {
        fad = path_table_lookup(c, m, name, title ? guest_string(m, ctx + 8, 12) : std::string(), pc);
        if (!fad)
            return E_NOTFOUND;  // W+48 untouched (0x8C010BE4 branches straight to the return)
        r = load_dir(c, m, fad, kSector, dirhn, pc);
    } else {
        const u32 idx = lookup(m, cur, name);
        if (!idx)
            return E_NOTFOUND;
        const u32 e = cur + 16 + 44 * (idx - 1);
        if (!(m.read8(e + 8) & 2))
            return E_NOTDIR;
        fad = m.read32(e) + kFadOffset;
        r = load_dir(c, m, fad, m.read32(e + 4), dirhn, pc);
    }
    if (title && m.read32(ctx + 4) == fad && (r == 0 || r == E_DIROVER))
        patch_dotdot(m, dirhn);
    return r;
}

// 0x8C010DB4 gdFsOpen (name, dirhn or 0).
u32 open(Memory& m, u32 name_ptr, u32 dirhn) {
    const u32 w = work(m);
    if (!dirhn)
        dirhn = m.read32(w + W_CURDIR);
    const u32 idx = lookup(m, dirhn, guest_string(m, name_ptr));
    if (!idx) {
        m.write32(w + W_LASTERR, static_cast<u32>(E_NOTFOUND));
        return 0;
    }
    const u32 e = dirhn + 16 + 44 * (idx - 1);
    for (u32 i = 0; i < m.read32(w + W_MAX); ++i) {
        const u32 h = m.read32(w + W_USERH) + kHandleSize * i;
        if (init_handle(m, h, idx, m.read32(e) + kFadOffset, m.read32(e + 4))) {
            m.write32(w + W_LASTERR, 0);
            return h;
        }
    }
    m.write32(w + W_LASTERR, static_cast<u32>(E_NOHNDL));
    return 0;
}

// 0x8C010E30: a handle on `nsct` sectors at `fad` (the game area only).
u32 open_fad(Memory& m, u32 fad, u32 nsct) {
    const u32 w = work(m);
    if (static_cast<s32>(fad) < static_cast<s32>(kHdFad)) {
        m.write32(w + W_LASTERR, static_cast<u32>(E_NOREADABLE));
        return 0;
    }
    for (u32 i = 0; i < m.read32(w + W_MAX); ++i) {
        const u32 h = m.read32(w + W_USERH) + kHandleSize * i;
        if (init_handle(m, h, 0, fad, nsct * kSector)) {
            m.write32(w + W_LASTERR, 0);
            return h;
        }
    }
    m.write32(w + W_LASTERR, static_cast<u32>(E_NOHNDL));
    return 0;
}

// ---- gdFs: the drive's status, TOC, CD-DA -------------------------------------------------------

// 0x8C010F68 GetDrvStat: the drive state, -1 if the drive were busy.
s32 drive_stat(Ctx& c) {
    u32 d[2];
    return g_drive.check_drive(c, d) ? static_cast<s32>(d[0]) : -1;
}

// An internal command on the internal handle: +60, +80 = 4, +72 = type, W+28; the drive's server
// kicked unless the original does not (`exec`).
s32 internal_cmd(Ctx& c, Memory& m, u32 h, u32 req, s32 type, bool exec) {
    if (!req)
        return E_BUSY;
    m.write32(h + H_REQ, req);
    w16(m, h + H_STAT, ST_OTHER);
    w16(m, h + H_CMD, type);
    m.write32(work(m) + W_BUSY, h);
    if (exec)
        g_drive.exec_server(c);
    return 0;
}

// 0x8C010FA0 ReqDrvStat: Q subcode into W+64, not waited for.
s32 req_drive_stat(Ctx& c, Memory& m) {
    const u32 h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return E_BUSY;
    return internal_cmd(c, m, h, g_drive.subcode(1, 14, work(m) + W_BUF), T_DRVSTAT, true);
}

// 0x8C010EDE: the TOC of `area` into `buf`, waited for.
s32 read_toc(Ctx& c, Memory& m, u32 area, u32 buf, u32 pc) {
    const u32 h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return E_BUSY;
    if (internal_cmd(c, m, h, g_drive.toc(area, buf), T_NONE, false))
        return E_BUSY;
    return wait(c, m, h, pc);
}

// 0x8C011590: the disc-change check.
s32 disc_changed(Ctx& c, Memory& m) {
    const u32 w = work(m);
    if (m.read32(w + W_CHANGED) == 1)
        return 1;
    const s32 d = drive_stat(c);
    if (d == 6) {
        m.write32(w + W_CHANGED, 1);
        return 1;
    }
    if (d == 0)
        req_drive_stat(c, m);
    return 0;
}

bool replay_on(Memory& m) { return static_cast<s32>(m.read32(work(m) + W_VERS)) >= 0x10001; }

// 0x8C011070 (by track) and 0x8C01117C (by FAD): CD-DA play, not waited for.
s32 cdda_play(Ctx& c, Memory& m, u32 start, u32 end, u32 repeat, bool by_fad) {
    if (!by_fad) {
        // gdFs reads the context with no null check (guest address 0 before mount); here a null
        // context counts as single-title.
        const u32 ctx = m.read32(kTitleCtxPtr);
        if (ctx && rs16(m, ctx) == 1) {
            const s32 off = rs16(m, ctx + 2) - 4;
            start += static_cast<u32>(off);
            end += static_cast<u32>(off);
        }
        if (static_cast<s32>(start) < 3)
            return E_BUSY;
    } else if (static_cast<s32>(start) < static_cast<s32>(kHdFad)) {
        return E_BUSY;
    }
    const u32 h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return E_BUSY;
    const u32 req = by_fad ? g_drive.play2(start, end, repeat) : g_drive.play(start, end, repeat);
    if (!req)
        return E_BUSY;
    const u32 w = work(m);
    m.write32(h + H_REQ, req);
    w16(m, h + H_STAT, ST_OTHER);
    if (replay_on(m)) {
        w16(m, w + W_REPLAY, 1);
        m.write32(kReplayMode, by_fad ? 2 : 1);
        m.write32(kReplayStart, start);
        m.write32(kReplayEnd, end);
        m.write32(kReplayRepeat, repeat);
        m.write32(h + H_IDX, kReqStatPlay);
        w16(m, h + H_CMD, T_REPLAY);
    } else {
        w16(m, h + H_CMD, T_CDDA);
    }
    m.write32(w + W_BUSY, h);
    g_drive.exec_server(c);
    return 0;
}

// 0x8C011274 / 0x8C01131E: CD-DA pause.
s32 cdda_pause(Ctx& c, Memory& m) {
    const u32 h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return E_BUSY;
    const u32 req = g_drive.pause();
    if (!req)
        return E_BUSY;
    const u32 w = work(m);
    m.write32(h + H_REQ, req);
    w16(m, h + H_STAT, ST_OTHER);
    if (rs16(m, w + W_REPLAY) != 0 && replay_on(m)) {
        m.write32(h + H_IDX, kReqStatPause);
        w16(m, h + H_CMD, T_REPLAY);
    } else {
        w16(m, h + H_CMD, T_CDDA);
    }
    m.write32(w + W_BUSY, h);
    g_drive.exec_server(c);
    return 0;
}

// 0x8C0113DA: CD-DA release, or a replay of the saved range.
s32 cdda_release(Ctx& c, Memory& m, u32 p) {
    const u32 h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return E_BUSY;
    const u32 w = work(m);
    if (rs16(m, w + W_REPLAY) && m.read32(kReqStatPlay) != m.read32(kReqStatPause))
        w16(m, w + W_REPLAY, 0);
    u32 req;
    if (rs16(m, w + W_REPLAY) && replay_on(m)) {
        const u32 a = m.read32(kReplayStart), b = m.read32(kReplayEnd), r = m.read32(kReplayRepeat);
        req = m.read32(kReplayMode) == 1 ? g_drive.play(a, b, r) : g_drive.play2(a, b, r);
        if (req) {
            m.write32(h + H_IDX, kReqStatPlay);
            m.write32(h + H_REQ, req);
            w16(m, h + H_STAT, ST_OTHER);
            w16(m, h + H_CMD, T_REPLAY);
            m.write32(w + W_BUSY, h);
            g_drive.exec_server(c);
        }
    } else {
        req = g_drive.release(p);
        if (req)
            internal_cmd(c, m, h, req, T_CDDA, true);
    }
    return req ? 0 : E_BUSY;
}

// 0x8C0114FA: the CD-DA position into `out` (five s32), not waited for.
s32 cdda_position(Ctx& c, Memory& m, u32 out) {
    const u32 h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return E_BUSY;
    const u32 req = g_drive.subcode(0, 100, work(m) + W_BUF);
    if (!req)
        return E_BUSY;
    m.write32(h + H_FAD, out);
    return internal_cmd(c, m, h, req, T_SUBCODE, true);
}

// 0x8C031C80: the raw subcode at `buf` (format 0) -> {track, rel M, S, F, ctrl<<24 | abs FAD,
// index}; false where gdFs's parser fails (header, ADR, CRC).
bool parse_subcode(Memory& m, u32 buf, s32 o[6]) {
    if (static_cast<std::int8_t>(m.read8(buf)) != 0)
        return false;
    if (static_cast<std::int8_t>(m.read8(buf + 2)) * 256 + static_cast<std::int8_t>(m.read8(buf + 3)) != 100)
        return false;
    u8 q[12] = {};
    for (unsigned i = 0; i < 12; ++i) {
        for (unsigned b = 0; b < 8; ++b)
            if (m.read8(buf + 4 + 8 * i + b) & 0x40)
                q[i] |= static_cast<u8>(0x80 >> b);
        if ((q[0] & 0x0F) != 1)
            return false;
    }
    std::uint16_t crc = 0;
    for (unsigned i = 0; i < 10; ++i) {
        crc ^= static_cast<std::uint16_t>(q[i]) << 8;
        for (int k = 0; k < 8; ++k) crc = static_cast<std::uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    if (static_cast<std::uint16_t>(~crc) != static_cast<std::uint16_t>(q[10] << 8 | q[11]))
        return false;
    auto bcd = [](u8 v) { return static_cast<s32>((v >> 4) * 10 + (v & 15)); };
    o[0] = bcd(q[1]);
    o[1] = bcd(q[3]);
    o[2] = bcd(q[4]);
    o[3] = bcd(q[5]);
    o[4] = static_cast<s32>(static_cast<u32>(q[0]) << 24 | static_cast<u32>((bcd(q[7]) * 60 + bcd(q[8])) * 75 + bcd(q[9])));
    o[5] = bcd(q[2]);
    return true;
}

// ---- gdFs: init, mount, reset, finish -----------------------------------------------------------

void register_handlers(Ctx& c, Memory& m, u32 w, u32 from) {
    if (!m.read32(kChainVblank))
        m.write32(kChainVblank, call_guest(c, m, kChainAdd, from, kEvVblank, kVblankHandler, 80, w));
    if (!m.read32(kChainDma))
        m.write32(kChainDma, call_guest(c, m, kChainAdd, from, kEvGdDma, kDmaEndHandler, 16, w));
}

// 0x8C01089C gdFsInit (max_open, work, max_dirent, dirbuf, API table).
void init(Ctx& c, Memory& m, u32 max_open, u32 w, u32 max_dirent, u32 dirbuf, u32 api) {
    const u32 cur = work(m);
    if (cur && rs16(m, cur + W_MOUNTED) == 1)
        return;
    if (w & 31)
        return;
    g_drive.init_system();
    m.write32(kWorkPtr, w);
    w16(m, w + W_MOUNTED, 0);
    m.write32(w + W_SERVING, 0);
    m.write32(w + W_MAX, max_open);
    m.write32(w + W_CURDIR, create_dirhn(m, dirbuf, max_dirent));
    m.write32(w + W_DRIVE, kDriveTable);
    m.write32(w + W_BUSY, 0);
    w16(m, w + W_REPLAY, 0);
    m.write32(w + W_ERRF, 0);
    m.write32(w + W_CHANGED, 0);
    m.write32(w + W_API, api);
    for (u32 i = 0; i <= max_open; ++i) {
        const u32 h = w + W_HANDLES + kHandleSize * i;
        w16(m, h + H_INUSE, 0);
        m.write32(h + H_WORK, w);
    }
    m.write32(w + W_INTH, w + W_HANDLES);
    m.write32(w + W_USERH, w + W_HANDLES + kHandleSize);
    init_handle(m, w + W_HANDLES, 0, 0, 0);
    register_handlers(c, m, w, 0x8C01089Cu);
}

// 0x8C01059E mount, with interrupts masked throughout, as gdFs runs it.
s32 mount(Ctx& c, Memory& m, u32 pc) {
    Masked masked(c);
    const u32 w = work(m);
    s32 deferred = 0;
    auto ret = [&](s32 r) { return deferred ? deferred : r; };
    u32 h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return ret(E_RESET);
    if (internal_cmd(c, m, h, g_drive.init_cmd(), T_NONE, false))
        return ret(E_RESET);
    const s32 waited = wait(c, m, h, pc);
    u32 cd[2];
    if (!g_drive.check_drive(c, cd))
        return ret(E_RESET);
    if (waited != 0)
        return ret(waited == E_NOTREADY && cd[0] == 6 ? E_TRAYOPEN : waited);
    if (cd[1] != 0x80)
        return ret(E_DISC);
    h = iopen(m, internal_handle(m), 0, 0, 0);
    if (!h)
        return ret(E_RESET);
    m.write32(w + W_VERS, 0);
    if (!internal_cmd(c, m, h, g_drive.version(kVersBuf), T_NONE, false) && wait(c, m, h, pc) == 0) {
        auto digit = [&](u32 i) { return static_cast<u32>(static_cast<s32>(static_cast<std::int8_t>(m.read8(kVersBuf + i))) - '0'); };
        m.write32(w + W_VERS, m.read32(w + W_VERS) | digit(12) << 16);
        m.write32(w + W_VERS, m.read32(w + W_VERS) | digit(14) << 8);
        m.write32(w + W_VERS, m.read32(w + W_VERS) | digit(15));
    }
    read_toc(c, m, 1, w + W_BUF, pc);
    if ((m.read32(w + W_BUF + 8) & 0xFFFFFFu) != kHdFad)
        deferred = E_TOCMISMATCH;
    u8 pvd[kSector];
    if (!read_meta(c, m, kPvdFad, pvd, pc, W_BUF))
        return ret(E_MOUNT);
    if (std::memcmp(pvd + 1, "CD001", 5) != 0)
        return ret(E_DISC);
    m.write32(w + W_PTFAD, le32(pvd + 140) + kFadOffset);
    m.write32(w + W_PTSIZE, le32(pvd + 132));
    m.write32(kTitleCtxPtr, kTitleCtx);
    w16(m, kTitleCtx, static_cast<std::int8_t>(m.read8(kIpTitleFlag)));
    if (rs16(m, kTitleCtx) == 1) {
        w16(m, kTitleCtx + 2, static_cast<std::int8_t>(m.read8(kIpTitleOfs)));
        m.write32(kTitleCtx + 4, 0);
        unsigned i = 0;
        for (; i < 12; ++i) {
            const u8 ch = m.read8(kIpTitleName + i);
            if (ch == ' ')
                break;
            m.write8(kTitleCtx + 8 + i, ch);
        }
        m.write8(kTitleCtx + 8 + i, 0);
    }
    const u32 dir = m.read32(w + W_CURDIR);
    m.write32(dir + 8, 0);
    s32 r = load_dir(c, m, le32(pvd + 158) + kFadOffset, le32(pvd + 166), dir, pc);
    if (r == E_DIROVER)
        return ret(E_DIROVER);
    if (r)
        return ret(E_MOUNT);
    if (m.read32(dir) == 0)
        return ret(E_DISC);
    if (rs16(m, kTitleCtx) == 1) {
        const u32 idx = lookup(m, dir, guest_string(m, kTitleCtx + 8, 13));
        if (!idx)
            return ret(E_MOUNT);
        const u32 e = dir + 16 + 44 * (idx - 1);
        const u32 fad = m.read32(e) + kFadOffset;
        m.write32(kTitleCtx + 4, fad);
        r = load_dir(c, m, fad, 1, dir, pc);
        if (r == E_DIROVER)
            return ret(E_DIROVER);
        if (r)
            return ret(E_MOUNT);
        if (m.read32(dir) == 0)
            return ret(E_DISC);
        patch_dotdot(m, dir);
    }
    w16(m, w + W_MOUNTED, 1);
    return ret(0);
}

// 0x8C0109E8 gdFsReset.
s32 reset(Ctx& c, Memory& m, u32 pc) {
    const u32 w = work(m);
    if (!w)
        return -1;
    for (u32 i = 0; i <= m.read32(w + W_MAX); ++i) close(c, m, w + W_HANDLES + kHandleSize * i, pc);
    {
        Masked masked(c);
        g_drive.init_system();
        w16(m, w + W_MOUNTED, 0);
        m.write32(w + W_SERVING, 0);
        m.write32(w + W_BUSY, 0);
        w16(m, w + W_REPLAY, 0);
        m.write32(w + W_CHANGED, 0);
        for (u32 i = 0; i <= m.read32(w + W_MAX); ++i) {
            const u32 hh = w + W_HANDLES + kHandleSize * i;
            w16(m, hh + H_INUSE, 0);
            m.write32(hh + H_WORK, w);
        }
        m.write32(w + W_INTH, w + W_HANDLES);
        m.write32(w + W_USERH, w + W_HANDLES + kHandleSize);
        init_handle(m, w + W_HANDLES, 0, 0, 0);
        register_handlers(c, m, w, 0x8C0109E8u);
    }
    return mount(c, m, pc);
}

// 0x8C010B20 gdFsFinish.
void finish(Ctx& c, Memory& m, u32 pc) {
    for (u32 chain : {kChainVblank, kChainDma})
        if (const u32 hh = m.read32(chain))
            call_guest(c, m, kChainDel, 0x8C010B20u, hh);
    m.write32(kChainVblank, 0);
    m.write32(kChainDma, 0);
    if (const u32 w = work(m))
        for (u32 i = 0; i <= m.read32(w + W_MAX); ++i) close(c, m, w + W_HANDLES + kHandleSize * i, pc);
    m.write32(kWorkPtr, 0);
}

// ---- guards ---------------------------------------------------------------------------------------

// Functions only gdFs itself calls (its helpers, its lower layer, its BIOS stubs): with every
// entry point native they cannot run. One that does is said once.
struct Guard {
    const char* what;
    bool said = false;
};
Guard g_guard_core{"gdFs helper"}, g_guard_lower{"gdFs lower layer"}, g_guard_bios{"BIOS GD-ROM stub"};

bool guard(Guard& g, const Ctx& c) {
    if (!g_on)
        return false;
    if (!g.said) {
        g.said = true;
        std::fprintf(stderr, "native gdFs: a %s ran (called from %08x)\n", g.what, c.pr);
    }
    return false;
}


}  // namespace

void native_gdfs_start(dream::System& sys) {
    g_sys = &sys;
    g_on = sys.disc != nullptr;  // a run without a disc has no drive to read
    if (!g_on)
        return;
    g_drive.start(sys, true);
    sys.no_gdrom = true;  // the BIOS stand-in refuses any GD-ROM call that still reaches it
}

bool gdfs_on() {
    return g_on && g_sys;
}

}  // namespace rez

// ---- the hooks ------------------------------------------------------------------------------------
// Every gdFs function: the entry points native, the rest guarded (game/rez.toml lists them).

#define GDFS_HOOK(name) DC_HOOK_ENTRY(name)
#define ON() if (!rez::gdfs_on()) return false

using rez::s32;
using rez::u32;

namespace {

// The public stubs: when gdFs is initialised, a tail jump through the API table, which lands on
// a native implementation; otherwise 0 (or -1 for mount and reset).
bool api_stub(dream::sh4::Ctx& c, dream::Memory& m, u32 at, u32 slot, u32 uninit) {
    const u32 api = m.read32(rez::kApiPtr);
    if (!api) {
        c.r[0] = uninit;
        return true;
    }
    c.pc = at;
    dream::sh4::call_indirect(c, m, m.read32(api + slot));
    return true;
}

}  // namespace

#define API_STUB(name, at, slot, uninit) \
    GDFS_HOOK(name) {                   \
        ON();                           \
        return api_stub(c, m, at, slot, uninit); \
    }

API_STUB(gdfs_api_read_fad, 0x8C011EB4u, 32, 0)
API_STUB(gdfs_api_set_error, 0x8C011ECAu, 28, 0)
API_STUB(gdfs_api_last_error, 0x8C011EE0u, 80, 0)
API_STUB(gdfs_api_mount, 0x8C011F14u, 16, ~0u)
API_STUB(gdfs_api_reset, 0x8C011F2Au, 20, ~0u)
API_STUB(gdfs_api_init_handle, 0x8C011F5Eu, 60, 0)
API_STUB(gdfs_api_load_dir, 0x8C011F74u, 36, 0)
API_STUB(gdfs_api_load_dir2, 0x8C011F8Au, 40, 0)
API_STUB(gdfs_api_set_dir, 0x8C011FA0u, 44, 0)
API_STUB(gdfs_api_get_dir_info, 0x8C011FB6u, 48, 0)
API_STUB(gdfs_api_open, 0x8C011FCCu, 64, 0)
API_STUB(gdfs_api_open2, 0x8C011FE4u, 68, 0)
API_STUB(gdfs_api_open_fad, 0x8C011FFCu, 72, 0)
API_STUB(gdfs_api_busy_handle, 0x8C012014u, 52, 0)
API_STUB(gdfs_api_internal_handle, 0x8C01202Au, 56, 0)
API_STUB(gdfs_api_server, 0x8C012040u, 76, 0)
API_STUB(gdfs_api_err_func_all, 0x8C012056u, 120, 0)
API_STUB(gdfs_api_read_toc, 0x8C01206Cu, 84, 0)
API_STUB(gdfs_api_drive_stat, 0x8C012084u, 88, 0)
API_STUB(gdfs_api_req_drive_stat, 0x8C01209Cu, 92, 0)
API_STUB(gdfs_api_play, 0x8C0120B4u, 96, 0)
API_STUB(gdfs_api_play_fad, 0x8C0120CCu, 100, 0)
API_STUB(gdfs_api_pause, 0x8C0120E4u, 104, 0)
API_STUB(gdfs_api_pause2, 0x8C012106u, 108, 0)
API_STUB(gdfs_api_release, 0x8C01211Eu, 112, 0)
API_STUB(gdfs_api_position, 0x8C012136u, 116, 0)
API_STUB(gdfs_api_disc_changed, 0x8C01214Eu, 124, 0)

GDFS_HOOK(gdfs_api_init) {  // 0x8C011EF8 (and the trampoline 0x8C01088E): the API table, then init
    ON();
    m.write32(rez::kApiPtr, rez::kApiTable);
    rez::init(c, m, c.r[4], c.r[5], c.r[6], c.r[7], rez::kApiTable);
    return true;
}
GDFS_HOOK(gdfs_api_finish) {  // 0x8C011F40: Finish, then the API table pointer cleared
    ON();
    if (m.read32(rez::kApiPtr)) {
        rez::finish(c, m, 0x8C011F40u);
        m.write32(rez::kApiPtr, 0);
    }
    c.r[0] = 0;
    return true;
}

GDFS_HOOK(gdfs_init) {  // 0x8C01089C (slot +12): (max_open, work, max_dirent, dirbuf; table on the stack)
    ON();
    rez::init(c, m, c.r[4], c.r[5], c.r[6], c.r[7], m.read32(c.r[15]));
    return true;
}
GDFS_HOOK(gdfs_mount) {  // 0x8C0109BA (slot +16) and 0x8C01059E
    ON();
    c.r[0] = static_cast<u32>(rez::mount(c, m, 0x8C01059Eu));
    return true;
}
GDFS_HOOK(gdfs_reset) {  // 0x8C0109E8 (slot +20)
    ON();
    c.r[0] = static_cast<u32>(rez::reset(c, m, 0x8C0109E8u));
    return true;
}
GDFS_HOOK(gdfs_finish) {  // 0x8C010B20 (slot +24)
    ON();
    rez::finish(c, m, 0x8C010B20u);
    return true;
}
GDFS_HOOK(gdfs_set_error) {  // 0x8C010268 (slot +28)
    ON();
    m.write32(rez::work(m) + rez::W_LASTERR, c.r[4]);
    c.r[0] = 1;
    return true;
}
GDFS_HOOK(gdfs_read_fad) {  // 0x8C010510 (slot +32): (fad, nsct, buf), synchronous
    ON();
    c.r[0] = static_cast<u32>(rez::read_fad(c, m, c.r[4], static_cast<s32>(c.r[5]), c.r[6], 0x8C010510u));
    return true;
}
GDFS_HOOK(gdfs_load_dir) {  // 0x8C010B82 (slot +36) and its alias 0x8C010C8E (slot +40)
    ON();
    c.r[0] = static_cast<u32>(rez::load_dir_named(c, m, c.r[4], c.r[5], 0x8C010B82u));
    return true;
}
GDFS_HOOK(gdfs_set_dir) {  // 0x8C010C92 (slot +44): copies a directory handle into the current one
    ON();
    const u32 cur = m.read32(rez::work(m) + rez::W_CURDIR), src = c.r[4];
    const u32 count = m.read32(src), max = m.read32(cur + 4), n = std::min(count, max);
    for (u32 i = 0; i < n * 44; i += 4) m.write32(cur + 16 + i, m.read32(src + 16 + i));
    m.write32(cur, n);
    m.write32(cur + 8, m.read32(src + 8));
    c.r[0] = count > max ? static_cast<u32>(rez::E_DIROVER) : 0;
    return true;
}
GDFS_HOOK(gdfs_get_dir_info) {  // 0x8C010CFA (slot +48): (name, info*) -> FAD, size, flags
    ON();
    const u32 cur = m.read32(rez::work(m) + rez::W_CURDIR);
    const u32 idx = rez::lookup(m, cur, rez::guest_string(m, c.r[4]));
    if (!idx) {
        c.r[0] = static_cast<u32>(rez::E_NOTFOUND);
        return true;
    }
    const u32 e = cur + 16 + 44 * (idx - 1);
    m.write32(c.r[5], m.read32(e) + rez::kFadOffset);
    m.write32(c.r[5] + 4, m.read32(e + 4));
    m.write8(c.r[5] + 8, m.read8(e + 8));
    c.r[0] = 0;
    return true;
}
GDFS_HOOK(gdfs_busy_handle) {  // 0x8C010ECE (slot +52)
    ON();
    c.r[0] = m.read32(rez::work(m) + rez::W_BUSY);
    return true;
}
GDFS_HOOK(gdfs_internal_handle) {  // 0x8C010ED6 (slot +56)
    ON();
    c.r[0] = m.read32(rez::work(m) + rez::W_INTH);
    return true;
}
GDFS_HOOK(gdfs_init_handle) {  // 0x8C010D44 (slot +60)
    ON();
    c.r[0] = rez::init_handle(m, c.r[4], c.r[5], c.r[6], c.r[7]);
    return true;
}
GDFS_HOOK(gdfs_open) {  // 0x8C010DB4 (slot +64) and its alias 0x8C010E2C (slot +68)
    ON();
    c.r[0] = rez::open(m, c.r[4], c.r[5]);
    return true;
}
GDFS_HOOK(gdfs_open_fad) {  // 0x8C010E30 (slot +72): (fad, nsct)
    ON();
    c.r[0] = rez::open_fad(m, c.r[4], c.r[5]);
    return true;
}
GDFS_HOOK(gdfs_server) {  // 0x8C010EA2 (slot +76): from VBlank, gdFsServer and the wait loops
    ON();
    c.r[0] = static_cast<u32>(rez::server(c, m, 0x8C010EA2u));
    return true;
}
GDFS_HOOK(gdfs_vblank) {  // 0x8C010568, gdFs's VBlank handler (r4 = W): the server unless it runs
    ON();
    if (m.read32(c.r[4] + rez::W_SERVING) != 1)
        rez::server(c, m, 0x8C010568u);
    return true;
}
GDFS_HOOK(gdfs_dma_end) {  // 0x8C01054E, gdFs's GD-DMA-end handler (r4 = W)
    ON();
    const u32 h = m.read32(c.r[4] + rez::W_BUSY);
    if (h && m.read32(h + rez::H_TREND))
        rez::g_drive.g1_dma_end(c, m, m.read32(h + rez::H_TREND), m.read32(h + rez::H_TROBJ));
    else
        rez::g_drive.g1_dma_end(c, m, 0, 0);
    return true;
}
GDFS_HOOK(gdfs_last_error) {  // 0x8C010EC6 (slot +80)
    ON();
    c.r[0] = m.read32(rez::work(m) + rez::W_LASTERR);
    return true;
}
GDFS_HOOK(gdfs_read_toc) {  // 0x8C010EDE (slot +84): (area, buf)
    ON();
    c.r[0] = static_cast<u32>(rez::read_toc(c, m, c.r[4], c.r[5], 0x8C010EDEu));
    return true;
}
GDFS_HOOK(gdfs_drive_stat) {  // 0x8C010F68 (slot +88); Rez calls it every frame
    ON();
    c.r[0] = static_cast<u32>(rez::drive_stat(c));
    return true;
}
GDFS_HOOK(gdfs_req_drive_stat) {  // 0x8C010FA0 (slot +92)
    ON();
    c.r[0] = static_cast<u32>(rez::req_drive_stat(c, m));
    return true;
}
GDFS_HOOK(gdfs_play) {  // 0x8C011070 (slot +96): (first track, last track, repeat)
    ON();
    c.r[0] = static_cast<u32>(rez::cdda_play(c, m, c.r[4], c.r[5], c.r[6], false));
    return true;
}
GDFS_HOOK(gdfs_play_fad) {  // 0x8C01117C (slot +100): (start FAD, end FAD, repeat)
    ON();
    c.r[0] = static_cast<u32>(rez::cdda_play(c, m, c.r[4], c.r[5], c.r[6], true));
    return true;
}
GDFS_HOOK(gdfs_pause) {  // 0x8C011274 (slot +104) and 0x8C01131E (slot +108)
    ON();
    c.r[0] = static_cast<u32>(rez::cdda_pause(c, m));
    return true;
}
GDFS_HOOK(gdfs_release) {  // 0x8C0113DA (slot +112): (p)
    ON();
    c.r[0] = static_cast<u32>(rez::cdda_release(c, m, c.r[4]));
    return true;
}
GDFS_HOOK(gdfs_position) {  // 0x8C0114FA (slot +116): (out)
    ON();
    c.r[0] = static_cast<u32>(rez::cdda_position(c, m, c.r[4]));
    return true;
}
GDFS_HOOK(gdfs_err_func_all) {  // 0x8C011030 (slot +120): (func, obj)
    ON();
    const u32 w = rez::work(m);
    m.write32(w + rez::W_ERRF, c.r[4]);
    m.write32(w + rez::W_ERROBJ, c.r[5]);
    return true;
}
GDFS_HOOK(gdfs_disc_changed) {  // 0x8C011590 (slot +124)
    ON();
    c.r[0] = static_cast<u32>(rez::disc_changed(c, m));
    return true;
}
GDFS_HOOK(gdfs_wait) {  // 0x8C0115D0 (internal): (h)
    ON();
    c.r[0] = static_cast<u32>(rez::wait(c, m, c.r[4], 0x8C0115D0u));
    return true;
}
GDFS_HOOK(gdfs_internal_open) {  // 0x8C011662: (h, p4, fad, nbytes)
    ON();
    c.r[0] = rez::iopen(m, c.r[4], c.r[5], c.r[6], c.r[7]);
    return true;
}
GDFS_HOOK(gdfs_create_dirhn) {  // 0x8C0116B0: (buf, max)
    ON();
    c.r[0] = rez::create_dirhn(m, c.r[4], c.r[5]);
    return true;
}
GDFS_HOOK(gdfs_change_dir) {  // 0x8C0116BC: (name) = LoadDir(name, current)
    ON();
    c.r[0] = static_cast<u32>(rez::load_dir_named(c, m, c.r[4], 0, 0x8C0116BCu));
    return true;
}
GDFS_HOOK(gdfs_close) {  // 0x8C0116D4: (h)
    ON();
    rez::close(c, m, c.r[4], 0x8C0116D4u);
    return true;
}
GDFS_HOOK(gdfs_file_size) {  // 0x8C01170C: (h, s32*) bytes
    ON();
    if (!rez::valid(m, c.r[4])) {
        c.r[0] = 0;
        return true;
    }
    m.write32(c.r[5], m.read32(c.r[4] + rez::H_SIZE));
    c.r[0] = 1;
    return true;
}
GDFS_HOOK(gdfs_file_sct_size) {  // 0x8C011726: (h, s32*) sectors
    ON();
    if (!rez::valid(m, c.r[4])) {
        c.r[0] = 0;
        return true;
    }
    m.write32(c.r[5], m.read32(c.r[4] + rez::H_SCT));
    c.r[0] = 1;
    return true;
}
GDFS_HOOK(gdfs_file_fad) {  // 0x8C011740: (h, s32*) start FAD
    ON();
    if (!rez::valid(m, c.r[4])) {
        c.r[0] = 0;
        return true;
    }
    m.write32(c.r[5], m.read32(c.r[4] + rez::H_FAD));
    c.r[0] = 1;
    return true;
}
GDFS_HOOK(gdfs_seek) {  // 0x8C01175A: (h, ofs, org) in sectors
    ON();
    const u32 h = c.r[4];
    if (!rez::valid(m, h)) {
        c.r[0] = static_cast<u32>(rez::E_ILLHNDL);
        return true;
    }
    const s32 ofs = static_cast<s32>(c.r[5]);
    s32 pos;
    switch (c.r[6]) {
        case 0: pos = ofs; break;
        case 1: pos = static_cast<s32>(m.read32(h + rez::H_POS)) + ofs; break;
        case 2: pos = static_cast<s32>(m.read32(h + rez::H_SCT)) + ofs; break;
        default: c.r[0] = static_cast<u32>(rez::E_SEEK); return true;
    }
    if (pos < 0 || pos > static_cast<s32>(m.read32(h + rez::H_SCT))) {
        c.r[0] = static_cast<u32>(rez::E_OFS);
        return true;
    }
    m.write32(h + rez::H_POS, static_cast<u32>(pos));
    c.r[0] = 0;
    return true;
}
GDFS_HOOK(gdfs_tell) {  // 0x8C0117A6: (h) position in sectors
    ON();
    c.r[0] = rez::valid(m, c.r[4]) ? m.read32(c.r[4] + rez::H_POS) : 0;
    return true;
}
GDFS_HOOK(gdfs_eof) {  // 0x8C0117BC: (h, s32*)
    ON();
    if (!rez::valid(m, c.r[4])) {
        c.r[0] = static_cast<u32>(rez::E_ILLHNDL);
        return true;
    }
    m.write32(c.r[5], m.read32(c.r[4] + rez::H_POS) >= m.read32(c.r[4] + rez::H_SCT) ? 1 : 0);
    c.r[0] = 0;
    return true;
}
GDFS_HOOK(gdfs_trans_mode) {  // 0x8C0117E4: (h, mode)
    ON();
    if (!rez::valid(m, c.r[4])) {
        c.r[0] = static_cast<u32>(rez::E_ILLHNDL);
        return true;
    }
    rez::w16(m, c.r[4] + rez::H_MODE, static_cast<s32>(c.r[5]));
    c.r[0] = 0;
    return true;
}
GDFS_HOOK(gdfs_read) {  // 0x8C0117FE: gdFsRead (h, nsct, buf), blocking
    ON();
    c.r[0] = static_cast<u32>(rez::read_sync(c, m, c.r[4], static_cast<s32>(c.r[5]), c.r[6], 0x8C0117FEu));
    return true;
}
GDFS_HOOK(gdfs_req_rd_pio) {  // 0x8C01188C: ReqRd without the alignment check, always PIO
    ON();
    c.r[0] = static_cast<u32>(rez::req_read(c, m, c.r[4], static_cast<s32>(c.r[5]), c.r[6], true));
    return true;
}
GDFS_HOOK(gdfs_req_rd32) {  // 0x8C011964: gdFsReqRd32 (h, nsct, buf)
    ON();
    c.r[0] = static_cast<u32>(rez::req_read(c, m, c.r[4], static_cast<s32>(c.r[5]), c.r[6], false));
    return true;
}
GDFS_HOOK(gdfs_stream_start) {  // 0x8C011A4E: (h, nsct)
    ON();
    c.r[0] = static_cast<u32>(rez::stream_start(c, m, c.r[4], static_cast<s32>(c.r[5])));
    return true;
}
GDFS_HOOK(gdfs_trans32) {  // 0x8C011B2C gdFsTrans32: (h, nbytes, buf)
    ON();
    c.r[0] = static_cast<u32>(rez::trans32(c, m, c.r[4], c.r[5], c.r[6]));
    return true;
}
GDFS_HOOK(gdfs_phys_seek) {  // 0x8C011BF2: (h)
    ON();
    c.r[0] = static_cast<u32>(rez::phys_seek(c, m, c.r[4], 0x8C011BF2u));
    return true;
}
GDFS_HOOK(gdfs_trans_stat) {  // 0x8C011BD8: (h) 0 ready, 1 busy, 2 complete
    ON();
    c.r[0] = rez::valid(m, c.r[4]) ? static_cast<u32>(rez::rs16(m, c.r[4] + rez::H_TSTAT)) : static_cast<u32>(rez::E_ILLHNDL);
    return true;
}
GDFS_HOOK(gdfs_stat) {  // 0x8C011C84: gdFsGetStat (h)
    ON();
    c.r[0] = static_cast<u32>(rez::get_stat(m, c.r[4]));
    return true;
}
GDFS_HOOK(gdfs_trans_count) {  // 0x8C011C9C: (h) bytes transferred
    ON();
    c.r[0] = rez::valid(m, c.r[4]) ? m.read32(c.r[4] + rez::H_TRANS) : 0;
    return true;
}
GDFS_HOOK(gdfs_stop) {  // 0x8C011CB2: gdFsStop (h)
    ON();
    c.r[0] = static_cast<u32>(rez::stop(c, m, c.r[4], 0x8C011CB2u));
    return true;
}
GDFS_HOOK(gdfs_err_stat) {  // 0x8C011DE0: gdFsGetErrStat (h)
    ON();
    c.r[0] = rez::valid(m, c.r[4]) ? static_cast<u32>(rez::rs16(m, c.r[4] + rez::H_ERR)) : 0;
    return true;
}
GDFS_HOOK(gdfs_rd_end_func) {  // 0x8C011E08: (h, func, obj)
    ON();
    if (rez::valid(m, c.r[4])) {
        m.write32(c.r[4] + rez::H_RDEND, c.r[5]);
        m.write32(c.r[4] + rez::H_RDOBJ, c.r[6]);
    }
    return true;
}
GDFS_HOOK(gdfs_tr_end_func) {  // 0x8C011E40: (h, func, obj); 0 for a bad handle
    ON();
    if (!rez::valid(m, c.r[4])) {
        c.r[0] = 0;
        return true;
    }
    m.write32(c.r[4] + rez::H_TREND, c.r[5]);
    m.write32(c.r[4] + rez::H_TROBJ, c.r[6]);
    return true;
}
GDFS_HOOK(gdfs_err_func) {  // 0x8C011E78: (h, func, obj)
    ON();
    if (rez::valid(m, c.r[4])) {
        m.write32(c.r[4] + rez::H_ERRF, c.r[5]);
        m.write32(c.r[4] + rez::H_ERROBJ, c.r[6]);
    }
    return true;
}

GDFS_HOOK(gdfs_guard_core) { return rez::guard(rez::g_guard_core, c); }
GDFS_HOOK(gdfs_guard_lower) { return rez::guard(rez::g_guard_lower, c); }
GDFS_HOOK(gdfs_guard_bios) { return rez::guard(rez::g_guard_bios, c); }
