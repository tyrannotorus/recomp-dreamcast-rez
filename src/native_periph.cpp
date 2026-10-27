// Native peripherals: Katana's Maple server, pd, pdVib, pdLcd and bu, replaced at library level
// with no Maple bus underneath. A model of what is plugged in follows the server's connection rules
// transfer by transfer; each transfer ends on the Maple DMA-end interrupt, and bu operations
// take as many frames as on the bus, so the game sees the original timing. Saves go to the VMU
// image, block for block as bu writes them. The rest of these libraries is guarded.
#include "native_periph.h"
#include "native_irq.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dream/runtime/maple/maple.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"
#include "dreamcomp/hook.h"

namespace rez {
namespace {

using dream::Memory;
using dream::sh4::Ctx;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

dream::System* g_sys = nullptr;
bool g_on = true;


// ---- guest memory ----------------------------------------------------------------------------------

// Katana's Maple server.
constexpr u32 kWorkPtr = 0x8C13EE74u, kWork = 0x8C13DD78u;
constexpr u32 kInfoRec = 0x8C13E1C4u;   // 24 x 120: device-info records ("I")
constexpr u32 kRec24 = 0x8C13CEE0u;     // 24 x 24: the per-unit records clients read
constexpr u32 kCallbacks = 0x8C13CE60u; // 32 periodic callbacks
constexpr u32 kOneShot = 0x8C13D160u;   // 48 x 12 one-shot callbacks
constexpr u32 kStats = 0x8C13D120u;     // statistics, 0x38 bytes
constexpr u32 kInited = 0x8C13D3A0u;
constexpr u32 kChainVblank = 0x8C13CE50u, kChainDmaEnd = 0x8C13CE54u, kChainVbOver = 0x8C13CE5Cu;
constexpr u32 kVblankHandler = 0x8C033928u, kDmaEndHandler = 0x8C033988u, kVbOverHandler = 0x8C033B20u;
constexpr u32 kChainAdd = 0x8C0326B0u;
constexpr u32 kEvVblank = 0x0900, kEvDmaEnd = 0x0A00, kEvVbOver = 0x0A20;
constexpr u32 kIml4Nrm = 0xA05F6920u, kIstNrm = 0xA05F6900u;
// pd.
constexpr u32 kPds = 0x8C12000Cu;  // 24 x 52 PDS_PERIPHERAL
constexpr u32 kPdLogic = 0x8C1204ECu, kPdPressAt = 0x8C1204F0u, kPdReleaseAt = 0x8C1204F1u;
constexpr u32 kPdLState = 0x8C1204F2u, kPdRState = 0x8C12050Au;
constexpr u32 kNoDevice = 0x8C0E7FDCu;
// pdLcd.
constexpr u32 kLcdPortUnit = 0x8C0E81B4u, kLcdDirTable = 0x8C0E818Cu;
// bu.
constexpr u32 kBuRunning = 0x8C11FC08u;
constexpr u32 kBuIconBytes = 0x8C11FC0Cu, kBuEyeBytes = 0x8C11FC1Cu;
constexpr u32 kBuWeekday = 0x8C10EA34u;

constexpr unsigned kUnits = 24, kPorts = 4;
constexpr u8 kAddr[6] = {0x20, 0x01, 0x02, 0x04, 0x08, 0x10};  // Maple address of each slot

// The device-info records the server's parser writes for a controller, VMU and vibration pack:
// function bits, three function-definition words, slot and region and directions, product name,
// licence, power.
constexpr u8 kInfoController[120] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x06, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff,
    0x00, 0x00, 'D',  'r',  'e',  'a',  'm',  'c',  'a',  's',  't',  ' ',  'C',  'o',  'n',  't',  'r',  'o',
    'l',  'l',  'e',  'r',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  0x00, 0x00, 'P',  'r',
    'o',  'd',  'u',  'c',  'e',  'd',  ' ',  'B',  'y',  ' ',  'o',  'r',  ' ',  'U',  'n',  'd',  'e',  'r',
    ' ',  'L',  'i',  'c',  'e',  'n',  's',  'e',  ' ',  'F',  'r',  'o',  'm',  ' ',  'S',  'E',  'G',  'A',
    ' ',  'E',  'N',  'T',  'E',  'R',  'P',  'R',  'I',  'S',  'E',  'S',  ',',  'L',  'T',  'D',  '.',  ' ',
    ' ',  ' ',  ' ',  ' ',  0x00, 0x00, 0x00, 0x00, 0xae, 0x01, 0xf4, 0x01};
constexpr u8 kInfoVmu[120] = {
    0x0e, 0x00, 0x00, 0x00, 0x7e, 0x7e, 0x3f, 0x40, 0x00, 0x05, 0x10, 0x00, 0x00, 0x0f, 0x41, 0x00, 0x00, 0xff,
    0x00, 0x00, 'V',  'i',  's',  'u',  'a',  'l',  ' ',  'M',  'e',  'm',  'o',  'r',  'y',  ' ',  ' ',  ' ',
    ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  0x00, 0x00, 'P',  'r',
    'o',  'd',  'u',  'c',  'e',  'd',  ' ',  'B',  'y',  ' ',  'o',  'r',  ' ',  'U',  'n',  'd',  'e',  'r',
    ' ',  'L',  'i',  'c',  'e',  'n',  's',  'e',  ' ',  'F',  'r',  'o',  'm',  ' ',  'S',  'E',  'G',  'A',
    ' ',  'E',  'N',  'T',  'E',  'R',  'P',  'R',  'I',  'S',  'E',  'S',  ',',  'L',  'T',  'D',  '.',  ' ',
    ' ',  ' ',  ' ',  ' ',  0x00, 0x00, 0x00, 0x00, 0x7c, 0x00, 0x82, 0x00};
constexpr u8 kInfoPack[120] = {
    0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x00, 'P',  'u',  'r',  'u',  ' ',  'P',  'u',  'r',  'u',  ' ',  'P',  'a',  'c',  'k',  ' ',  ' ',
    ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  ' ',  0x00, 0x00, 'P',  'r',
    'o',  'd',  'u',  'c',  'e',  'd',  ' ',  'B',  'y',  ' ',  'o',  'r',  ' ',  'U',  'n',  'd',  'e',  'r',
    ' ',  'L',  'i',  'c',  'e',  'n',  's',  'e',  ' ',  'F',  'r',  'o',  'm',  ' ',  'S',  'E',  'G',  'A',
    ' ',  'E',  'N',  'T',  'E',  'R',  'P',  'R',  'I',  'S',  'E',  'S',  ',',  'L',  'T',  'D',  '.',  ' ',
    ' ',  ' ',  ' ',  ' ',  0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x40, 0x06};

u32 info_rec(unsigned u) { return kInfoRec + 120 * u; }
u32 rec24(unsigned u) { return kRec24 + 24 * u; }
u32 pds(unsigned u) { return kPds + 52 * u; }

// ---- guest calls -------------------------------------------------------------------------------

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

struct Masked {
    Ctx& c;
    u32 saved;
    explicit Masked(Ctx& ctx) : c(ctx), saved(dream::sh4::read_sr(ctx)) { dream::sh4::write_sr(c, saved | 0xF0u); }
    ~Masked() { dream::sh4::write_sr(c, (dream::sh4::read_sr(c) & ~0xF0u) | (saved & 0xF0u)); }
};

// ---- what is plugged in ------------------------------------------------------------------------

enum class Kind { None, Controller, Vmu, Pack };

Kind kind_at(unsigned u) {
    if (!g_sys || !g_sys->maple)
        return Kind::None;
    dream::maple::Device* d = g_sys->maple->device_for(u / 6, kAddr[u % 6]);
    if (!d)
        return Kind::None;
    if (dynamic_cast<dream::maple::Controller*>(d))
        return Kind::Controller;
    if (dynamic_cast<dream::maple::MemoryCard*>(d))
        return Kind::Vmu;
    if (dynamic_cast<dream::maple::VibrationPack*>(d))
        return Kind::Pack;
    return Kind::None;
}

dream::maple::MemoryCard* card_at(unsigned u) {
    if (!g_sys || !g_sys->maple)
        return nullptr;
    return dynamic_cast<dream::maple::MemoryCard*>(g_sys->maple->device_for(u / 6, kAddr[u % 6]));
}

// The eight condition bytes a controller reports: buttons (active low; the standard pad has no C,
// D, Z or second D-pad, so those read released), R and L triggers, the stick, the second stick.
void condition_bytes(unsigned port, u8 out[8]) {
    dream::maple::ControllerState s;
    if (auto* ctl = dynamic_cast<dream::maple::Controller*>(g_sys->maple->device_for(port, 0x20)))
        s = ctl->state;
    const u16 buttons = static_cast<u16>(s.buttons | 0xF901u);
    out[0] = static_cast<u8>(buttons);
    out[1] = static_cast<u8>(buttons >> 8);
    out[2] = s.rtrigger;
    out[3] = s.ltrigger;
    out[4] = s.joy_x;
    out[5] = s.joy_y;
    out[6] = s.joy2_x;
    out[7] = s.joy2_y;
}

// The server's view of each unit, transfer by transfer.
enum : u8 { R_NONE = 0xFF, R_DEVSTAT = 5, R_ACK = 7, R_DATA = 8 };
enum : u8 { C_IDLE = 0, C_DEVINFO = 1, C_COND = 9, C_GIVEUP = 0xFF, C_CLIENT = 0x80 };
struct Unit {
    bool connected = false, prev = false, fresh = false, gone = false;
    u8 next = C_IDLE;       // the automatic command for the next transfer
    u8 inflight = C_IDLE;   // what this transfer carries for the unit
    u8 reply = 0;           // the reply code of the last command (0: none yet)
    unsigned retries = 0;   // socket device-info tries
    bool client = false;    // a client command is queued for the next transfer
    u8 cond[8] = {}, cond_inflight[8] = {};  // controller data: received, in flight
};
std::array<Unit, kUnits> g_units;
std::array<u8, kPorts> g_port_mask{}, g_port_prev{};
std::array<unsigned, kPorts> g_port_misses{};
bool g_inited = false, g_transfer = false;
int g_dma_event = -1;  // the transfer's end on the bus

// bu's transactions: GET_MEDIA_INFO, BLOCK_READ, one BLOCK_WRITE phase, the block's completion
// (GET_LAST_ERROR).
enum class Tx : u8 { Media, Read, Write, Complete };

// A queued client frame per unit: vibration, VMU screen, bu's transaction.
struct Client {
    enum Type { None, Vib, Lcd, Bu } type = None;
    u32 condition = 0;
    Tx tx = Tx::Read;
    std::array<u8, 192> bitmap{};
};
std::array<Client, kUnits> g_client{}, g_client_inflight{};

// An empty unit's record, as the server's 0x8C053F8C leaves it: zeros and the name "(none)",
// padded with spaces over 31 bytes (one into the licence field, as the original does).
void clear_info(Memory& m, unsigned u) {
    const u32 i = info_rec(u);
    for (u32 k = 0; k < 120; k += 4) m.write32(i + k, 0);
    const char* none = "(none)";
    for (unsigned k = 0; k < 31; ++k) m.write8(i + 20 + k, k < 6 ? static_cast<u8>(none[k]) : ' ');
}

void fill_info(Memory& m, unsigned u, Kind k) {
    const u8* t = k == Kind::Controller ? kInfoController : k == Kind::Vmu ? kInfoVmu : kInfoPack;
    const u32 i = info_rec(u);
    for (unsigned b = 0; b < 120; ++b) m.write8(i + b, t[b]);
    m.write8(i + 16, u % 6 == 0 ? 1 : 0);
}

// What the server's record holds for client code: connected, previous, new, gone, then the reply
// header's command byte. (The reply frame address and error word are not kept: no receive
// buffers exist.)
void write_rec24(Memory& m, unsigned u) {
    const Unit& s = g_units[u];
    const u32 r = rec24(u);
    m.write8(r + 0, s.connected);
    m.write8(r + 1, s.prev);
    m.write8(r + 2, s.fresh);
    m.write8(r + 3, s.gone);
    m.write8(r + 4, s.reply);
}

// ---- bu --------------------------------------------------------------------------------------------

constexpr unsigned kDrives = 8;
constexpr unsigned kBlock = 512;
constexpr u16 kFatFree = 0xFFFC, kFatEnd = 0xFFFA;
enum : s32 {
    BU_OK = 0, BU_BUSY = -1, BU_BADCHAIN = -191, BU_NOSPACE = -252, BU_NOTFOUND = -251, BU_DIRFULL = -248,
    BU_UNFORMAT = -253, BU_NOCARD = -255, BU_BADARG = -256, BU_BADHEADER = -223, BU_BADCRC = -224
};
enum : u32 { OP_MOUNT = 1, OP_REMOVED = 2, OP_FORMAT = 3, OP_LOAD = 11, OP_SAVE = 12, OP_PREAD = 14, OP_CONNECT = 18 };

struct Op {
    u32 op = 0;
    std::string name;
    u32 buf = 0, blocks = 0, skip = 0, flag = 0, volume = 0, icon = 0, surface = 0;
    u8 date[8] = {};
    bool started = false;
    std::vector<Tx> plan;  // its transactions, one a transfer
    unsigned done = 0;     // transactions answered
    bool pending = false;  // one is on the bus
};
struct Drive {
    bool mounted = false;
    Op op;
};
std::array<Drive, kDrives> g_drives{};

Op op_of(u32 code) {
    Op o;
    o.op = code;
    return o;
}
bool g_bu = false;
u32 g_bu_mask = 0, g_bu_prev = 0, g_bu_cb = 0;

unsigned drive_unit(unsigned d) { return (d / 2) * 6 + d % 2 + 1; }
bool drive_busy(unsigned d) { return g_drives[d].op.op != 0; }
bool card_present(unsigned d) {
    const unsigned u = drive_unit(d);
    return g_units[u].connected && kind_at(u) == Kind::Vmu;
}

// The VMU's file system, read and written in the image.
struct Fs {
    dream::maple::MemoryCard* card;
    std::array<u8, kBlock> sys{}, fat{};
    u16 dir_block = 253, dir_blocks = 13, user = 200;

