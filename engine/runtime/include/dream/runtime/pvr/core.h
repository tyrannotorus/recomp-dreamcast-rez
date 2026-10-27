// PowerVR2 core, headless part: the register block at 0x005F8000, the Tile Accelerator's
// parameter parser on the FIFO at 0x10000000 (state machine and interrupts as in Flycast's ta.cpp,
// GPL-2.0), render start/done timing, and the texture and YUV paths. The captured parameter
// stream per render is what the Vulkan renderer will consume; nothing is drawn here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/spg.h"
#include "dream/runtime/sched/scheduler.h"

namespace dream::pvr {

enum ListType : unsigned {
    kOpaque = 0,
    kOpaqueModVol = 1,
    kTranslucent = 2,
    kTranslucentModVol = 3,
    kPunchThrough = 4
};
enum ParamType : unsigned {
    kEndOfList = 0,
    kUserTileClip = 1,
    kObjectListSet = 2,
    kPolygonOrModVol = 4,
    kSprite = 5,
    kVertex = 7
};

// A render's parameter words, shared so the launcher's render thread keeps them without a copy.
// Nothing changes words once a render holds them (Stream). In order, pieces of words the stream
// copied or borrowed: read in place, from memory the producer keeps unchanged until the render
// after this one is handed over (Stream::borrow).
struct RenderStream {
    struct Piece {
        const std::uint32_t* borrowed;  // null: words[offset, offset + count)
        std::uint32_t offset, count;
    };
    std::vector<std::uint32_t> words;
    std::vector<Piece> pieces;
    std::size_t total = 0;
    bool borrows = false;

    std::size_t size() const noexcept { return total; }
    bool empty() const noexcept { return total == 0; }
    // Each piece in order: f(words, count).
    template <class F>
    void for_each(F&& f) const {
        for (const Piece& p : pieces) f(p.borrowed ? p.borrowed : words.data() + p.offset, std::size_t{p.count});
    }
    void clear() noexcept;
};
using RenderWords = std::shared_ptr<const RenderStream>;

// The parameter words since a list init, as a producer collects them (the TA model below, or
// native code that builds the game's lists). The buffers come from a small pool and are reused
// once no render holds them, so a stream's capacity carries over from frame to frame. Words a
// render holds are copied before anything is added to them.
class Stream {
public:
    void clear();  // a list init
    void append(const std::uint32_t* words, std::size_t n);
    // Words not copied: the render reads them in place. The caller keeps them unchanged until it
    // next begins a scene's lists after the next render start, and reports each such beginning
    // (Core::on_lists_begin), where the launcher makes sure they have been read. Suits
    // double-buffered lists, whose buffer comes round again two renders on.
    // DREAM_BORROW_CHECK=1 keeps a copy and counts any word that changed before decoding.
    void borrow(const std::uint32_t* words, std::size_t n);
    RenderWords share();  // for a render: the words as they are now
    std::size_t size() const noexcept { return words_ ? words_->size() : 0; }
    bool empty() const noexcept { return size() == 0; }

private:
    using Words = std::shared_ptr<RenderStream>;
    bool held(const Words& w) const noexcept;
    Words unheld();
    void writable();  // words_ one no render holds, with what it held
    Words words_;
    std::vector<Words> pool_;  // every buffer this stream made, words_ among them
};

// Parameter parser: 32-byte chunks in, end-of-list events out.
class Ta {
public:
    void reset() noexcept;
    void feed(const std::uint32_t words[8]);
    std::function<void(unsigned list)> on_list_end;

    unsigned current_list = 7;  // 7: none open
    Stream stream;              // raw parameter words since the last list init