    explicit Fs(dream::maple::MemoryCard* c) : card(c) {
        card->read_block(255, sys.data());
        card->read_block(254, fat.data());
        dir_block = rd16(sys.data() + 0x4A);
        dir_blocks = rd16(sys.data() + 0x4C);
        user = rd16(sys.data() + 0x50) ? rd16(sys.data() + 0x50) : 200;
    }
    static u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
    static void wr16(u8* p, u16 v) {
        p[0] = static_cast<u8>(v);
        p[1] = static_cast<u8>(v >> 8);
    }
    bool formatted() const {
        for (unsigned i = 0; i < 16; ++i)
            if (sys[i] != 0x55)
                return false;
        return true;
    }
    u16 link(unsigned b) const { return rd16(fat.data() + 2 * b); }
    void set_link(unsigned b, u16 v) { wr16(fat.data() + 2 * b, v); }
    // The directory blocks, following the FAT chain from the system block's directory field.
    std::vector<u16> dir_chain() const {
        std::vector<u16> out;
        for (u16 b = dir_block; out.size() < dir_blocks && b < 256; b = link(b)) out.push_back(b);
        return out;
    }
    unsigned free_blocks() const {
        unsigned n = 0;
        for (unsigned b = 0; b < user; ++b)
            if (link(b) == kFatFree)
                ++n;
        return n;
    }
    struct Entry {
        u16 block;
        unsigned index;
        std::array<u8, 32> raw;
    };
    // The first 200 entries, as bu scans them.
    std::vector<Entry> entries() const {
        std::vector<Entry> out;
        u8 blk[kBlock];
        for (u16 b : dir_chain()) {
            card->read_block(b, blk);
            for (unsigned i = 0; i < 16 && out.size() < 200; ++i) {
                Entry e{b, i, {}};
                std::memcpy(e.raw.data(), blk + 32 * i, 32);
                out.push_back(e);
            }
        }
        return out;
    }
    static bool name_is(const Entry& e, const std::string& name) {
        if (e.raw[0] != 0x33 && e.raw[0] != 0xCC)
            return false;
        unsigned len = 0;
        while (len < 12 && e.raw[4 + len]) ++len;
        return len == std::min<std::size_t>(name.size(), 12) && std::memcmp(e.raw.data() + 4, name.data(), len) == 0;
    }
    const Entry* find(const std::vector<Entry>& es, const std::string& name) const {
        for (const Entry& e : es)
            if (name_is(e, name))
                return &e;
        return nullptr;
    }
    // A file's chain: links below the user area, ending in 0xFFFA (else empty: a broken chain).
    std::vector<u16> chain(u16 first, unsigned size) const {
        std::vector<u16> out;
        for (u16 b = first; out.size() < size; ) {
            if (b >= user)
                return {};
            out.push_back(b);
            const u16 next = link(b);
            if (out.size() == size)
                return next == kFatEnd ? out : std::vector<u16>{};
            if (next == kFatFree || next == 0xFFFF || next == kFatEnd)
                return {};
            b = next;
        }
        return out;
    }
    void write_entry(const Entry& e) {
        u8 blk[kBlock];
        card->read_block(e.block, blk);
        std::memcpy(blk + 32 * e.index, e.raw.data(), 32);
        card->write_block(e.block, blk);
    }
    void flush_fat() { card->write_block(254, fat.data()); }
};

u8 bcd(unsigned v) { return static_cast<u8>(((v / 10) << 4) + v % 10); }

// The date Rez passes (u16 year, then month, day, hour, minute, second, weekday) as bu stores it:
// BCD century, year, month, day, hour, minute, second, and the weekday through bu's table.
void bus_time(Memory& m, u32 date, u8 out[8]) {
    const u16 year = m.read16(date);
    out[0] = bcd(year / 100);
    out[1] = bcd(year % 100);
    for (unsigned i = 0; i < 5; ++i) out[2 + i] = bcd(m.read8(date + 2 + i));
    out[7] = m.read8(kBuWeekday + (m.read8(date + 7) & 7));
}

// The transactions bu's fiber makes for an operation, in order, one a transfer.
std::vector<Tx> transactions(const Op& op, unsigned file_blocks) {
    std::vector<Tx> t;
    const auto reads = [&](unsigned n) { t.insert(t.end(), n, Tx::Read); };
    const auto writes = [&](unsigned blocks) {  // four phases and the completion, a block
        for (unsigned b = 0; b < blocks; ++b) {
            t.insert(t.end(), 4, Tx::Write);
            t.push_back(Tx::Complete);
        }
    };
    switch (op.op) {
        case OP_CONNECT:  // media info, block 0
            t.push_back(Tx::Media);
            reads(1);
            break;
        case OP_MOUNT: reads(1 + 1 + 13); break;  // system, FAT, directory
        case OP_LOAD: reads(op.blocks ? op.blocks : file_blocks); break;
        case OP_PREAD: reads(op.blocks ? op.blocks : (file_blocks > op.skip ? file_blocks - op.skip : 0)); break;
        case OP_SAVE:  // the data blocks, a directory block, the FAT, then the data read back
            writes(op.blocks + 2);
            reads(op.blocks);
            break;
        case OP_FORMAT:  // a zeroed system block, media info, the directory, the FAT, the system
            writes(1);   // block, then the remount
            t.push_back(Tx::Media);
            writes(13 + 1 + 1);
            reads(15);
            break;
        default: reads(1); break;
    }
    return t;
}

// Carries out a finished operation: what bu's fiber did over its transactions.
s32 finish_op(Memory& m, unsigned d, Op& op) {
    dream::maple::MemoryCard* card = card_at(drive_unit(d));
    if (!card || !card_present(d))
        return BU_NOCARD;
    Drive& dr = g_drives[d];
    switch (op.op) {
        case OP_CONNECT: return 15;  // a 128 KB card: 15 blocks of system, FAT and directory
        case OP_MOUNT: dr.mounted = true; return 0;  // 0 also for an unformatted card (status says)
        case OP_LOAD:
        case OP_PREAD: {
            Fs fs(card);
            const auto es = fs.entries();
            const Fs::Entry* e = fs.find(es, op.name);
            if (!e)
                return BU_BADCHAIN;  // bu checks the chain before the lookup
            const unsigned size = Fs::rd16(e->raw.data() + 24);
            const auto ch = fs.chain(Fs::rd16(e->raw.data() + 2), size);
            if (ch.empty())
                return BU_BADCHAIN;
            const unsigned from = op.op == OP_PREAD ? op.skip : 0;
            const unsigned n = op.blocks ? op.blocks : size - from;
            u8 blk[kBlock];
            for (unsigned i = 0; i < n && from + i < ch.size(); ++i) {
                card->read_block(ch[from + i], blk);
                for (unsigned k = 0; k < kBlock; ++k) m.write8(op.buf + i * kBlock + k, blk[k]);
            }
            return 0;
        }
        case OP_SAVE: {
            Fs fs(card);
            auto es = fs.entries();
            const Fs::Entry* old = fs.find(es, op.name);
            const unsigned old_size = old ? Fs::rd16(old->raw.data() + 24) : 0;
            if (fs.free_blocks() + old_size < op.blocks)
                return BU_NOSPACE;
            if (old) {  // deleted first: its chain freed, its entry emptied
                for (u16 b : fs.chain(Fs::rd16(old->raw.data() + 2), old_size)) fs.set_link(b, kFatFree);
                Fs::Entry gone = *old;
                gone.raw.fill(0);
                fs.write_entry(gone);
                es = fs.entries();
            }
            const Fs::Entry* slot = nullptr;
            for (const Fs::Entry& e : es)
                if (e.raw[0] == 0) {
                    slot = &e;
                    break;
                }
            if (!slot)
                return BU_DIRFULL;
            // Blocks: the highest free one each time, from the top of the user area down.
            std::vector<u16> blocks;
            for (int b = static_cast<int>(fs.user) - 1; b >= 0 && blocks.size() < op.blocks; --b)
                if (fs.link(static_cast<unsigned>(b)) == kFatFree)
                    blocks.push_back(static_cast<u16>(b));
            u8 blk[kBlock];
            for (unsigned i = 0; i < blocks.size(); ++i) {
                for (unsigned k = 0; k < kBlock; ++k) blk[k] = m.read8(op.buf + i * kBlock + k);
                card->write_block(blocks[i], blk);
                fs.set_link(blocks[i], i + 1 < blocks.size() ? blocks[i + 1] : kFatEnd);
            }
            Fs::Entry e = *slot;
            e.raw.fill(0);
            e.raw[0] = 0x33;
            e.raw[1] = static_cast<u8>(op.flag);
            Fs::wr16(e.raw.data() + 2, blocks.empty() ? 0 : blocks[0]);
            for (unsigned k = 0; k < 12; ++k) e.raw[4 + k] = k < op.name.size() ? static_cast<u8>(op.name[k]) : 0;
            std::memcpy(e.raw.data() + 16, op.date, 8);
            Fs::wr16(e.raw.data() + 24, static_cast<u16>(op.blocks));
            Fs::wr16(e.raw.data() + 26, 0);
            fs.write_entry(e);
            fs.flush_fat();
            return 0;
        }
        case OP_FORMAT: {
            std::array<u8, kBlock> sys{};
            for (unsigned i = 0; i < 16; ++i) sys[i] = 0x55;
            if (op.volume)
                for (unsigned i = 0; i < 32; ++i) sys[16 + i] = m.read8(op.volume + i);
            std::memcpy(sys.data() + 0x30, op.date, 8);
            const u16 geo[] = {255, 0, 255, 254, 1, 253, 13};
            for (unsigned i = 0; i < 7; ++i) Fs::wr16(sys.data() + 0x40 + 2 * i, geo[i]);
            sys[0x4E] = static_cast<u8>(op.icon);
            sys[0x4F] = 0;
            Fs::wr16(sys.data() + 0x50, 200);
            Fs::wr16(sys.data() + 0x52, 31);
            sys[0x56] = 128;
            std::array<u8, kBlock> fat{}, zero{};
            for (unsigned b = 0; b < 255; ++b) Fs::wr16(fat.data() + 2 * b, kFatFree);
            for (unsigned b = 241; b <= 253; ++b) Fs::wr16(fat.data() + 2 * b, b == 241 ? kFatEnd : static_cast<u16>(b - 1));
            Fs::wr16(fat.data() + 2 * 254, kFatEnd);
            Fs::wr16(fat.data() + 2 * 255, kFatEnd);
            for (unsigned b = 241; b <= 253; ++b) card->write_block(b, zero.data());
            card->write_block(254, fat.data());
            card->write_block(255, sys.data());
            dr.mounted = true;
            return 0;
        }
        default:
            return BU_BADARG;
    }
}

void bu_callback(Ctx& c, Memory& m, unsigned d, u32 op, s32 result) {
    if (g_bu_cb)
        call_guest(c, m, g_bu_cb, 0x8C0D7452u, d, op, static_cast<u32>(result), 0);
}

u32 drive_mask() {
    u32 mask = 0;
    for (unsigned d = 0; d < kDrives; ++d)
        if (card_present(d))
            mask |= 1u << d;
    return mask;
}

// bu's tick (its Maple hook, 0x8C0D7452): the operations advance a transaction, finished ones
// complete through the callback; then cards that came or went.
void bu_tick(Ctx& c, Memory& m) {
    for (unsigned d = 0; d < kDrives; ++d) {
        Op& op = g_drives[d].op;
        if (!op.op)
            continue;
        if (!op.started) {
            op.started = true;
            unsigned file_blocks = 0;
            bool formatted = true;
            if (auto* card = card_at(drive_unit(d))) {
                Fs fs(card);
                formatted = fs.formatted();
                if (op.op == OP_LOAD || op.op == OP_PREAD) {
                    const auto es = fs.entries();
                    if (const Fs::Entry* e = fs.find(es, op.name))
                        file_blocks = Fs::rd16(e->raw.data() + 24);
                }
            }
            op.plan = transactions(op, file_blocks);
            if (op.op == OP_MOUNT && !formatted)
                op.plan.resize(1);  // mount stops after the system block on an unformatted card
            if (op.plan.empty())
                op.plan.push_back(Tx::Read);
        } else if (op.pending) {  // its reply is in
            op.pending = false;
            if (++op.done == op.plan.size()) {
                Op finished = op;
                const s32 r = finish_op(m, d, finished);
                op = Op{};  // the drive is idle when the callback runs
                bu_callback(c, m, d, finished.op, r);
                continue;
            }
        }
        // The next transaction goes on the next transfer, unless the unit already has a frame
        // there (a VMU screen write): then bu yields and tries again a tick later.
        Client& cl = g_client[drive_unit(d)];
        if (cl.type != Client::None)
            continue;
        cl.type = Client::Bu;
        cl.tx = op.plan[op.done];
        op.pending = true;
    }
    const u32 now = drive_mask() & g_bu_mask;
    for (unsigned d = 0; d < kDrives; ++d) {
        const u32 bit = 1u << d;
        if ((now & bit) && !(g_bu_prev & bit) && !drive_busy(d))
            g_drives[d].op = op_of(OP_CONNECT);
        if (!(now & bit) && (g_bu_prev & bit) && !drive_busy(d)) {
            g_drives[d].mounted = false;
            bu_callback(c, m, d, OP_REMOVED, 0);
        }
    }
    g_bu_prev = now;
}

// ---- the server's transfers --------------------------------------------------------------------

// A frame's words on the bus, its header included: what a unit is sent and what it answers.
struct Frame {
    unsigned out, in;
};
Frame frame_words(const Unit& s, const Client& cl) {
    switch (s.inflight) {
        case C_DEVINFO: return {1, 1 + 28};  // the 112-byte device info
        case C_COND: return {2, 1 + 3};      // function; function and the 8 condition bytes
        case C_CLIENT:
            switch (cl.type) {
                case Client::Vib: return {3, 1};       // function, condition; acknowledged
                case Client::Lcd: return {3 + 48, 1};  // function, location, the bitmap
                case Client::Bu:
                    switch (cl.tx) {
                        case Tx::Media: return {3, 1 + 7};
                        case Tx::Read: return {3, 1 + 2 + 128};
                        case Tx::Write: return {3 + 32, 1};
                        case Tx::Complete: return {3, 1};
                    }
                    break;
                default: break;
            }
            break;
        default: break;
    }
    return {0, 0};
}

// VBlank-in (0x8C033928): the transfer starts: each unit's command for it is fixed, a controller's
// condition is sampled now, client frames go out. It ends after its time on the bus (as the
// console's Maple DMA takes it: frames out at 2 Mb/s, replies back at about 740 kb/s, nothing back
// from a unit with no device), with the Maple DMA-end interrupt.
void transfer_start() {
    if (g_transfer)
        return;  // the last transfer's replies are not in yet: the server skips this VBlank
    u64 bits_out = 0, bits_in = 0;
    std::array<unsigned, kPorts> frames{};
    for (unsigned u = 0; u < kUnits; ++u) {
        Unit& s = g_units[u];
        g_client_inflight[u] = g_client[u];
        s.client = g_client[u].type != Client::None;
        s.inflight = s.client ? static_cast<u8>(C_CLIENT) : s.next;
        if (s.inflight == C_COND)
            condition_bytes(u / 6, s.cond_inflight);
        const Frame f = frame_words(s, g_client_inflight[u]);
        if (f.out) {
            bits_out += (f.out * 4 + 3) * 8;
            if (kind_at(u) != Kind::None)
                bits_in += (f.in * 4 + 3) * 8;
            ++frames[u / 6];
        }
        g_client[u] = Client{};
    }
    // Between two frames for the same port the server's list has five NOP descriptors, a byte each
    // on the bus.
    for (const unsigned n : frames)
        if (n > 1)
            bits_out += (n - 1) * 5 * 8;
    const u64 cycles = bits_out * dream::sched::kSh4Clock / 2'000'000 + bits_in * dream::sched::kSh4Clock / 740'000;
    g_transfer = true;
    g_sys->sched.request(g_dma_event, cycles ? cycles : 1);
}

void deliver_client(unsigned u, const Client& cl) {
    if (cl.type == Client::Vib && g_sys->rumble) {
        // The pack turns the condition into a strength and a duration (the VibrationPack):
        // power over 7; a duration from frequency and inclination, at most the auto-stop time.
        const u32 cond = cl.condition;
        const unsigned pos = (cond >> 8) & 7u, neg = (cond >> 12) & 7u, freq = (cond >> 16) & 0xFFu;
        int inc = static_cast<int>((cond >> 24) & 0xFFu);
        if (cond & 0x8000u)
            inc = -inc;
        else if (!(cond & 0x0800u))
            inc = 0;
        const bool continuous = cond & 1u;
        const float strength = std::min(1.0f, static_cast<float>(pos + neg) / 7.0f);
        unsigned ms = 5000;
        if (freq > 0 && (!continuous || inc != 0)) {
            const unsigned steps = inc ? static_cast<unsigned>(std::abs(inc)) * std::max(pos, neg) : 1u;
            ms = std::min(1000u * steps / freq, 5000u);
        }
        g_sys->rumble(u / 6, strength, strength > 0.0f ? ms : 0u);
    }
}

// DMA end (0x8C033988): the replies, unit by unit, as the server parsed them.
void transfer_end(Ctx& c, Memory& m) {
    if (!g_transfer)
        return;
    g_transfer = false;
    for (unsigned port = 0; port < kPorts; ++port) {
        const unsigned u0 = port * 6;
        const bool present = kind_at(u0) != Kind::None;
        u8 mask = 0;
        if (present) {
            mask = 0x20;
            for (unsigned s = 1; s < 6; ++s)
                if (kind_at(u0 + s) != Kind::None)
                    mask |= kAddr[s];
            g_port_misses[port] = 0;
        } else if (++g_port_misses[port] <= 5) {
            mask = g_port_mask[port];  // still believed there
        }
        g_port_prev[port] = g_port_mask[port];
        g_port_mask[port] = mask;
        for (unsigned s = 0; s < 6; ++s) {
            const unsigned u = u0 + s;
            Unit& st = g_units[u];
            const Kind k = kind_at(u);
            st.prev = st.connected;
            st.fresh = st.gone = false;
            const u8 cmd = st.inflight;
            st.inflight = C_IDLE;
            if (s == 0) {
                // Slot 0: a device-info or condition reply means connected.
                if (cmd == C_IDLE || cmd == C_GIVEUP) {
                    // no command, no reply
                } else if (k == Kind::None) {
                    st.reply = R_NONE;
                    st.connected = false;
                } else {
                    st.reply = cmd == C_DEVINFO ? R_DEVSTAT : cmd == C_COND ? R_DATA : R_ACK;
                    st.connected = true;
                    if (cmd == C_COND)
                        std::memcpy(st.cond, st.cond_inflight, 8);
                }
                st.next = st.connected ? C_COND : C_DEVINFO;
            } else {
                // Sockets follow the port's mask: device info when newly present, until it answers.
                const bool there = (mask & kAddr[s]) != 0;
                if (!there) {
                    st.connected = false;
                    st.next = C_GIVEUP;
                    st.retries = 0;
                } else {
                    if (!(g_port_prev[port] & kAddr[s]) && !st.connected)
                        st.next = C_DEVINFO;
                    if (cmd == C_DEVINFO) {
                        st.reply = k != Kind::None ? R_DEVSTAT : R_NONE;
                        if (k != Kind::None) {
                            st.connected = true;
                            st.next = C_IDLE;
                        } else if (++st.retries > 10) {
                            st.next = C_GIVEUP;
                        }
                    } else if (cmd == C_CLIENT) {
                        // Block reads and media info answer with data; the rest is acknowledged.
                        const Client& cl = g_client_inflight[u];
                        st.reply = cl.type == Client::Bu && (cl.tx == Tx::Read || cl.tx == Tx::Media) ? R_DATA : R_ACK;
                    }
                }
            }
            if (st.connected && !st.prev) {
                st.fresh = true;
                fill_info(m, u, k);
            } else if (!st.connected && st.prev) {
                st.gone = true;
                clear_info(m, u);
            }
            if (cmd == C_CLIENT)
                deliver_client(u, g_client_inflight[u]);
            write_rec24(m, u);
        }
    }
    if (g_bu)
        bu_tick(c, m);
    irq_ack(kIstNrm, 0x1000);  // the Maple DMA-end event, acknowledged
}

// ---- pd ---------------------------------------------------------------------------------------

// The function-definition word of function bit `f` in a device-info record (0x8C034964): the
// record's function bits are walked from bit 31 down; each set bit has a word, in that order.
u32 fd_word(Memory& m, u32 info, u32 f) {
    const u32 func = m.read32(info);
    u32 idx = 0;
    for (u32 bit = 0x80000000u; bit; bit >>= 1) {
        if (!(func & bit))
            continue;
        if (bit == f)
            return info + 4 + 4 * idx;
        ++idx;
    }
    return 0;
}

void pd_clear(Memory& m, u32 p) {
    m.write32(p + 0, 0);
    m.write32(p + 4, 0);
    const bool neg = m.read32(kPdLogic) != 0;
    m.write32(p + 8, neg ? 0xFFFFFFFFu : 0);
    m.write32(p + 12, neg ? 0 : 0xFFFFFFFFu);
    m.write32(p + 16, neg ? 0xFFFFFFFFu : 0);
    m.write32(p + 20, neg ? 0xFFFFFFFFu : 0);
    for (u32 off : {24u, 26u, 34u, 32u, 30u, 28u}) m.write16(p + off, 0);
    m.write32(p + 40, 0);
    m.write32(p + 36, kNoDevice);
}

// pd's controller parser (0x8C0126F8): buttons and the two triggers as digital bits with pd's
// hysteresis, then the analog values the device supports.
void pd_parse(Memory& m, u32 p, const u8* b, unsigned u) {
    u32 raw = 0xFFFF0000u | static_cast<u32>(b[1]) << 8 | b[0];
    const u8 press_at = m.read8(kPdPressAt), release_at = m.read8(kPdReleaseAt);
    // Both compares sign-extend the bytes and compare unsigned: the order of unsigned bytes.
    const auto trigger = [&](u8 v, u32 bit, u32 state) {
        if (v >= press_at) {
            raw &= ~bit;
            m.write8(state + u, 1);
        } else if (v <= release_at) {
            raw |= bit;
            m.write8(state + u, 0);
        } else if (m.read8(state + u)) {
            raw &= ~bit;
        }
    };
    trigger(b[3], 0x20000u, kPdLState);
    trigger(b[2], 0x10000u, kPdRState);
    if (m.read32(kPdLogic) == 0)
        raw = ~raw;
    m.write32(p + 8, raw);
    const u32 support = m.read32(p + 4);
    m.write16(p + 24, support & 0x10000u ? b[2] : 0);
    m.write16(p + 26, support & 0x20000u ? b[3] : 0);
    m.write16(p + 28, support & 0x40000u ? static_cast<u16>(b[4] - 128) : 0);
    m.write16(p + 30, support & 0x80000u ? static_cast<u16>(b[5] - 128) : 0);
    m.write16(p + 32, support & 0x100000u ? static_cast<u16>(b[6] - 128) : 0);
    m.write16(p + 34, support & 0x200000u ? static_cast<u16>(b[7] - 128) : 0);
}

// pd's update (0x8C0129D4), once a VBlank from Rez's VBlank hook: the 24 records from the last
// transfer's replies.
void pd_update(Memory& m) {
    for (unsigned u = 0; u < kUnits; ++u) {
        const u32 p = pds(u), info = info_rec(u);
        const Unit& s = g_units[u];
        m.write32(p + 48, info);
        m.write32(p + 44, m.read32(p + 8));
        if (u % 6 != 0) {
            m.write32(p + 36, info + 20);
            continue;
        }
        if ((m.read32(info) & 0x21u) && !s.fresh) {
            u32 support = 0;
            if (m.read32(info) & 1u) {
                if (const u32 w = fd_word(m, info, 1))
                    support = static_cast<u32>(m.read8(w)) << 24 | static_cast<u32>(m.read8(w + 1)) << 16 |
                              static_cast<u32>(m.read8(w + 2)) << 8 | m.read8(w + 3);
            }
            m.write32(p + 4, support);
            m.write32(p + 0, support);
            m.write32(p + 36, info + 20);
            if (s.reply == R_DATA)
                pd_parse(m, p, s.cond, u);
        } else {
            pd_clear(m, p);
        }
        const u32 on = m.read32(p + 8), old = m.read32(p + 44);
        m.write32(p + 12, ~on);
        const u32 x = on ^ old;
        if (m.read32(kPdLogic) == 0) {
            m.write32(p + 16, x & on);
            m.write32(p + 20, x & ~on);
        } else {
            m.write32(p + 16, ~x | on);
            m.write32(p + 20, ~x | ~on);
        }
    }
}

// ---- guards ----------------------------------------------------------------------------------------

// Functions the native layer replaces whole: one that runs anyway is said once.
struct Guard {
    const char* what;
    bool said = false;
};
Guard g_guard_maple{"Maple server function"}, g_guard_pd{"pd function"}, g_guard_bu{"bu function"};

bool guard(Guard& g, const Ctx& c) {
    if (!g_on)
        return false;
    if (!g.said) {
        g.said = true;
        std::fprintf(stderr, "native peripherals: a %s ran (called from %08x)\n", g.what, c.pr);
    }
    return false;
}

}  // namespace

void native_periph_start(dream::System& sys) {
    g_sys = &sys;
    g_on = sys.maple != nullptr;  // its memory card holds the VMU image the saves are written to
    if (g_on) {
        g_dma_event = sys.sched.add("native periph maple", [](u64, u64) {
            g_sys->holly.raise(dream::holly::Irq::MapleDma);
        });
    }
}

bool periph_on() {
    return g_on && g_sys;
}

}  // namespace rez

// ---- the hooks ----------------------------------------------------------------------------------------

#define ON() if (!rez::periph_on()) return false

using rez::u32;
using rez::s32;

// Katana's Maple server.
DC_HOOK_ENTRY(maple_init) {  // 0x8C033D24 (recv, send), from pdInitPeripheral
    ON();
    using namespace rez;
    m.write32(kWorkPtr, kWork);
    for (u32 a = kStats; a < kStats + 0x38; a += 4) m.write32(a, 0);
    for (unsigned u = 0; u < kUnits; ++u) {
        for (u32 k = 0; k < 24; k += 4) m.write32(rec24(u) + k, 0);
        clear_info(m, u);
        g_units[u] = Unit{};
        g_client[u] = g_client_inflight[u] = Client{};
    }
    for (u32 a = kOneShot; a < kOneShot + 48 * 12; a += 4) m.write32(a, 0);
    for (u32 a = kCallbacks; a < kCallbacks + 32 * 4; a += 4) m.write32(a, 0);
    // The server's first transfer asks every port's slot 0 for its device info.
    for (unsigned port = 0; port < kPorts; ++port) g_units[port * 6].next = C_DEVINFO;
    g_port_mask.fill(0);
    g_port_prev.fill(0);
    g_port_misses.fill(0);
    // Its three interrupt handlers, as the server registers them; the Maple DMA-end one is the
    // point where a transfer's replies are taken.
    m.write32(kChainVblank, call_guest(c, m, kChainAdd, 0x8C033D24u, kEvVblank, kVblankHandler, 0x38, 0));
    m.write32(kChainDmaEnd, call_guest(c, m, kChainAdd, 0x8C033D24u, kEvDmaEnd, kDmaEndHandler, 0x10, 0));
    m.write32(kChainVbOver, call_guest(c, m, kChainAdd, 0x8C033D24u, kEvVbOver, kVbOverHandler, 0x10, 0));
    irq_mask(kIml4Nrm, irq_mask_value(kIml4Nrm) | 0x3000u);
    m.write32(kInited, 1);
    g_inited = true;
    c.r[0] = 0;
    return true;
}
DC_HOOK_ENTRY(maple_exit) {  // 0x8C0343C6
    ON();
    rez::irq_mask(rez::kIml4Nrm, rez::irq_mask_value(rez::kIml4Nrm) & ~0x3000u);
    rez::g_inited = false;
    m.write32(rez::kInited, 0);
    return true;
}
DC_HOOK_ENTRY(maple_vblank) {  // 0x8C033928: the transfer starts
    ON();
    m.write32(rez::kStats + 8, m.read32(rez::kStats + 8) + 1);
    if (rez::g_inited)
        rez::transfer_start();
    return true;
}
DC_HOOK_ENTRY(maple_dma_end) {  // 0x8C033988: the transfer's replies
    ON();
    rez::Masked masked(c);
    rez::transfer_end(c, m);
    return true;
}
DC_HOOK_ENTRY(maple_vb_over) {  // 0x8C033B20: a transfer still running at VBlank: never here
    ON();
    rez::irq_ack(rez::kIstNrm, 0x2000);
    return true;
}
DC_HOOK_ENTRY(maple_record) {  // 0x8C0344C8 (unit)
    ON();
    c.r[0] = rez::rec24(c.r[4]);
    return true;
}
DC_HOOK_ENTRY(maple_info) {  // 0x8C0344E8 (unit)
    ON();
    c.r[0] = rez::info_rec(c.r[4]);
    return true;
}
DC_HOOK_ENTRY(maple_fd) {  // 0x8C034964 (info, function bit) -> the function's word, 0 if none
    ON();
    c.r[0] = rez::fd_word(m, c.r[4], c.r[5]);
    return true;
}
DC_HOOK_ENTRY(maple_fd_controller) {  // 0x8C0349DC (out, info): the controller word, big-endian
    ON();
    const u32 w = rez::fd_word(m, c.r[5], 1);
    if (!w) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    m.write32(c.r[4], static_cast<u32>(m.read8(w)) << 24 | static_cast<u32>(m.read8(w + 1)) << 16 |
                          static_cast<u32>(m.read8(w + 2)) << 8 | m.read8(w + 3));
    c.r[0] = 0;
    return true;
}
DC_HOOK_ENTRY(maple_fd_lcd) {  // 0x8C034B08 (out, info): width, height, gradation, two flags
    ON();
    const u32 w = rez::fd_word(m, c.r[5], 4), o = c.r[4];
    if (!w) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    m.write16(o + 0, static_cast<std::uint16_t>(m.read8(w) + 1));
    m.write16(o + 2, static_cast<std::uint16_t>((m.read8(w + 1) + 1) << 5));
    m.write16(o + 4, static_cast<std::uint16_t>((m.read8(w + 2) >> 4) & 15));
    m.write8(o + 6, static_cast<std::uint8_t>((m.read8(w + 3) >> 6) & 3));
    m.write8(o + 7, static_cast<std::uint8_t>((m.read8(w + 3) >> 5) & 1));
    c.r[0] = 0;
    return true;
}
DC_HOOK_ENTRY(maple_callback) {  // 0x8C034ED4 (slot, fn): a periodic callback
    ON();
    if (c.r[4] <= 31)
        m.write32(rez::kCallbacks + 4 * c.r[4], c.r[5]);
    return true;
}