    // Sizes from the parameter control word (Flycast TaTypeLut): a 64-byte polygon header is an
    // intensity-coloured textured polygon with offset colour, or an intensity two-volume polygon;
    // 64-byte vertices are textured floating-colour or textured two-volume ones.
    static bool header_is_64(std::uint32_t pcw) noexcept;
    static bool vertex_is_64(std::uint32_t pcw) noexcept;

private:
    enum State { kNs, kPlv32, kPlv64, kMlv64, kPlhv32, kPlhv64, kPlv64H, kMlv64H };
    State state_ = kNs;
    bool pending_v64_ = false;
};

class Core final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kRegBase = 0x005F8000u, kRegEnd = 0x005FA000u;
    static constexpr std::uint32_t kFifoBase = 0x10000000u, kYuvBase = 0x10800000u,
                                   kTexBase = 0x11000000u, kFifoEnd = 0x12000000u;
    // Registers (offsets from kRegBase)
    static constexpr std::uint32_t kId = 0x00, kRevision = 0x04, kSoftReset = 0x08,
                                   kStartRender = 0x14, kParamBase = 0x20, kRegionBase = 0x2C,
                                   kFbRCtrl = 0x44, kFbWCtrl = 0x48, kFbRSof1 = 0x50,
                                   kFbRSof2 = 0x54, kFbWSof1 = 0x60, kFbWSof2 = 0x64,
                                   kIspBackgndT = 0x8C, kTaOlBase = 0x124, kTaIspBase = 0x128,
                                   kTaOlLimit = 0x12C, kTaIspLimit = 0x130, kTaNextOpb = 0x134,
                                   kTaItpCurrent = 0x138, kTaGlobTileClip = 0x13C,
                                   kTaAllocCtrl = 0x140, kTaListInit = 0x144, kTaYuvTexBase = 0x148,
                                   kTaYuvTexCtrl = 0x14C, kTaYuvTexCnt = 0x150, kTaListCont = 0x160,
                                   kTaNextOpbInit = 0x164;

    Core(sched::Scheduler& sched, holly::Intc& holly, Spg& spg, mem::DcMemory& memory);
    void reset();

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;
    void write_burst(std::uint32_t addr, const std::uint32_t* words) override;

    std::uint32_t reg(std::uint32_t offset) const noexcept { return regs_[offset >> 2]; }
    // The whole register block, for callers that need several registers as a unit: describing the
    // framebuffer takes four of them, and palette memory lives at offset 0x1000 inside it.
    const std::uint32_t* reg_block() const noexcept { return regs_.data(); }
    // The frame settings the renderer reads: native code that takes over the game's graphics
    // library points `settings` at its own block, in the register layout; unset, the renderer
    // reads this model's registers.
    const std::uint32_t* settings = nullptr;
    const std::uint32_t* render_regs() const noexcept { return settings ? settings : regs_.data(); }
    std::uint32_t render_reg(std::uint32_t offset) const noexcept { return render_regs()[offset >> 2]; }

    Ta ta;
    // Called when the guest starts a render, with the parameter stream that render will draw.
    // The renderer hangs off this.
    std::function<void(const RenderWords& words)> on_render;
    // Called by a producer that borrows (Stream::borrow) before it starts writing a scene's
    // lists: from there the words a render two back borrowed may be written again, so the
    // launcher makes sure they are read first.
    std::function<void()> on_lists_begin;
    std::uint64_t frame_swaps = 0;  // FB_R_SOF1 writes: pictures the guest has shown
    // Render time model: fixed cost plus per parameter word.
    std::uint64_t render_base_cycles = 200'000, render_cycles_per_word = 4;

private:
    void start_render();
    void fifo_word(std::uint32_t addr, std::uint32_t value);
    // YUV converter: the guest streams 4:2:0 macroblocks into the converter FIFO and the
    // hardware writes 4:2:2 texels into texture memory at TA_YUV_TEX_BASE. Sofdec video arrives
    // this way, so a title playing an FMV draws nothing at all without it.
    void yuv_word(std::uint32_t value);
    void yuv_macroblock();

    std::array<std::uint8_t, 384> yuv_mb_{};  // 64 U + 64 V + 256 Y, in arrival order
    std::uint32_t yuv_fill_ = 0;              // bytes of the current macroblock collected
    std::uint32_t yuv_index_ = 0;             // macroblock position within the texture

    sched::Scheduler& sched_;
    holly::Intc& holly_;
    Spg& spg_;
    mem::DcMemory& memory_;
    std::array<std::uint32_t, 0x2000 / 4> regs_{};
    std::uint32_t fifo_buf_[8]{};
    unsigned fifo_fill_ = 0;
    int render_event_ = -1;
};

}  // namespace dream::pvr