// pd.
DC_HOOK_ENTRY(pd_server) {  // 0x8C0129D4, from Rez's VBlank hook
    ON();
    rez::pd_update(m);
    return true;
}

// pdVib.
DC_HOOK_ENTRY(pd_vib_start) {  // 0x8C014F9E (unit, const PDS_VIBPARAM*, count)
    using namespace rez;
    const u32 unit = c.r[4], p = c.r[5];
    // The condition word as pdVibMxStart builds it: unit and CNT, power, frequency, inclination.
    const u8 flag = m.read8(p + 1), freq = m.read8(p + 3), inc = m.read8(p + 4);
    const auto power = static_cast<std::int8_t>(m.read8(p + 2));
    int pw = power == 1 ? 2 : power == -1 ? -2 : power;
    u8 code = 0;
    if (pw > 0)
        code = static_cast<u8>((pw << 4) & 0x70);
    else if (pw < 0)
        code = static_cast<u8>((-pw) & 7);
    const u32 cond = static_cast<u32>(m.read8(p) << 4 | (flag & 1)) | static_cast<u32>((flag & 0x88) | code) << 8 |
                     static_cast<u32>(freq < 15 ? 15 : freq) << 16 | static_cast<u32>(flag & 0x88 ? inc : 0) << 24;
    ON();
    if (unit >= kUnits || !(m.read32(info_rec(unit)) & 0x100u)) {
        c.r[0] = static_cast<u32>(-2);
        return true;
    }
    if (g_client[unit].type != Client::None) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    g_client[unit].type = Client::Vib;
    g_client[unit].condition = cond;
    c.r[0] = 0;
    return true;
}

// pdLcd.
DC_HOOK_ENTRY(pd_lcd_direction) {  // 0x8C0146EC (unit)
    ON();
    const u32 u = c.r[4];
    const u32 port_info = rez::info_rec(m.read8(rez::kLcdPortUnit + u)), info = rez::info_rec(u);
    const u32 second = (u == 2 || u == 8 || u == 14 || u == 20) ? 1 : 0;
    c.r[0] = m.read8(rez::kLcdDirTable + m.read8(port_info + 18 + second) * 4 + m.read8(info + 18));
    return true;
}
namespace {
bool lcd_standard(dream::Memory& m, u32 unit) {
    const u32 info = rez::info_rec(unit), w = rez::fd_word(m, info, 4);
    if (!w || m.read32(info) != 14)
        return false;
    return m.read8(w) + 1 == 1 && ((m.read8(w + 1) + 1) << 5) == 192 && ((m.read8(w + 2) >> 4) & 15) == 1 &&
           ((m.read8(w + 3) >> 6) & 3) == 0 && ((m.read8(w + 3) >> 5) & 1) == 0;
}
}  // namespace
DC_HOOK_ENTRY(pd_lcd_check) {  // 0x8C014808 (unit): a standard VMU screen
    ON();
    c.r[0] = lcd_standard(m, c.r[4]) ? 1 : 0;
    return true;
}
DC_HOOK_ENTRY(pd_lcd_write) {  // 0x8C0147BE (unit, 48-word bitmap)
    using namespace rez;
    const u32 unit = c.r[4], bmp = c.r[5];
    ON();
    if (unit >= kUnits || !lcd_standard(m, unit)) {
        c.r[0] = static_cast<u32>(-2);
        return true;
    }
    if (g_client[unit].type != Client::None) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    g_client[unit].type = Client::Lcd;
    for (unsigned i = 0; i < 192; ++i) g_client[unit].bitmap[i] = m.read8(bmp + i);
    c.r[0] = 0;
    return true;
}

// bu.
namespace {
std::string bu_name(dream::Memory& m, u32 p) {
    std::string s;
    for (unsigned i = 0; p && i < 12; ++i) {
        const char ch = static_cast<char>(m.read8(p + i));
        if (!ch)
            break;
        s += ch;
    }
    return s;
}
bool bu_queue(dream::sh4::Ctx& c, unsigned d, rez::Op op) {
    if (d >= rez::kDrives || rez::drive_busy(d)) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    rez::g_drives[d].op = op;
    c.r[0] = 0;
    return true;
}
}  // namespace
DC_HOOK_ENTRY(bu_init) {  // 0x8C0D72A6 (capacity, drive mask, static work, init callback)
    ON();
    using namespace rez;
    Masked masked(c);
    g_bu_mask = c.r[5];
    g_bu_prev = 0;
    for (auto& d : g_drives) d = Drive{};
    const u32 init_cb = c.r[7];
    g_bu = true;
    m.write32(kBuRunning, 1);
    if (init_cb)
        call_guest(c, m, init_cb, 0x8C0D72A6u);
    c.r[0] = 0;
    return true;
}
DC_HOOK_ENTRY(bu_exit) {  // 0x8C0D73F4
    ON();
    for (unsigned d = 0; d < rez::kDrives; ++d)
        if (rez::drive_busy(d)) {
            c.r[0] = static_cast<u32>(-1);
            return true;
        }
    rez::g_bu = false;
    m.write32(rez::kBuRunning, 0);
    c.r[0] = 0;
    return true;
}
DC_HOOK_ENTRY(bu_complete_cb) {  // 0x8C0D7640 (fn)
    ON();
    rez::g_bu_cb = c.r[4];
    return true;
}
DC_HOOK_ENTRY(bu_progress_cb) {  // 0x8C0D7656 (fn): Rez's does nothing
    ON();
    return true;
}
DC_HOOK_ENTRY(bu_mount) {  // 0x8C0D83D8 (drive, work, size)
    ON();
    return bu_queue(c, c.r[4], rez::op_of(rez::OP_MOUNT));
}
DC_HOOK_ENTRY(bu_unmount) {  // 0x8C0D7798 (drive)
    ON();
    const u32 d = c.r[4];
    if (d >= rez::kDrives || rez::drive_busy(d)) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    rez::g_drives[d].mounted = false;
    c.r[0] = rez::card_present(d) ? 0 : static_cast<u32>(rez::BU_NOCARD);
    return true;
}
DC_HOOK_ENTRY(bu_status) {  // 0x8C0D77D4 (drive): 0 formatted, -253 unformatted, -255 no card
    ON();
    const u32 d = c.r[4];
    if (d >= rez::kDrives || !rez::card_present(d) || !rez::g_drives[d].mounted) {
        c.r[0] = static_cast<u32>(rez::BU_NOCARD);
        return true;
    }
    rez::Fs fs(rez::card_at(rez::drive_unit(d)));
    c.r[0] = fs.formatted() ? 0 : static_cast<u32>(rez::BU_UNFORMAT);
    return true;
}
DC_HOOK_ENTRY(bu_busy) {  // 0x8C0D7E7C (drive): -1 busy, 0 idle
    ON();
    c.r[0] = c.r[4] < rez::kDrives && rez::drive_busy(c.r[4]) ? static_cast<u32>(-1) : 0;
    return true;
}
namespace {
// The checks bu's synchronous calls make first: card, mounted, formatted.
s32 bu_ready(u32 d) {
    if (d >= rez::kDrives || !rez::card_present(d))
        return rez::BU_NOCARD;
    if (!rez::g_drives[d].mounted)
        return rez::BU_NOCARD;
    if (!rez::Fs(rez::card_at(rez::drive_unit(d))).formatted())
        return rez::BU_UNFORMAT;
    return 0;
}
}  // namespace
DC_HOOK_ENTRY(bu_free) {  // 0x8C0D78BC (drive, 0): free blocks
    ON();
    const s32 r = bu_ready(c.r[4]);
    c.r[0] = r ? static_cast<u32>(r) : rez::Fs(rez::card_at(rez::drive_unit(c.r[4]))).free_blocks();
    return true;
}
DC_HOOK_ENTRY(bu_exists) {  // 0x8C0D7DF8 (drive, name): 0 found, -251 not
    ON();
    if (rez::drive_busy(c.r[4])) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    if (const s32 r = bu_ready(c.r[4])) {
        c.r[0] = static_cast<u32>(r);
        return true;
    }
    rez::Fs fs(rez::card_at(rez::drive_unit(c.r[4])));
    const auto es = fs.entries();
    c.r[0] = fs.find(es, bu_name(m, c.r[5])) ? 0 : static_cast<u32>(rez::BU_NOTFOUND);
    return true;
}
DC_HOOK_ENTRY(bu_size) {  // 0x8C0D7F4C (drive, name): blocks
    ON();
    if (rez::drive_busy(c.r[4])) {
        c.r[0] = static_cast<u32>(-1);
        return true;
    }
    if (const s32 r = bu_ready(c.r[4])) {
        c.r[0] = static_cast<u32>(r);
        return true;
    }
    rez::Fs fs(rez::card_at(rez::drive_unit(c.r[4])));
    const auto es = fs.entries();
    const auto* e = fs.find(es, bu_name(m, c.r[5]));
    c.r[0] = e ? rez::Fs::rd16(e->raw.data() + 24) : static_cast<u32>(rez::BU_NOTFOUND);
    return true;
}
DC_HOOK_ENTRY(bu_load) {  // 0x8C0D7FF8 (drive, name, buf, blocks)
    ON();
    rez::Op op = rez::op_of(rez::OP_LOAD);
    op.name = bu_name(m, c.r[5]);
    op.buf = c.r[6];
    op.blocks = c.r[7];
    return bu_queue(c, c.r[4], op);
}
DC_HOOK_ENTRY(bu_save) {  // 0x8C0D8070 (drive, name, image, blocks, [sp] date, [sp+4] flag)
    ON();
    rez::Op op = rez::op_of(rez::OP_SAVE);
    op.name = bu_name(m, c.r[5]);
    op.buf = c.r[6];
    op.blocks = c.r[7];
    const u32 date = m.read32(c.r[15]);
    op.flag = m.read32(c.r[15] + 4);
    if (date)
        rez::bus_time(m, date, op.date);
    return bu_queue(c, c.r[4], op);
}
DC_HOOK_ENTRY(bu_partial_read) {  // 0x8C0D8180 (drive, name, buf, skip, [sp] count)
    ON();
    rez::Op op = rez::op_of(rez::OP_PREAD);
    op.name = bu_name(m, c.r[5]);
    op.buf = c.r[6];
    op.skip = c.r[7];
    op.blocks = m.read32(c.r[15]);
    return bu_queue(c, c.r[4], op);
}
DC_HOOK_ENTRY(bu_format) {  // 0x8C0D77F8 (drive, volume, icon, date, [sp] surface test)
    ON();
    rez::Op op = rez::op_of(rez::OP_FORMAT);
    op.volume = c.r[5];
    op.icon = c.r[6];
    if (c.r[7])
        rez::bus_time(m, c.r[7], op.date);
    op.surface = m.read32(c.r[15]);
    return bu_queue(c, c.r[4], op);
}

// bu's file-image helpers: pure, no card.
namespace {
u32 bu_crc(dream::Memory& m, u32 p, u32 n) {  // CRC-16/XMODEM
    u32 crc = 0;
    for (u32 i = 0; i < n; ++i) {
        crc ^= static_cast<u32>(m.read8(p + i)) << 8;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) & 0xFFFFu : (crc << 1) & 0xFFFFu;
    }
    return crc;
}
}  // namespace
DC_HOOK_ENTRY(bu_crc) {  // 0x8C0D8C7C (buf, n)
    ON();
    c.r[0] = bu_crc(m, c.r[4], c.r[5]);
    return true;
}
DC_HOOK_ENTRY(bu_calc_size) {  // 0x8C0D898C (icons, eyecatch type, data size) -> blocks
    ON();
    const u32 icons = c.r[4], eye = c.r[5];
    if (icons == 0 || icons > 3 || eye > 3) {
        c.r[0] = static_cast<u32>(rez::BU_BADARG);
        return true;
    }
    const u32 total = 0x80 + m.read32(rez::kBuIconBytes + 4 * icons) + m.read32(rez::kBuEyeBytes + 4 * eye) + c.r[6];
    c.r[0] = (total + 511) / 512;
    return true;
}
DC_HOOK_ENTRY(bu_make_image) {  // 0x8C0D8628 (buf, descriptor) -> blocks
    ON();
    using namespace rez;
    const u32 buf = c.r[4], d = c.r[5];
    const u32 icons = m.read16(d + 76), eye = m.read16(d + 84);
    const u32 palette = m.read32(d + 68), icon_data = m.read32(d + 72), eye_data = m.read32(d + 80);
    if (icons == 0 || icons > 3 || eye > 3 || !palette || !icon_data || (eye && !eye_data)) {
        c.r[0] = static_cast<u32>(BU_BADARG);
        return true;
    }
    for (u32 i = 0; i < 0x60; ++i) m.write8(buf + i, i < 0x30 ? ' ' : 0);
    const auto copy = [&](u32 to, u32 from, unsigned max) {
        for (unsigned i = 0; i < max; ++i) {
            const u8 ch = m.read8(from + i);
            if (!ch)
                break;
            m.write8(to + i, ch);
        }
    };
    copy(buf + 0x00, d + 0, 16);
    copy(buf + 0x10, d + 18, 32);
    copy(buf + 0x30, d + 52, 16);
    m.write16(buf + 0x40, static_cast<std::uint16_t>(icons));
    m.write16(buf + 0x42, m.read16(d + 78));
    m.write16(buf + 0x44, static_cast<std::uint16_t>(eye));
    const u32 size = m.read32(d + 92), data = m.read32(d + 88);
    m.write32(buf + 0x48, size);
    for (u32 i = 0; i < 32; ++i) m.write8(buf + 0x60 + i, m.read8(palette + i));
    const u32 icon_bytes = m.read32(kBuIconBytes + 4 * icons), eye_bytes = m.read32(kBuEyeBytes + 4 * eye);
    u32 at = buf + 0x80;
    for (u32 i = 0; i < icon_bytes; ++i) m.write8(at++, m.read8(icon_data + i));
    for (u32 i = 0; i < eye_bytes; ++i) m.write8(at++, m.read8(eye_data + i));
    for (u32 i = 0; i < size; ++i) m.write8(at++, m.read8(data + i));
    const u32 total = at - buf, blocks = (total + 511) / 512;
    for (; at < buf + blocks * 512; ++at) m.write8(at, 0);
    m.write16(buf + 0x46, 0);
    m.write16(buf + 0x46, static_cast<std::uint16_t>(bu_crc(m, buf, total)));
    c.r[0] = blocks;
    return true;
}
DC_HOOK_ENTRY(bu_analyze_image) {  // 0x8C0D8A0E (descriptor, buf) -> 0, -223, -224
    ON();
    using namespace rez;
    const u32 d = c.r[4], buf = c.r[5];
    const u32 icons = m.read16(buf + 0x40), eye = m.read16(buf + 0x44), size = m.read32(buf + 0x48);
    if (icons == 0 || icons > 3 || eye > 3 || size > 0xE1000) {
        c.r[0] = static_cast<u32>(BU_BADHEADER);
        return true;
    }
    const u32 icon_bytes = m.read32(kBuIconBytes + 4 * icons), eye_bytes = m.read32(kBuEyeBytes + 4 * eye);
    const u32 total = 0x80 + icon_bytes + eye_bytes + size;
    if (total > 0xE1000) {
        c.r[0] = static_cast<u32>(BU_BADHEADER);
        return true;
    }
    const auto copy = [&](u32 to, u32 from, unsigned n) {
        for (unsigned i = 0; i < n; ++i) m.write8(to + i, m.read8(from + i));
        m.write8(to + n, 0);
    };
    copy(d + 0, buf + 0x00, 16);
    copy(d + 18, buf + 0x10, 32);
    copy(d + 52, buf + 0x30, 16);
    m.write32(d + 68, buf + 0x60);
    m.write32(d + 72, buf + 0x80);
    m.write16(d + 76, static_cast<std::uint16_t>(icons));
    m.write16(d + 78, m.read16(buf + 0x42));
    m.write32(d + 80, eye ? buf + 0x80 + icon_bytes : 0);
    m.write16(d + 84, static_cast<std::uint16_t>(eye));
    m.write32(d + 88, buf + 0x80 + icon_bytes + eye_bytes);
    m.write32(d + 92, size);
    const u32 stored = m.read16(buf + 0x46);
    if (stored) {
        m.write16(buf + 0x46, 0);
        const u32 crc = bu_crc(m, buf, total);
        m.write16(buf + 0x46, static_cast<std::uint16_t>(stored));
        if (crc != stored) {
            c.r[0] = static_cast<u32>(BU_BADCRC);
            return true;
        }
    }
    c.r[0] = 0;
    return true;
}

DC_HOOK_ENTRY(periph_guard_maple) { return rez::guard(rez::g_guard_maple, c); }
DC_HOOK_ENTRY(periph_guard_pd) { return rez::guard(rez::g_guard_pd, c); }
DC_HOOK_ENTRY(periph_guard_bu) { return rez::guard(rez::g_guard_bu, c); }
