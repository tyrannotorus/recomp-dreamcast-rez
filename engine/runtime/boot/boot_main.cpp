// The launcher: boots a translated title on the runtime, in a window or headless. Each game
// target compiles this file together with its emitted units; the per-game TOML says what to load.
//
// Exit code 0 when the run ended normally, 1 on a fault, 2 on a setup error, 3 for a disc this
// build cannot run.
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dream/runtime/aica/aica.h"
#include "dream/runtime/aica/rtc.h"
#include "dream/runtime/gdrom/disc.h"
#include "dream/runtime/gdrom/mod_disc.h"
#include "dream/runtime/sha1.h"
#include "dream/runtime/host_ext.h"
#include "dream/runtime/hle/bios.h"
#include "dream/runtime/holly/g2dma.h"
#include "dream/runtime/holly/sysblock.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/pvr/core.h"
#include "dream/runtime/sh4/abi.h"
#include "dream/runtime/sh4/dmac.h"
#include "dream/runtime/sh4/ops.h"
#include "dream/runtime/system.h"
#include "dream/translator/config/game_config.h"
#ifdef DREAM_WITH_AUDIO
#include "dream/audio/sink.h"
#endif
#ifdef DREAM_WITH_RENDERER

#include "dream/render/background.h"
#include "dream/render/display_list.h"
#include "dream/render/framebuffer.h"
#include "dream/render/interpolate.h"
#include "dream/render/input_menu.h"
#include "dream/render/overlay.h"
#include "dream/render/vk/offscreen.h"
#include "dream/render/vk/present.h"
#include "dream/render/vk/renderer.h"
#include "dream/render/vk/window.h"
#endif

namespace {

// --cache-dir: where the pipeline cache lives when not beside the bindings.
std::string g_cache_dir;
// --pipeline-keys: a port's list of the pipeline variants its game uses, made at start-up.
std::string g_pipeline_keys;

struct StopRun {
    const char* why;
};

#ifdef DREAM_WITH_RENDERER
// The windowed mode. Nothing here is constructed without --window.
//
// The guest still nominates what is shown: at vertical blank the address in FB_R_SOF1 decides.
// What differs is where those pixels are read from.
//
//   the guest displays a buffer we rendered    the rendered image is shown directly, at whatever
//                                              resolution --scale asked for
//   the guest displays anything else           that buffer is decoded out of video memory, which
//                                              is how a title's own pixel writes appear
//
// The rendered frame is never written back into video memory: it would land on texture data a
// title keeps in the same memory. A render into texture memory is kept on the GPU instead.

// Display sync (--sync-display): when the display refreshes within 0.5 % of the guest's
// frame rate, the pacing clock runs at the display's rate instead of the guest's. A 59.94 Hz game
// on a 60 Hz panel otherwise falls one frame behind every 16.7 s and the panel shows a frame twice.
// The rate is aimed 0.01 % under the display's, so the swapchain queue stays drained (the lowest
// latency) and the panel repeats one frame about every 2.8 minutes instead. The game runs 0.1 %
// fast (60 for 59.94); the audio sink's rate control absorbs it.
struct DisplaySync {
    bool enabled = false;
    double guest_frame_s = 0;  // average guest frame length, from cycles per vblank
    std::uint64_t last_cycles = 0;
    double display_hz = 0, factor = 1.0;
    // Recent guest frame lengths in host time (pads read -> next vblank).
    std::vector<float> emu_ms;
    // Phase lock, where frames are flipped (Window::flips(), the DXGI path): each
    // frame starts so that even a slow one is presented `margin` before its vblank. The start is
    // `budget` (the slowest frame of the last second plus `pad`) before that point, so the pads
    // are still read just before the guest runs, and a typical frame lands a few ms before the
    // vblank instead of straddling it.
    std::chrono::nanoseconds margin{3'000'000};
    std::chrono::nanoseconds pad{1'000'000};
    std::chrono::steady_clock::time_point target{};  // the vblank the frame being run aims at
    std::chrono::nanoseconds budget(std::chrono::nanoseconds period) const {
        const std::size_t n = emu_ms.size(), from = n > 60 ? n - 60 : 0;
        float worst = 10.0f;
        if (n > from)
            worst = *std::max_element(emu_ms.begin() + static_cast<std::ptrdiff_t>(from), emu_ms.end());
        const auto b = std::chrono::nanoseconds(static_cast<long long>(worst * 1e6)) + pad;
        return std::min(b, period - margin - std::chrono::nanoseconds(1'000'000));
    }
    void note_emulation(float ms) {
        if (emu_ms.size() >= 120)
            emu_ms.erase(emu_ms.begin(), emu_ms.begin() + 60);
        emu_ms.push_back(ms);
    }
    double note_guest_frame(std::uint64_t cycles) {
        double s = 0;
        if (last_cycles && cycles > last_cycles) {
            s = static_cast<double>(cycles - last_cycles) / 200e6;
            guest_frame_s = guest_frame_s == 0 ? s : guest_frame_s * 0.95 + s * 0.05;
        }
        last_cycles = cycles;
        return s;
    }
};

struct Live {
    Live(dream::mem::DcMemory& memory, const dream::pvr::Core& pvr) : memory_(memory), pvr_(pvr) {}
    // When the pads were last read: the start of the guest's frame, for display sync's budget.
    std::chrono::steady_clock::time_point pads_read_at{};

    // `scale` multiplies the resolution the geometry is drawn at. The frame is resampled back to
    // the guest's own framebuffer on the way into video memory, so a higher setting sharpens the
    // geometry without lying to the guest about the size of its screen.
    // The render target's size for a scale and shape (also used when the shape
    // changes at run time).
    // kScalePanel: the largest 4:3 picture the window's own pixels hold, so the presenter copies
    // one pixel to one pixel.
    void target_size(unsigned scale, float render_aspect, std::uint32_t& w, std::uint32_t& h) const {
        const float wide = render_aspect > 0.0f ? render_aspect / (4.0f / 3.0f) : 1.0f;
        if (scale == dream::host::HostControls::kScalePanel) {
            int pw = 0, ph = 0;
            window.pixel_size(pw, ph);
            h = static_cast<std::uint32_t>(std::max(kGuestHeight / 2, std::min<std::uint32_t>(
                static_cast<std::uint32_t>(std::max(ph, 1)), static_cast<std::uint32_t>(std::max(pw, 1)) * 3 / 4)));
            w = static_cast<std::uint32_t>(static_cast<float>(h) * (4.0f / 3.0f) * wide + 0.5f);
            return;
        }
        w = static_cast<std::uint32_t>(static_cast<float>(kGuestWidth * scale) * wide + 0.5f);
        h = kGuestHeight * scale;
    }
    void target_size(float render_aspect, std::uint32_t& w, std::uint32_t& h) const {
        target_size(scale_, render_aspect, w, h);
    }

    // Rebuilds the render target(s) at a new shape (Expanded aspect follows the
    // window). On the guest thread at a vblank: the device is idled, the old frame is not shown
    // again, and the next render fills the new target.
    // A new scale (Resolution applied in game) takes the same path.
    bool retarget_wanted() const {
        return (want_render_aspect > 0.0f && std::fabs(want_render_aspect - render_aspect_) >= 0.005f) ||
               (want_scale != kNoScale && want_scale != scale_);
    }

    void retarget() {
        const bool new_aspect =
            want_render_aspect > 0.0f && std::fabs(want_render_aspect - render_aspect_) >= 0.005f;
        const bool new_scale = want_scale != kNoScale && want_scale != scale_;
        if (!new_aspect && !new_scale)
            return;
        if (new_aspect)
            render_aspect_ = want_render_aspect;
        if (new_scale)
            scale_ = want_scale;
        std::uint32_t w = 0, h = 0;
        target_size(render_aspect_, w, h);
        if (w == offscreen.width() && h == offscreen.height())
            return;
        vkDeviceWaitIdle(window.context().device());
        // The presenter's sets name the old views by handle; the new ones may reuse the values.
        presenter.forget_views();
        overlay_presenter.forget_views();
        const bool readback = offscreen.readback;
        offscreen.destroy();
        offscreen.readback = readback;
        if (!offscreen.create(window.context(), w, h))
            std::fprintf(stderr, "window: cannot rebuild the render target: %s\n",
                         offscreen.error().c_str());
        else
            std::printf("window: now drawing at %ux%u\n", w, h);
        // The old pass is gone: pipelines made from now on use the new (compatible) one.
        renderer.set_render_pass(offscreen.render_pass());
        if (interpolate) {
            interp_off.destroy();
            interp_off.readback = false;
            interp_off.create(window.context(), w, h);
            interp_ready = false;
            have_prev = false;
        }
        if (rtt_on) {
            renderer.textures().drop_rtt();  // captured at the old scale
            rtt_off.destroy();
            rtt_off.readback = false;
            rtt_on = rtt_off.create(window.context(), w, h);
        }
        await_render = true;
        std::printf("window: render target now %ux%u (aspect %.3f)\n", w, h,
                    static_cast<double>(render_aspect_));
    }

    bool start(unsigned scale, const std::string& title, float render_aspect = 0.0f,
               bool fullscreen = false) {
        scale_ = scale;
        render_aspect_ = want_render_aspect = render_aspect;
        // --render-aspect: an anamorphic widescreen title still addresses a 640x480
        // screen, but its view is squeezed horizontally; drawing it into a target that is wider
        // by the same factor keeps the horizontal resolution of a native wide render.
        const std::uint32_t win_w = static_cast<std::uint32_t>(720.0f * (render_aspect > 0.0f ? render_aspect : 4.0f / 3.0f));
        if (!window.create(title.c_str(), win_w, 720)) {
            error = window.error();
            return false;
        }
        if (fullscreen)
            window.set_fullscreen(true);
        std::uint32_t w = 0, h = 0;  // after the window: kScalePanel takes its size
        target_size(render_aspect, w, h);
        if (!offscreen.create(window.context(), w, h)) {
            error = offscreen.error();
            return false;
        }
        if (interpolate && interpolate_only_above_60) {
            const float hz = window.refresh_rate();
            interpolate = hz > 61.0f;
            std::printf("interpolation: display %.0f Hz, %s\n", static_cast<double>(hz),
                        interpolate ? "on" : "off (60 Hz or unknown)");
        }
        // Geometry interpolation: a second target for the frame between two renders. The
        // in-between frame is only ever sampled by the presenter, so it is not read back.
        interp_off.readback = false;
        if (interpolate && !interp_off.create(window.context(), w, h)) {
            error = interp_off.error();
            return false;
        }
        rtt_off.readback = false;
        rtt_on = rtt_off.create(window.context(), w, h);
        // The renderer's pipelines are built against the offscreen render pass, not the window's:
        // that is where the geometry is drawn.
        if (!renderer.create(window.context(), offscreen.render_pass())) {
            error = renderer.error();
            return false;
        }
        if (!presenter.create(window.context(), window.render_pass())) {
            error = presenter.error();
            return false;
        }
        overlay_presenter.overlay_layer = true;  // the in-game menu, blended on top
        if (!overlay_presenter.create(window.context(), window.render_pass())) {
            error = overlay_presenter.error();
            return false;
        }
        // Pipelines from earlier runs, created now rather than mid-game. Beside the bindings, or
        // in --cache-dir (a NextUI window has no bindings file).
        {
            std::string dir = g_cache_dir.empty() ? std::string{} : g_cache_dir + "/";
            if (dir.empty()) {
                const std::string bindings = dream::render::vk::default_bindings_path();
                const auto slash = bindings.find_last_of("/\\");
                if (slash != std::string::npos)
                    dir = bindings.substr(0, slash + 1);
            }
            if (!dir.empty()) {
                const auto t0 = std::chrono::steady_clock::now();
                renderer.use_pipeline_cache(dir + "pipeline_cache.bin");
                if (renderer.prewarmed)
                    std::printf("pipelines: %u prepared in %.1f ms\n", renderer.prewarmed,
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
            }
        }
        if (!g_pipeline_keys.empty()) {
            std::vector<std::uint32_t> keys;
            std::ifstream in(g_pipeline_keys);
            for (std::string line; std::getline(in, line);) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(line.c_str(), &end, 16);
                if (!line.empty() && line[0] != '#' && end != line.c_str())
                    keys.push_back(static_cast<std::uint32_t>(v));
            }
            const auto t0 = std::chrono::steady_clock::now();
            const std::uint32_t made = renderer.prewarm(keys);
            std::printf("pipelines: %u of the port's %zu made in %.1f ms\n", made, keys.size(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                            .count());
        }
        renderer.set_memory(memory_.vram(), dream::mem::DcMemory::kVramSize,
                            pvr_.render_regs() + 0x1000 / 4, palette_format());
        std::printf("window: %s, drawing at %ux%u\n", window.context().caps().device_name.c_str(),
                    w, h);
        std::printf("window: presenting through %s\n", window.present_path().c_str());
        return true;
    }

    // The guest has handed the hardware a display list. Draw it, then put the result where the
    // hardware would have put it.
    void render(const dream::pvr::RenderWords& words) {
        // Kept (shared, not copied) to draw again while an extension pauses the guest.
        last_stream = words;
        // The render thread decodes it too, when every extension allows it this frame.
        if (threaded_ && frames_offthread()) {
            hand_over(&words);
            return;
        }
        decoder.reset();
        words->for_each([this](const std::uint32_t* w, std::size_t n) { decoder.feed_stream(w, n); });
        // The plane the whole frame sits on is not in the stream: the title writes it into the
        // parameter buffer and points a register at it. Without it the screen behind the geometry
        // is whatever the renderer cleared to.
        dream::render::add_background(decoder.frame(), pvr_.render_regs(), memory_.vram(),
                                      dream::mem::DcMemory::kVramSize);
        // Geometry reaching far off the screen, cut to a sane rectangle: some GPUs' clippers lose
        // depth precision on triangles millions of pixels across.
        dream::render::clip_far_polygons(decoder.frame());
        for (auto* e : dream::host::extensions()) e->on_frame(decoder.frame());
        if (threaded_) {
            hand_over(nullptr);
            return;
        }
        draw(decoder.frame(), pvr_.render_regs(),
             [this](auto&& f) { memory_.take_vram_dirty(std::forward<decltype(f)>(f)); });
    }

    static bool frames_offthread() {
        for (auto* e : dream::host::extensions())
            if (!e->frame_offthread_ok())
                return false;
        return true;
    }

    // Everything after decoding a render: the textures, the draw, and where the frame went. On the
    // render thread, `regs` is the render-register block as of the hand-over and `take_dirty`
    // yields the video memory the guest wrote before it.
    template <typename TakeDirty>
    void draw(const dream::render::Frame& frame, const std::uint32_t* regs, TakeDirty&& take_dirty) {
        // PT_ALPHA_REF: the punch-through threshold the guest chose, which changes per scene.
        geometry.alpha_ref = static_cast<float>(regs[0x11C / 4] & 0xFFu) / 255.0f;
        geometry.shadow_scale = static_cast<float>(regs[0x074 / 4] & 0xFFu) / 256.0f;
        dream::render::describe_fog(regs, geometry.fog);
        // Auto-sort: with FPU_PARAM_CFG bit 21 the region array's per-tile PreSort bit decides
        // (not read yet: games that use type-2 headers sort); otherwise ISP_FEED_CFG bit 0 clear
        // means the hardware sorts.
        geometry.autosort = (regs[0x07C / 4] & (1u << 21)) != 0 || (regs[0x098 / 4] & 1u) == 0;
        // Palette memory is re-read every render but only invalidates the cache when it has
        // actually changed, which is rare; dropping it every frame would decode every texture
        // every frame.
        renderer.textures().begin_frame();  // LRU eviction between renders
        // Textures the guest has overwritten since the last render are decoded again.
        take_dirty([&](std::uint32_t begin, std::uint32_t end) {
            renderer.textures().invalidate_range(begin, end, /*rendered=*/false);
        });
        renderer.textures().set_palette(regs + 0x1000 / 4, palette_format(regs));
        // A render into texture memory: drawn on its own target and kept on the GPU. It is not a
        // frame to show, so presentation and interpolation never see it.
        if (rtt_on && (regs[0x060 / 4] & 0x01000000u) != 0) {
            render_to_texture(frame, regs);
            return;
        }
        if (interpolate) {
            interp_ready = false;
            if (have_prev) {
                dream::render::BlendStats st;
                if (dream::render::blend_frames(prev_frame, frame, interp_opts, interp_pairs,
                                                interp_frame, st) &&
                    interp_off.render(renderer, interp_frame, geometry))
                    interp_ready = true;
            }
            prev_frame = frame;
            have_prev = true;
        }
        // On the render thread, the guest's thread did this bookkeeping at the hand-over.
        if (!threaded_)
            await_render = false;
        if (!offscreen.render(renderer, frame, geometry)) {
            if (!reported_error) {
                reported_error = true;
                std::fprintf(stderr, "render: %s\n", offscreen.error().c_str());
            }
            return;
        }
        if (!threaded_)
            note_target(regs);
    }

    // --- the render thread ------------------------------------------------------------------------
    // On the fast path a frame's Vulkan work (textures, recording, submission, the present) runs
    // here, in the order the guest handed it over. What reads guest state stays on the guest's
    // thread: decoding the display list, the background plane, the displayed framebuffer. What is
    // rare (the menus, overlays, the counter, interpolation, a new target) first drains the queue
    // and then runs inline, so only one thread ever touches Vulkan at a time.
    // A borrowing render's hand-off: its decode and the guest's next render start race for the
    // borrowed words. The decode takes them (kDecoding) and reads them where they are; or the
    // guest, finding them not taken, copies them (kRescuing, then kRescued) and goes on, and the
    // decode reads the copy. A decode already under way is waited for (kDone).
    struct Borrow {
        enum : int { kPending, kDecoding, kDone, kRescuing, kRescued };
        std::atomic<int> state{kPending};
        std::vector<std::uint32_t> rescued;  // the borrowed pieces' words, in order
    };
    struct RenderJob {
        bool present = false;
        // A render: the decoded frame, the render-register block at the hand-over (8 KB: the
        // settings, the palette, the fog table), the video memory written before it, its number.
        dream::render::Frame frame;
        // Or: the parameter stream, decoded on the render thread, with the background
        // plane read at the hand-over (append_background).
        bool decode = false;
        dream::pvr::RenderWords stream;  // dropped once decoded, so the producer can reuse it
        std::shared_ptr<Borrow> borrow;  // when the stream borrowed
        dream::render::Frame background;
        std::vector<std::uint32_t> regs;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> dirty;
        // The guest's floating-point mode at the hand-over (usually the SH-4's rounding toward zero
        // and denormal flushing): the renderer's CPU side must run in it, or its vertex arithmetic
        // differs in the last bit.
        dream::fenv::Mode fmode;
        // A present: what is displayed, for --screenshot-at.
        dream::render::FramebufferInfo shown;
        std::uint64_t guest_frame = 0;
    };

    void start_render_thread() {
        threaded_ = true;
        render_thread_ = std::thread([this] { render_loop(); });
        decoding_ = true;
        decode_thread_ = std::thread([this] { decode_loop(); });
    }

    // Draws whatever is queued, then ends the threads. Safe to call twice.
    void stop_render_thread() {
        if (!render_thread_.joinable())
            return;
        if (decode_thread_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(jobs_mutex_);
                decode_quit_ = true;
            }
            decode_cv_.notify_all();
            decode_thread_.join();  // what it had queued is the render thread's now
            decoding_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            quit_ = true;
        }
        jobs_cv_.notify_all();
        render_thread_.join();
        threaded_ = false;
    }

    // Waits until the decode and render threads have nothing queued or running: the guest's
    // thread may then touch Vulkan itself.
    void drain() {
        if (!threaded_)
            return;
        std::unique_lock<std::mutex> lock(jobs_mutex_);
        idle_cv_.wait(lock, [this] {
            return decode_jobs_.empty() && !decode_busy_ && jobs_.empty() && !busy_;
        });
    }

    RenderJob take_job() {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        if (spare_.empty())
            return RenderJob{};
        RenderJob job = std::move(spare_.back());
        spare_.pop_back();
        return job;
    }

    void push(RenderJob&& job) {
        std::unique_lock<std::mutex> lock(jobs_mutex_);
        if (job.present) {
            // One present at most behind the guest: the picture stays within a frame of the game.
            idle_cv_.wait(lock, [this] { return presents_queued_ == 0; });
            ++presents_queued_;
        }
        if (decoding_) {
            decode_jobs_.push_back(std::move(job));
            decode_cv_.notify_one();
            return;
        }
        jobs_.push_back(std::move(job));
        jobs_cv_.notify_one();
    }

    // A render job's parameter stream into its own frame, with the background plane and the far
    // clipping (the decode thread's work, or the render thread's without it).
    void decode_into(RenderJob& job) {
        parse_into(job, false);
        finish_decode(job);
    }

    // A render job's parameter stream parsed into its own frame: the part of its decode that reads
    // the guest's words, so a borrowing render hands them back here. `copy_first` (the decode
    // thread): the borrowed words are copied aside and handed back before the parse, which reads
    // the copy, so a guest waiting to reuse its buffer waits for the copy, not the whole parse.
    void parse_into(RenderJob& job, bool copy_first) {
        rt_decoder_.reset();
        const std::uint32_t* copy = nullptr;  // the borrowed words copied: by the guest, or here
        if (job.borrow) {
            int taken = Borrow::kPending;
            if (!job.borrow->state.compare_exchange_strong(taken, Borrow::kDecoding, std::memory_order_acq_rel)) {
                // The guest got there first: its copy, once made (a few hundred kilobytes).
                while (job.borrow->state.load(std::memory_order_acquire) == Borrow::kRescuing)
                    std::this_thread::yield();
                copy = job.borrow->rescued.data();
            } else if (copy_first) {
                decode_copy_.clear();
                for (const auto& p : job.stream->pieces)
                    if (p.borrowed)
                        decode_copy_.insert(decode_copy_.end(), p.borrowed, p.borrowed + p.count);
                hand_back(job);
                copy = decode_copy_.data();
            }
        }
        if (copy) {
            for (const auto& p : job.stream->pieces) {
                if (p.borrowed) {
                    rt_decoder_.feed_stream(copy, p.count);
                    copy += p.count;
                } else {
                    rt_decoder_.feed_stream(job.stream->words.data() + p.offset, p.count);
                }
            }
        } else {
            job.stream->for_each([this](const std::uint32_t* w, std::size_t n) { rt_decoder_.feed_stream(w, n); });
            if (job.borrow)
                hand_back(job);
        }
        job.borrow.reset();
        job.stream.reset();
        std::swap(job.frame, rt_decoder_.frame());  // the decoder clears what it gets back
    }

    // A borrowing render's words are read (or copied): the guest may write them again
    // (wait_borrowed).
    void hand_back(RenderJob& job) {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        job.borrow->state.store(Borrow::kDone, std::memory_order_release);
        decoded_cv_.notify_all();
    }

    // The rest of a render job's decode, on its parsed frame: the background plane, the far
    // clipping and the extensions' work.
    void finish_decode(RenderJob& job) {
        dream::render::append_background(job.frame, job.background);
        dream::render::clip_far_polygons(job.frame);
        for (auto* e : dream::host::extensions()) e->on_frame_offthread(job.frame);
        job.decode = false;
    }

    // The decode thread's floating-point mode: the guest's at a job's hand-over (as the render
    // thread draws in it).
    void decode_mode(const dream::fenv::Mode& mode) {
        if (!(mode == decode_fmode_)) {
            dream::fenv::apply(mode);
            decode_fmode_ = mode;
        }
    }

    // The decode stage: each job parsed, then finished and passed on to the render thread, in
    // order. Parsing goes first: a job queued while others wait to be finished is parsed before
    // them, up to two ahead, since the guest may be waiting to write over the words it borrowed
    // and only the parse reads them.
    void decode_loop() {
        constexpr std::size_t kParsedAhead = 2;
        std::deque<RenderJob> parsed;  // parsed, to be finished and passed on in order
        std::unique_lock<std::mutex> lock(jobs_mutex_);
        for (;;) {
            if (decode_jobs_.empty() && parsed.empty()) {
                decode_cv_.wait(lock, [this] { return decode_quit_ || !decode_jobs_.empty(); });
                if (decode_jobs_.empty())
                    return;  // asked to stop, and everything queued is passed on
            }
            decode_busy_ = true;
            if (!decode_jobs_.empty() && parsed.size() < kParsedAhead) {
                RenderJob job = std::move(decode_jobs_.front());
                decode_jobs_.pop_front();
                lock.unlock();
                if (!job.present && job.decode) {
                    decode_mode(job.fmode);
                    parse_into(job, true);
                }
                parsed.push_back(std::move(job));
                lock.lock();
                continue;
            }
            RenderJob job = std::move(parsed.front());
            parsed.pop_front();
            lock.unlock();
            if (!job.present && job.decode) {
                decode_mode(job.fmode);
                finish_decode(job);
            }
            lock.lock();
            jobs_.push_back(std::move(job));
            jobs_cv_.notify_one();
            if (decode_jobs_.empty() && parsed.empty()) {
                decode_busy_ = false;
                idle_cv_.notify_all();
            }
        }
    }

public:
    // A scene's lists begin (pvr::Core::on_lists_begin): they may be written over the words a
    // render two back borrowed (Stream::borrow), so every borrowing render but the last handed
    // over is settled first. This comes after the next frame's game code, later than the next
    // render start, so the decode has usually finished. Not at the TA list init: Kamui writes a
    // scene's lists before it.
    void lists_begin() {
        while (borrowed_.size() > 1) {
            settle_borrowed(*borrowed_.front().first, *borrowed_.front().second);
            borrowed_.pop_front();
        }
    }

private:
    // The words a render borrowed are read: by its decode, waited for when it is under way, or
    // else by a copy made here for it (Borrow).
    void settle_borrowed(Borrow& b, const dream::pvr::RenderStream& words) {
        int taken = Borrow::kPending;
        if (b.state.compare_exchange_strong(taken, Borrow::kRescuing, std::memory_order_acq_rel)) {
            for (const auto& p : words.pieces)
                if (p.borrowed)
                    b.rescued.insert(b.rescued.end(), p.borrowed, p.borrowed + p.count);
            b.state.store(Borrow::kRescued, std::memory_order_release);
        } else if (taken == Borrow::kDecoding) {
            std::unique_lock<std::mutex> lock(jobs_mutex_);
            decoded_cv_.wait(lock, [&b] { return b.state.load(std::memory_order_acquire) == Borrow::kDone; });
        }
    }

public:

    // At the guest's render start: what the render thread needs, taken now.
    // `stream`: the parameter stream, for the render thread to decode; null: the guest's thread
    // decoded it into `decoder`.
    void hand_over(const dream::pvr::RenderWords* stream) {
        const std::uint32_t* regs = pvr_.render_regs();
        RenderJob job = take_job();
        job.present = false;
        job.decode = stream != nullptr;
        if (stream) {
            job.stream = *stream;
            job.background.clear();
            dream::render::add_background(job.background, regs, memory_.vram(),
                                          dream::mem::DcMemory::kVramSize);
        } else {
            std::swap(job.frame, decoder.frame());  // the decoder clears what it gets back
        }
        job.regs.assign(regs, regs + 0x2000 / 4);
        job.dirty.clear();
        memory_.take_vram_dirty(
            [&job](std::uint32_t begin, std::uint32_t end) { job.dirty.emplace_back(begin, end); });
        job.borrow.reset();
        if (stream && (*stream)->borrows) {
            job.borrow = take_borrow();
            borrowed_.emplace_back(job.borrow, *stream);
        }
        job.fmode = dream::fenv::current();
        // Where the frame goes, for the next VBlank's present: from the same registers the render
        // reads.
        if (!(rtt_on && (regs[0x060 / 4] & 0x01000000u) != 0)) {
            await_render = false;
            note_target(regs);
        }
        push(std::move(job));
    }

    void render_loop() {
        std::unique_lock<std::mutex> lock(jobs_mutex_);
        for (;;) {
            jobs_cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (jobs_.empty())
                return;  // asked to stop, and everything queued is drawn
            RenderJob job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
            lock.unlock();
            if (job.present) {
                present_job(job);
            } else {
                if (!(job.fmode == thread_fmode_)) {
                    dream::fenv::apply(job.fmode);
                    thread_fmode_ = job.fmode;
                }
                if (job.decode)
                    decode_into(job);
                draw(job.frame, job.regs.data(), [&job](auto&& f) {
                    for (const auto& [begin, end] : job.dirty) f(begin, end);
                });
            }
            lock.lock();
            busy_ = false;
            if (job.present)
                --presents_queued_;
            else
                spare_.push_back(std::move(job));
            idle_cv_.notify_all();
        }
    }

    // The fast path's present: the rendered frame sampled where it is, on the GPU.
    void present_job(const RenderJob& job) {
        std::uint32_t image = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!window.begin_frame(image, cmd)) {  // waits for a free image
            window.recreate_swapchain();
            return;
        }
        // A NextUI panel: the frame is blitted straight into fb0's page, with no pass of the
        // panel's size drawn and copied after it. The image goes back to being sampled for
        // whatever reads it next (a screenshot).
        if (!present_blit(cmd)) {
            begin_window_pass(cmd, image);
            presenter.draw_view(cmd, offscreen.sampled_view(), offscreen.width(), offscreen.height(),
                                window.extent());
            vkCmdEndRenderPass(cmd);
        }
        if (!window.end_frame(image))
            window.recreate_swapchain();
        last_present = std::chrono::steady_clock::now();
        ++presented;
        if (screenshot_at.count(presented))
            screenshot(job.shown, true, job.guest_frame);
    }

    // The fast path's frame (sampled-from, as render() leaves it with readback off) blitted into
    // the window's page where the window can (a NextUI panel), and back to sampled after.
    bool present_blit(VkCommandBuffer cmd) {
        if (!window.blits())
            return false;
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = offscreen.colour_image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        window.blit_frame(cmd, offscreen.colour_image(), offscreen.extent(), presenter.smooth);
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        return true;
    }

    void begin_window_pass(VkCommandBuffer cmd, std::uint32_t image) {
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].depthStencil = {0.0f, 0};
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = window.render_pass();
        rpbi.framebuffer = window.framebuffer(image);
        rpbi.renderArea.extent = window.extent();
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(window.extent().width),
                                  static_cast<float>(window.extent().height),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, window.extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void render_to_texture(const dream::render::Frame& frame, const std::uint32_t* regs) {
        dream::render::FramebufferInfo target;
        if (!dream::render::describe_write_framebuffer(regs, kGuestWidth, kGuestHeight, target))
            return;
        rtt_off.after_pass = [&](VkCommandBuffer cmd, VkImage image, VkExtent2D extent) {
            renderer.textures().capture_rtt(cmd, image, extent.width, extent.height, target.address,
                                            target.width, target.height);
        };
        // Only the region capture_rtt reads is drawn, plus one pixel for its filter, as the
        // PowerVR drew only the texture's tiles.
        const std::uint32_t tw = rtt_off.width(), th = rtt_off.height();
        rtt_off.area = {std::min(tw, (std::min(target.width, kGuestWidth) * tw + kGuestWidth - 1) / kGuestWidth + 1),
                        std::min(th, (std::min(target.height, kGuestHeight) * th + kGuestHeight - 1) / kGuestHeight + 1)};
        const bool ok = rtt_off.render(renderer, frame, geometry);
        rtt_off.after_pass = nullptr;
        rtt_off.area = {};
        if (!ok && !reported_error) {
            reported_error = true;
            std::fprintf(stderr, "render to texture: %s\n", rtt_off.error().c_str());
        }
    }

    // True for a buffer the renderer has drawn into at some point. A title alternates between two
    // and does not swap the displayed one on every render, so asking whether the displayed buffer
    // is *the* most recent target is too strict; asking whether it is one of ours is the question
    // that matters. A buffer we have never rendered to holds pixels the title put there itself,
    // and those have to come out of video memory.
    bool rendered_into(std::uint32_t address) const {
        for (std::uint32_t a : targets_)
            if (a == address)
                return true;
        return false;
    }

    // Where the frame went: presentation needs to know which buffer holds what the renderer drew.
    void note_target(const std::uint32_t* regs) {
        dream::render::FramebufferInfo display;
        std::uint32_t w = kGuestWidth, h = kGuestHeight;
        if (dream::render::describe_framebuffer(regs, display)) {
            w = display.width;
            h = display.height;
        }
        dream::render::FramebufferInfo target;
        if (!dream::render::describe_write_framebuffer(regs, w, h, target))
            return;
        if (!rendered_into(target.address) && targets_.size() < 8)
            targets_.push_back(target.address);
    }

    // Window events and pad state, once per frame. The vblank hook calls this after
    // its pacing sleep, just before the guest's pads are read, so the input the game sees is
    // sampled right before it runs the frame rather than a whole sleep earlier; present() then
    // works on that same poll (its hotkeys are edges computed per poll, so a second poll would
    // lose them). Returns false when the user has closed the window.
    bool poll_input() {
        input_polled = true;
        return window_open = window.poll();
    }

    // Vertical blank: show what the video hardware would be scanning out, and read the keyboard.
    // Returns false when the user has closed the window.
    bool present(std::uint64_t frame, std::uint64_t guest_cycles) {
        guest_frame = frame;
        if (!input_polled)
            poll_input();
        input_polled = false;
        if (!window_open)
            return false;
        if (threaded_ && retarget_wanted())
            drain();
        retarget();
        update_counter(guest_cycles);
        pump_menu();

        dream::render::FramebufferInfo fb;
        const bool described = dream::render::describe_framebuffer(pvr_.render_regs(), fb);
        // The guest is displaying the buffer the renderer drew into, so show what was drawn rather
        // than a copy of it squeezed back through the guest's pixel format. This is also what makes
        // --scale visible: the window gets the full rendered resolution.
        // Not while a rebuilt target waits for its first render: its image is not drawn yet.
        from_renderer = described && fb.enabled && rendered_into(fb.address) && !await_render;
        if (from_renderer) {
            shown = fb;
            shown.width = offscreen.width();
            shown.height = offscreen.height();
            have_frame = true;
        } else if (described && fb.enabled &&
                   dream::render::decode_framebuffer(fb, memory_.vram(),
                                                     dream::mem::DcMemory::kVramSize, pixels)) {
            shown = fb;
            have_frame = true;
        }
        if (!have_frame)
            return true;  // nothing displayable yet: the window keeps its cleared background

        // The in-between frame goes first, before the real frame's swapchain image is acquired
        // (acquiring twice would wait on a fence the first acquire just reset).
        const bool overlay_up = overlay_open();
        // An open overlay owns the keyboard and pads; the game keeps running with them released.
        window.set_game_input_blocked(overlay_up);
        // The render thread's fast path: a frame the renderer drew, shown as it is. Anything else
        // waits for the queue to empty and runs here.
        if (threaded_) {
            using dream::render::vk::Control;
            if (from_renderer && !show_fps && !menu.is_open() && !overlay_up &&
                !window.pressed(Control::Screenshot)) {
                RenderJob job = take_job();
                job.present = true;
                job.shown = shown;
                job.guest_frame = frame;
                push(std::move(job));
                if (window.pressed(Control::ToggleFps)) {
                    show_fps = !show_fps;
                    fps_mark = {};
                }
                return true;
            }
            drain();
        }
        if (interpolate && interp_ready && from_renderer && !offscreen.readback && !show_fps &&
            !menu.is_open() && !overlay_up) {
            interp_ready = false;
            std::uint32_t iimage = 0;
            VkCommandBuffer icmd = VK_NULL_HANDLE;
            if (window.begin_frame(iimage, icmd)) {
                VkClearValue iclear[2]{};
                iclear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                iclear[1].depthStencil = {0.0f, 0};
                VkRenderPassBeginInfo ib{};
                ib.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                ib.renderPass = window.render_pass();
                ib.framebuffer = window.framebuffer(iimage);
                ib.renderArea.extent = window.extent();
                ib.clearValueCount = 2;
                ib.pClearValues = iclear;
                vkCmdBeginRenderPass(icmd, &ib, VK_SUBPASS_CONTENTS_INLINE);
                const VkViewport iv{0.0f, 0.0f, static_cast<float>(window.extent().width),
                                    static_cast<float>(window.extent().height), 0.0f, 1.0f};
                const VkRect2D isc{{0, 0}, window.extent()};
                vkCmdSetViewport(icmd, 0, 1, &iv);
                vkCmdSetScissor(icmd, 0, 1, &isc);
                presenter.draw_view(icmd, interp_off.sampled_view(), interp_off.width(),
                                    interp_off.height(), window.extent());
                vkCmdEndRenderPass(icmd);
                if (!window.end_frame(iimage))
                    window.recreate_swapchain();
                // Half a guest frame (59.94 Hz) before the real one.
                if (pace_interpolation) {
                    const auto due = last_present + std::chrono::microseconds(8342);
                    if (due > std::chrono::steady_clock::now())
                        std::this_thread::sleep_until(due);
                }
            }
        }
        std::uint32_t image = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!window.begin_frame(image, cmd)) {
            window.recreate_swapchain();
            return true;
        }
        // Direct present: a rendered frame nothing needs on the host is sampled where it
        // is, on the GPU. Overlays drawn on the CPU still take the copy. An overlay that can hand
        // over a window-sized image is blended on the GPU, so it does not force the frame back to
        // the CPU.
        const std::uint32_t* overlay_px = nullptr;
        bool overlay_changed = false;
        if (overlay_up) {
            const VkExtent2D ext = window.extent();
            for (auto* e : overlays)
                if (e->overlay_open() &&
                    (overlay_px = e->overlay_image(ext.width, ext.height, overlay_changed)))
                    break;
            if (overlay_px && (overlay_changed || overlay_w != ext.width ||
                               overlay_h != ext.height)) {
                overlay_w = ext.width;
                overlay_h = ext.height;
                overlay_dirty = true;
            }
        }
        const bool cpu_overlay = overlay_up && !overlay_px;
        const bool direct =
            from_renderer && !offscreen.readback && !show_fps && !menu.is_open() && !cpu_overlay;
        const std::uint32_t* source =
            direct ? nullptr : (from_renderer ? offscreen.pixels() : pixels.data());
        // Composited into the frame as it is handed to the GPU, so it costs no extra copy of the
        // picture. Deliberately not into the buffer screenshot() reads: F12 produces the game's own
        // pixels.
        if (direct) {
            // nothing to upload
        } else if (cpu_overlay && source) {
            // an extension overlay blends into the frame, which means reading it back.
            // The staging buffer `decorate` hands over is write-combined on some GPUs (reads cost
            // ~100x), so the overlay works on a cached host copy that is then uploaded as is.
            overlay_frame.assign(source, source + static_cast<std::size_t>(shown.width) *
                                                      shown.height);
            std::uint32_t* px = overlay_frame.data();
            if (show_fps)
                draw_counter(px, shown.width, shown.height);
            for (auto* e : overlays)
                if (e->overlay_open())
                    e->draw_overlay(px, shown.width, shown.height, window.extent().width,
                                    window.extent().height);
            if (menu.is_open())
                menu.draw(px, shown.width, shown.width, shown.height, window.devices());
            presenter.upload(cmd, px, shown.width, shown.height);
        } else if (show_fps || menu.is_open())
            presenter.upload(cmd, source, shown.width, shown.height,
                             [this](std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
                                 if (show_fps)
                                     draw_counter(px, w, h);
                                 // Last, so the screen sits over the counter rather than under it.
                                 if (menu.is_open())
                                     menu.draw(px, w, w, h, window.devices());
                             });
        else
            presenter.upload(cmd, source, shown.width, shown.height);
        // The overlay image goes up only when the menu redrew or the window changed size.
        if (overlay_px && overlay_dirty) {
            overlay_presenter.upload(cmd, overlay_px, overlay_w, overlay_h);
            overlay_dirty = false;
        }
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].depthStencil = {0.0f, 0};
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = window.render_pass();
        rpbi.framebuffer = window.framebuffer(image);
        rpbi.renderArea.extent = window.extent();
        rpbi.clearValueCount = 2;
        rpbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(window.extent().width),
                                  static_cast<float>(window.extent().height),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, window.extent()};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (direct)
            presenter.draw_view(cmd, offscreen.sampled_view(), offscreen.width(), offscreen.height(),
                                window.extent());
        else
            presenter.draw(cmd, window.extent());
        if (overlay_px)
            overlay_presenter.draw(cmd, window.extent());
        vkCmdEndRenderPass(cmd);
        if (!window.end_frame(image))
            window.recreate_swapchain();
        last_present = std::chrono::steady_clock::now();
        ++presented;
        if (window.pressed(dream::render::vk::Control::ToggleFps)) {
            show_fps = !show_fps;
            fps_mark = {};  // start the next average fresh rather than from whenever it last ran
        }
        if (window.pressed(dream::render::vk::Control::Screenshot))
            screenshot();
        if (screenshot_at.count(presented))
            screenshot();
        return true;
    }

    // What the player is pressing, as a Maple controller sees it. The buttons are active low.
    //
    // Everything here comes through the player's bindings rather than off fixed keys, so a pad and
    // the keyboard are both live and either can be rebound. The stick and the triggers are read as
    // values rather than as flags: a real pad's travel reaches the guest, and a keyboard still
    // produces the extremes it always did, because a digital source reads a full 1.0.
    void read_controls(dream::maple::ControllerState& pad, unsigned player = 0) const {
        using dream::render::PadControl;
        auto pad_held = [&](PadControl c) { return window.pad_held(player, c); };
        auto pad_value = [&](PadControl c) { return window.pad_value(player, c); };
        std::uint16_t held = 0;
        if (pad_held(PadControl::Up))
            held |= dream::maple::kUp;
        if (pad_held(PadControl::Down))
            held |= dream::maple::kDown;
        if (pad_held(PadControl::Left))
            held |= dream::maple::kLeft;
        if (pad_held(PadControl::Right))
            held |= dream::maple::kRight;
        if (pad_held(PadControl::A))
            held |= dream::maple::kA;
        if (pad_held(PadControl::B))
            held |= dream::maple::kB;
        if (pad_held(PadControl::X))
            held |= dream::maple::kX;
        if (pad_held(PadControl::Y))
            held |= dream::maple::kY;
        if (pad_held(PadControl::Start))
            held |= dream::maple::kStart;
        pad.buttons = static_cast<std::uint16_t>(0xFFFFu & ~held);
        pad.joy_x = dream::maple::axis_byte(pad_value(PadControl::StickLeft),
                                            pad_value(PadControl::StickRight));
        pad.joy_y = dream::maple::axis_byte(pad_value(PadControl::StickUp),
                                            pad_value(PadControl::StickDown));
        pad.ltrigger = dream::maple::trigger_byte(pad_value(PadControl::LeftTrigger));
        pad.rtrigger = dream::maple::trigger_byte(pad_value(PadControl::RightTrigger));
    }

    // --- the binding screen --------------------------------------------------------------------

    // Its keys are the launcher's fixed ones, never the player's: rebinding the game's buttons can
    // never take away the way back out. Called once per poll, running or paused.
    void pump_menu() {
        using dream::render::vk::Control;
        // Escape does two things a keystroke apart: leave the screen, then end the run. The key has
        // to be let go before it means the second one. Without this, closing the screen with escape
        // re-arms quit under a finger that is still down, and one key repeat ends the run -- which
        // from the outside is indistinguishable from a crash.
        if (rearm_escape && !window.held(Control::Back)) {
            rearm_escape = false;
            window.set_escape_quits(overlays.empty());  // an overlay owns escape
        }
        // an overlay host has its own controls screen and gives F1 to the HUD editor.
        // The key is polled here, not taken from the event the overlay consumed, so it has to be
        // refused here as well.
        if (window.pressed(Control::Menu) && overlays.empty()) {
            if (menu.is_open())
                close_menu();
            else
                open_menu();
            return;
        }
        if (!menu.is_open())
            return;
        // A finished capture takes the frame to itself. The escape that cancelled one is also a
        // Back press, and closing the screen on it would make cancelling and leaving the same
        // keystroke.
        dream::render::Binding captured;
        bool cancelled = false;
        if (window.take_capture(captured, cancelled)) {
            menu.apply_capture(captured, cancelled);
            apply_bindings();
            return;
        }
        if (window.pressed(Control::Back)) {
            menu.back();  // one leaves a capture, a second leaves the screen
            if (!menu.is_open())
                close_menu();
            return;
        }
        if (window.pressed(Control::Up))
            menu.move(-1);
        if (window.pressed(Control::Down))
            menu.move(1);
        // The device row counts what is actually plugged in, so the selector wraps over the
        // keyboard and however many pads are connected right now.
        const unsigned devices = static_cast<unsigned>(window.devices().size());
        if (window.pressed(Control::Left))
            menu.adjust(-1, devices);
        if (window.pressed(Control::Right))
            menu.adjust(1, devices);
        if (window.pressed(Control::Start) || window.pressed(Control::A)) {
            if (menu.activate() == dream::render::InputMenu::Action::Close) {
                close_menu();
                return;
            }
            // Activating a binding row asks for an input; the window collects it for us.
            if (menu.awaiting())
                window.begin_capture();
        }
        apply_bindings();
    }

    void open_menu() {
        menu.open(window.bindings());
        bindings_dirty = false;
        rearm_escape = false;
        // While the screen is up, escape backs out of it rather than ending the run.
        window.set_escape_quits(false);
    }

    void close_menu() {
        menu.close();
        window.cancel_capture();
        rearm_escape = true;  // not until escape is released; see pump_menu()
        if (!bindings_dirty)
            return;
        bindings_dirty = false;
        if (bindings_path.empty()) {
            std::printf("bindings changed for this run only: no file to save them in\n");
            return;
        }
        if (save_bindings())
            std::printf("bindings saved to %s\n", bindings_path.c_str());
        else
            std::fprintf(stderr, "cannot write the bindings to %s\n", bindings_path.c_str());
    }

    // Applied live, so a rebound control works the moment it is bound rather than after a restart.
    // The file waits until the screen closes: a dead zone nudged five times is one save, not five.
    void apply_bindings() {
        if (!menu.dirty())
            return;
        menu.mark_saved();
        bindings_dirty = true;
        window.set_bindings(menu.bindings());
    }

    // A missing file is the ordinary first run, not a fault: the defaults are the layout the
    // launcher always had, so nothing needs to exist for the keyboard to work.
    void load_bindings() {
        dream::render::Bindings b = dream::render::Bindings::defaults();
        std::ifstream in(bindings_path, std::ios::binary);
        if (!bindings_path.empty() && in) {
            const std::string text((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            std::string warnings;
            b = dream::render::Bindings::from_text(text, &warnings);
            if (!warnings.empty())
                std::fprintf(stderr, "%s:\n%s", bindings_path.c_str(), warnings.c_str());
            std::printf("bindings: %s\n", bindings_path.c_str());
        }
        window.set_bindings(b);
    }

    bool save_bindings() const {
        std::ofstream out(bindings_path, std::ios::binary);
        if (!out)
            return false;
        const std::string text = menu.bindings().to_text();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(out);
    }

    // Averaged over a window rather than taken frame to frame: an instantaneous figure on a
    // 60 Hz display flickers between two integers and is unreadable, which is the usual reason an
    // on-screen counter gets turned off again. Half a second is long enough to settle and short
    // enough to react to a stutter.
    void update_counter(std::uint64_t guest_cycles) {
        const auto now = std::chrono::steady_clock::now();
        const double guest_s = static_cast<double>(guest_cycles) / 200e6;
        if (fps_mark == std::chrono::steady_clock::time_point{}) {
            fps_mark = now;
            fps_frames_at_mark = presented;
            fps_guest_at_mark = guest_s;
            return;
        }
        const double elapsed = std::chrono::duration<double>(now - fps_mark).count();
        if (elapsed < 0.5)
            return;
        fps_shown = static_cast<double>(presented - fps_frames_at_mark) / elapsed;
        speed_shown = (guest_s - fps_guest_at_mark) / elapsed;
        fps_mark = now;
        fps_frames_at_mark = presented;
        fps_guest_at_mark = guest_s;
    }

    // Drawn into the frame the presenter is about to upload, so its size follows --scale and it
    // stays the same size relative to the picture however the window is resized.
    void draw_counter(std::uint32_t* px, std::uint32_t w, std::uint32_t h) {
        char line[64];
        std::snprintf(line, sizeof line, "%.1f FPS  %.2fX", fps_shown, speed_shown);
        dream::render::TextStyle style;
        // One text pixel per two guest pixels at --scale 1, and the same apparent size at every
        // scale above it, so the counter neither disappears nor swamps the picture.
        style.scale = std::max(2u, 2u * (w / kGuestWidth));
        const int margin = static_cast<int>(4 * style.scale);
        dream::render::draw_text(px, w, w, h, margin, margin, line, style);
    }

    void screenshot() { screenshot(shown, from_renderer, guest_frame); }

    // What is displayed passed in: the render thread's present takes its shot from the values its
    // job carries, not from the guest's thread's, which have moved on.
    void screenshot(const dream::render::FramebufferInfo& shot_shown, bool shot_from_renderer,
                    std::uint64_t shot_guest_frame) {
        char base[64];
        std::snprintf(base, sizeof base, "screenshot-%03u.ppm", screenshots++);
        // --screenshot-dir: where they go; the working directory without one.
        const std::string name = screenshot_dir.empty() ? base : screenshot_dir + "/" + base;
        // A shot that could not be written (a missing --screenshot-dir, say) is reported as such.
        if (!screenshot_to(name, shot_shown, shot_from_renderer)) {
            std::printf("could not write %s: presented frame %llu\n", name.c_str(),
                        static_cast<unsigned long long>(presented));
            return;
        }
        std::printf("wrote %s: presented frame %llu, guest frame %llu\n", name.c_str(),
                    static_cast<unsigned long long>(presented),
                    static_cast<unsigned long long>(shot_guest_frame));
    }

    // The game's own pixels, without the overlays. False when nothing could be written.
    bool screenshot_to(const std::string& name, const dream::render::FramebufferInfo& shot_shown,
                       bool shot_from_renderer) {
        FILE* f = std::fopen(name.c_str(), "wb");
        if (!f)
            return false;
        std::fprintf(f, "P6\n%u %u\n255\n", shot_shown.width, shot_shown.height);
        const std::uint32_t* src = shot_from_renderer ? offscreen.pixels() : pixels.data();
        const std::size_t n = static_cast<std::size_t>(shot_shown.width) * shot_shown.height;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t p = src[i];
            const unsigned char rgb[3] = {static_cast<unsigned char>(p),
                                          static_cast<unsigned char>(p >> 8),
                                          static_cast<unsigned char>(p >> 16)};
            std::fwrite(rgb, 1, 3, f);
        }
        return std::fclose(f) == 0;
    }

    dream::render::PaletteFormat palette_format() const { return palette_format(pvr_.render_regs()); }
    static dream::render::PaletteFormat palette_format(const std::uint32_t* regs) {
        return static_cast<dream::render::PaletteFormat>(regs[0x108 / 4] & 3u);
    }

    static constexpr std::uint32_t kGuestWidth = 640, kGuestHeight = 480;

    dream::render::vk::Window window;
    dream::render::vk::Renderer renderer;
    dream::render::vk::Offscreen offscreen;
    // Geometry interpolation: --interpolate.
    bool interpolate = false, pace_interpolation = true, have_prev = false, interp_ready = false;
    unsigned scale_ = 1;
    float render_aspect_ = 0.0f, want_render_aspect = 0.0f;
    static constexpr unsigned kNoScale = ~0u;
    unsigned want_scale = kNoScale;  // a scale asked for at run time (kScalePanel too)
    bool await_render = false;
    bool interpolate_only_above_60 = false;
    dream::render::vk::Offscreen interp_off;
    // Render to texture on the GPU: a render the guest sends into texture memory
    // (FB_W_SOF1 bit 24) draws here, never over the frame being shown, and the texture cache keeps
    // a copy that later textures at that address sample (TextureCache::capture_rtt).
    dream::render::vk::Offscreen rtt_off;
    bool rtt_on = false;
    dream::render::Frame prev_frame, interp_frame;
    std::vector<dream::render::StripPair> interp_pairs;
    dream::render::BlendOptions interp_opts;
    std::chrono::steady_clock::time_point last_present{};
    dream::render::vk::Presenter presenter;
    dream::render::vk::Presenter overlay_presenter;  // the in-game menu layer
    std::uint32_t overlay_w = 0, overlay_h = 0;
    bool overlay_dirty = true;
    dream::render::DisplayList decoder;
    dream::render::vk::FrameGeometry geometry;
    dream::render::FramebufferInfo shown;
    std::vector<std::uint32_t> pixels;
    std::string error;
    std::atomic<std::uint64_t> presented{0};  // counted on the render thread when there is one
    std::uint64_t guest_frame = 0;
    dream::pvr::RenderWords last_stream;
    unsigned screenshots = 0;
    // The on-screen counter. Two numbers, because they answer different questions: how smooth it
    // looks (frames actually presented per second of wall clock) and whether it is keeping up
    // (guest time elapsed per second of wall clock, where 1.00x is the console's own pace).
    bool show_fps = false;
    bool input_polled = false, window_open = true;  // poll_input()
    DisplaySync sync;                                // --sync-display
    // The binding screen (F1, or a pad's select button). The guest is stopped while it is up, so
    // nobody rebinds a control mid-corner.
    dream::render::InputMenu menu;
    // Extensions drawing an in-game overlay (host_ext.h). While one is open the guest
    // is paused exactly as for the binding screen, and frames take the host-copy path so the
    // overlay can be composited.
    std::vector<dream::host::Extension*> overlays;
    std::vector<std::uint32_t> overlay_frame;  // the frame an overlay is composited into
    bool overlay_open() const {
        for (auto* e : overlays)
            if (e->overlay_open())
                return true;
        return false;
    }
    // Where a change is written back. Empty means this run only: --bindings was pointed nowhere
    // usable, or SDL could not name a settings directory on this host.
    std::string bindings_path;
    bool bindings_dirty = false;  // changed since the screen was opened
    // Escape closes the screen and also quits: quit stays disarmed until the key comes back up.
    bool rearm_escape = false;
    // --screenshot-at N: the same thing F12 does, at the Nth presented frame.
    std::set<std::uint64_t> screenshot_at;  // --screenshot-at N[,N...]
    std::string screenshot_dir;             // --screenshot-dir DIR; empty = cwd
    std::chrono::steady_clock::time_point fps_mark{};
    std::uint64_t fps_frames_at_mark = 0;
    double fps_guest_at_mark = 0;
    double fps_shown = 0, speed_shown = 0;
    bool have_frame = false, from_renderer = false, reported_error = false;
    bool threaded() const noexcept { return threaded_; }

    ~Live() { stop_render_thread(); }

private:
    bool threaded_ = false;
    std::thread render_thread_;
    std::mutex jobs_mutex_;
    std::condition_variable jobs_cv_;  // the render thread waits for work
    std::condition_variable idle_cv_;  // the guest's thread waits for room, or for the drain
    std::deque<RenderJob> jobs_;
    std::vector<RenderJob> spare_;  // finished render jobs, their vectors reused
    bool busy_ = false, quit_ = false;
    unsigned presents_queued_ = 0;  // present jobs queued or running
    dream::render::DisplayList rt_decoder_;  // the render thread's, or the decode thread's
    std::vector<std::uint32_t> decode_copy_;  // the decode thread's copy of a render's borrowed words
    dream::fenv::Mode thread_fmode_{dream::fenv::Rounding::Nearest, false};  // the host's default
    // The decode stage: a thread between the guest's and the render thread that turns each render
    // job's parameter stream into its frame, so that a frame is decoded while the one before it is
    // drawn. Every job passes through it, in order (a present stays behind the render it shows).
    std::thread decode_thread_;
    std::condition_variable decode_cv_;
    std::deque<RenderJob> decode_jobs_;
    bool decoding_ = false, decode_busy_ = false, decode_quit_ = false;
    dream::fenv::Mode decode_fmode_{dream::fenv::Rounding::Nearest, false};
    std::vector<std::shared_ptr<Borrow>> borrow_pool_;  // reused once no job holds one
    // The borrowing renders handed over and not yet settled (lists_begin), oldest first.
    std::deque<std::pair<std::shared_ptr<Borrow>, dream::pvr::RenderWords>> borrowed_;
    std::condition_variable decoded_cv_;
    std::shared_ptr<Borrow> take_borrow() {
        for (const auto& b : borrow_pool_)
            if (b.use_count() == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                b->state.store(Borrow::kPending, std::memory_order_relaxed);
                b->rescued.clear();
                return b;
            }
        borrow_pool_.push_back(std::make_shared<Borrow>());
        return borrow_pool_.back();
    }
    dream::mem::DcMemory& memory_;
    const dream::pvr::Core& pvr_;
    std::vector<std::uint32_t> targets_;  // every buffer the renderer has drawn into
};
#endif  // DREAM_WITH_RENDERER

// The serial port (SCIF, 0xFFE80000): status reads say the FIFO is empty so a polling sender
// never waits, and what is sent is dropped.
struct SerialPort final : dream::mem::MmioHandler {
    std::uint32_t read(std::uint32_t addr, unsigned) override {
        return (addr & 0xFFu) == 0x10 ? 0x0060u : 0;  // SCFSR2: TEND | TDFE
    }
    void write(std::uint32_t, std::uint32_t, unsigned) override {}
};

}  // namespace

void usage(const char* argv0, std::FILE* out) {
    std::fprintf(
        out,
        "usage: %s --config GAME.toml [options]\n"
        "\n"
        "Runs a recompiled title. Without --window it runs headless.\n"
        "\n"
        "Required:\n"
        "  --config FILE          the game's TOML (game/<id>.toml)\n"
        "\n"
        "Playing:\n"
        "  --window               open a window and play; implies sound\n"
        "  --scale N              draw at N times the guest's 640x480 (1 to 8, default 1), or\n"
        "                         'panel': the display's own pixels (the largest 4:3 they hold)\n"
        "  --render-aspect A      render target shape for anamorphic widescreen (e.g. 1.7778)\n"
        "  --fullscreen           start fullscreen (Alt+Enter toggles)\n"
        "  --interpolate          draw a blended frame between two game frames (needs a display\n"
        "                         above 60 Hz to be seen)\n"
        "  --interpolate-auto     the same, only when the window's display is above 60 Hz\n"
        "  --sync-display         pace at the display's rate when it is within 0.5%% of the\n"
        "                         game's (60 Hz for a 59.94 Hz game: +0.1%% speed, no repeats)\n"
        "  --fps                  start with the on-screen frame-rate counter showing\n"
        "  --present-path P       vulkan (default) or dxgi (Windows: flip model, lower latency)\n"
        "  --present-mode M       vsync (default), mailbox or immediate\n"
        "  --cache-dir DIR        keep the pipeline cache there (default: beside the bindings)\n"
        "  --pipeline-keys FILE   make these pipeline variants at the start (hex keys, one a line)\n"
        "  --bindings FILE        controller bindings; the default is one file per user, shared\n"
        "                         by every title. F1 (or a pad's select button) opens the screen\n"
        "                         that edits them, and writes them back here.\n"
        "  --disc FILE            the disc image (.gdi, .cue or .chd) instead of the config's\n"
        "                         [disc] image. Without an extracted [binary] path the boot\n"
        "                         file is read off this disc and checked against\n"
        "                         [disc] sha1_1st_read.\n"
        "  --vmu FILE             a 128 KB memory-card image; writes are saved back to it.\n"
        "                         Created, blank and formatted, if the path does not exist. A file\n"
        "                         that is not a card is refused, never overwritten.\n"
        "  --mod DIR              serve DIR's files in place of the disc's (repeatable; the first\n"
        "                         --mod providing a file wins)\n"
        "  --rumble N             controller rumble strength, 0 (off) to 100 percent (default)\n"
        "  --volume N             master volume, 0 to 100 percent (default 100)\n"
        "  --soft-clip            round loud peaks off instead of the console's hard clipping\n"
        "  --cpu-speed K          charge guest code 1/K of the SH-4's cycle estimates (1-16;\n"
        "                         default 1, the console's timing)\n"
        "  --texture-pack DIR     replace textures with DIR/**/*_<hash>.png (with --window)\n"
        "  --dump-textures DIR    write each distinct texture once as DIR/<w>x<h>_<fmt>_<hash>.png\n"
        "  --flash FILE           a 128 KB console flash image; without it one is synthesised\n"
        "  --no-audio / --audio   force sound off, or on for a headless run\n"
        "\n"
        "In the window: arrow keys are the d-pad, Z X A S are A B X Y, return is start, Q and W\n"
        "are the triggers, F10 toggles the frame-rate counter, F12 writes a screenshot, escape\n"
        "quits.\n"
        "\n"
        "Repeatable runs:\n"
        "  --max-frames N         stop after N guest frames\n"
        "  --rtc-seed N           fix the console clock so two runs do the same thing\n"
        "  --press B@F[,B@F...]   hold a button for 8 frames from frame F (a b c x y z start\n"
        "                         up down left right)\n"
        "  --no-live-input        the window's keyboard and pads do not reach the game\n"
        "  --unthrottled          run as fast as the host can rather than at the guest's clock\n"
        "  --write-hash FILE      a rolling hash of every guest write, one line per frame\n"
        "  --keyon-trace FILE     every sound key-on: frame, sample, channel, address, pitch\n"
        "  --wav FILE             record the audio as 16-bit stereo 44.1 kHz\n"
        "  --screenshot-at N[,N]  write a screenshot at each listed presented frame (F12)\n"
        "  --screenshot-dir DIR   where screenshots go (default: the working directory)\n",
        argv0);
    for (auto* e : dream::host::extensions()) e->usage(out);
}

// Crash reports: defined at the end of this file, away from <windows.h>'s macros.
void install_crash_handler();
// Sleeps until `t` within a fraction of a millisecond (Windows: a high-resolution
// waitable timer, where std::this_thread::sleep_until wakes 1-3 ms late). Defined at the end.
void precise_sleep_until(std::chrono::steady_clock::time_point t);

int main(int argc, char** argv) {
    install_crash_handler();
    // Extensions may rewrite the command line before it is parsed (host_ext.h). The rewritten
    // strings must outlive every pointer taken into them below, hence the statics.
    static std::vector<std::string> ext_args;
    static std::vector<char*> ext_argv;
    if (!dream::host::extensions().empty()) {
        ext_args.assign(argv, argv + argc);
        for (auto* e : dream::host::extensions()) e->adjust_args(ext_args);
        for (auto& a : ext_args) ext_argv.push_back(a.data());
        ext_argv.push_back(nullptr);
        argc = static_cast<int>(ext_args.size());
        argv = ext_argv.data();
    }
    std::string config;
    // No limit unless one is asked for (--max-frames).
    std::uint64_t max_frames = 0;
    // Set by host_controls().quit when nothing else ends the run (no settings overlay with a
    // window to close): a port's own way out, such as a game's menu item. The run stops at the
    // next frame, as at a frame limit.
    bool quit_requested = false;
    std::string wav;  // --wav FILE: record the AICA output (16-bit stereo 44.1 kHz)
#ifdef DREAM_WITH_RENDERER
    // --present-mode: vsync paces the run to the panel. Nothing about the guest changes either way.
    auto present_mode = dream::render::vk::Window::PresentMode::Fifo;
    // --present-path vulkan|dxgi: dxgi presents through a DXGI flip-model swapchain on Windows, a
    // refresh sooner than some drivers' Vulkan swapchains where the machine has the headroom
    // (dxgi_present.h).
    auto present_path = dream::render::vk::Window::PresentPath::Auto;
#endif
    // --bindings FILE: where the controller layout is read from and written back to. Unset means
    // the host's per-user settings directory, so one layout follows the player across every title.
    std::string bindings_file;
    bool bindings_file_set = false;
    std::string flash_image;          // --flash FILE: a 128 KB console flash image. The runtime
                                      // synthesises one when this is absent, which is enough for
                                      // some titles and not others: a title that reads the user's
                                      // settings out of the flash user partition may refuse to
                                      // start on a synthetic one.
    const char* disc_override = nullptr;  // --disc FILE: the image, overriding [disc]
    float render_aspect = 0.0f;           // --render-aspect A: render target shape
    // --mod DIR (repeatable): file-replacement mod roots layered over the disc
    // (gdrom/mod_disc.h); the first root providing a file wins.
    std::vector<std::filesystem::path> mod_roots;
    int rumble_percent = 100;  // --rumble N: pad rumble strength, 0 = off
    int volume_percent = 100;  // --volume N: master output level
    bool soft_clip = false;    // --soft-clip: round loud peaks off instead of clipping
    bool fullscreen = false;              // --fullscreen; Alt+Enter toggles
    bool interpolate_frames = false;      // --interpolate: a blended frame between renders
    bool interpolate_auto = false;        // --interpolate-auto: only on displays above 60 Hz
    bool sync_display = false;            // --sync-display: pace at the display's rate
    std::string vmu;                  // --vmu FILE: a 128 KB memory-card image in the standard
                                      // layout, as any Dreamcast tool or emulator writes. Writes
                                      // go back to the file.
    // --press BUTTON@FRAME[,BUTTON@FRAME...]: hold a controller button for 8 frames from that
    // frame. Names: a b c x y z start up down left right.
    std::string presses;
    // --window: open a window and play. --scale N draws the geometry at N times the guest's
    // resolution; the guest never learns its screen changed size.
    bool window_mode = false, unthrottled = false;
    // --no-live-input: no physical controls reach the game, so a scripted run repeats exactly.
    bool no_live_input = false;
    // --fps: start with the on-screen counter showing. F10 toggles it either way.
    bool start_with_fps = false;
    std::set<std::uint64_t> screenshot_at;  // --screenshot-at N[,N...]
    std::string screenshot_dir;             // --screenshot-dir DIR
    unsigned scale = 1;
    // --texture-pack DIR / --dump-textures DIR: replacement packs and the dump that
    // authors them, both keyed by the content hash in render/texture_pack.h.
    std::string texture_pack, dump_textures;
    // --rtc-seed N: a fixed console clock, so two runs of the same build do the same thing. Any
    // non-zero value will do; the number itself only changes the date a title shows.
    std::uint64_t rtc_seed = 0;
    // --cpu-speed K: the charge for guest code, dream/runtime/sh4/cost.h.
    unsigned cpu_speed = 1;
    // --write-hash FILE: one line per frame with a rolling hash of every guest memory write. Two
    // runs that agree line for line did the same thing.
    std::string write_hash;
    // --keyon-trace FILE: one line per AICA key-on (frame, output sample, channel, sample address,
    // OCT, FNS): the music's note timeline.
    std::string keyon_trace;
    // --audio plays the AICA's output through the host's sound device. On by default with a window;
    // --no-audio turns it off.
    bool audio = false, no_audio = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--config") && i + 1 < argc)
            config = argv[++i];
        else if (!std::strcmp(argv[i], "--max-frames") && i + 1 < argc)
            max_frames = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc)
            wav = argv[++i];
#ifdef DREAM_WITH_RENDERER
        else if (!std::strcmp(argv[i], "--present-path") && i + 1 < argc) {
            const char* p = argv[++i];
            using PP = dream::render::vk::Window::PresentPath;
            present_path = !std::strcmp(p, "vulkan") ? PP::Vulkan
                           : !std::strcmp(p, "dxgi") ? PP::Dxgi
                                                     : PP::Auto;
        }
        else if (!std::strcmp(argv[i], "--present-mode") && i + 1 < argc) {
            const char* m = argv[++i];
            using PM = dream::render::vk::Window::PresentMode;
            if (!std::strcmp(m, "mailbox"))
                present_mode = PM::Mailbox;
            else if (!std::strcmp(m, "immediate"))
                present_mode = PM::Immediate;
            else if (!std::strcmp(m, "vsync") || !std::strcmp(m, "fifo"))
                present_mode = PM::Fifo;
            else {
                std::fprintf(stderr, "unknown present mode %s: vsync, mailbox or immediate\n", m);
                return 2;
            }
        }
#endif
        else if (!std::strcmp(argv[i], "--bindings") && i + 1 < argc) {
            bindings_file = argv[++i];
            bindings_file_set = true;
        } else if (!std::strcmp(argv[i], "--cache-dir") && i + 1 < argc) {
            g_cache_dir = argv[++i];
        } else if (!std::strcmp(argv[i], "--pipeline-keys") && i + 1 < argc) {
            g_pipeline_keys = argv[++i];
        } else if (!std::strcmp(argv[i], "--flash") && i + 1 < argc) {
            flash_image = argv[++i];
        } else if (!std::strcmp(argv[i], "--disc") && i + 1 < argc)
            disc_override = argv[++i];
        else if (!std::strcmp(argv[i], "--mod") && i + 1 < argc)
            mod_roots.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--rumble") && i + 1 < argc)
            rumble_percent = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (!std::strcmp(argv[i], "--volume") && i + 1 < argc)
            volume_percent = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (!std::strcmp(argv[i], "--soft-clip"))
            soft_clip = true;
        else if (!std::strcmp(argv[i], "--vmu") && i + 1 < argc)
            vmu = argv[++i];
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc)
            presses = argv[++i];
        else if (!std::strcmp(argv[i], "--window"))
            window_mode = true;
        else if (!std::strcmp(argv[i], "--unthrottled"))
            unthrottled = true;
        else if (!std::strcmp(argv[i], "--no-live-input"))
            no_live_input = true;
        else if (!std::strcmp(argv[i], "--fps"))
            start_with_fps = true;
        else if (!std::strcmp(argv[i], "--screenshot-dir") && i + 1 < argc)
            screenshot_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--screenshot-at") && i + 1 < argc)
            for (const char* p = argv[++i]; *p;) {
                char* end = nullptr;
                const std::uint64_t n = std::strtoull(p, &end, 0);
                if (end == p)
                    break;
                screenshot_at.insert(n);
                p = (*end == ',') ? end + 1 : end;
            }
        else if (!std::strcmp(argv[i], "--rtc-seed") && i + 1 < argc)
            rtc_seed = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--cpu-speed") && i + 1 < argc)
            cpu_speed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--write-hash") && i + 1 < argc)
            write_hash = argv[++i];
        else if (!std::strcmp(argv[i], "--keyon-trace") && i + 1 < argc)
            keyon_trace = argv[++i];
        else if (!std::strcmp(argv[i], "--audio"))
            audio = true;
        else if (!std::strcmp(argv[i], "--no-audio"))
            no_audio = true;
        else if (!std::strcmp(argv[i], "--texture-pack") && i + 1 < argc)
            texture_pack = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-textures") && i + 1 < argc)
            dump_textures = argv[++i];
        else if (!std::strcmp(argv[i], "--render-aspect") && i + 1 < argc)
            render_aspect = std::strtof(argv[++i], nullptr);
        else if (!std::strcmp(argv[i], "--fullscreen"))
            fullscreen = true;
        else if (!std::strcmp(argv[i], "--interpolate"))
            interpolate_frames = true;
        else if (!std::strcmp(argv[i], "--interpolate-auto"))
            interpolate_auto = true;
        else if (!std::strcmp(argv[i], "--sync-display"))
            sync_display = true;
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) {
            ++i;
            scale = !std::strcmp(argv[i], "panel") ? dream::host::HostControls::kScalePanel
                                                   : static_cast<unsigned>(std::strtoul(argv[i], nullptr, 0));
        }
        else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            usage(argv[0], stdout);
            return 0;
        } else {
            int used = 0;
            for (auto* e : dream::host::extensions())
                if ((used = e->parse_arg(i, argc, argv)) > 0)
                    break;
            if (used > 0) {
                i += used - 1;
                continue;
            }
            std::fprintf(stderr, "unknown option: %s\n\n", argv[i]);
            usage(argv[0], stderr);
            return 2;
        }
    }
    if (config.empty()) {
        std::fprintf(stderr, "--config is required\n");
        return 2;
    }
    if (scale != dream::host::HostControls::kScalePanel && (scale < 1 || scale > 8)) {
        std::fprintf(stderr, "--scale must be 1 to 8, or panel\n");
        return 2;
    }
#ifndef DREAM_WITH_RENDERER
    if (window_mode) {
        std::fprintf(stderr,
                     "this build has no renderer: configure with -DDREAM_RENDERER=ON and make sure "
                     "Vulkan, SDL3 and glslc are found\n");
        return 2;
    }
    (void)unthrottled;
    (void)rumble_percent;
    (void)volume_percent;
#endif
    dream::translator::GameConfig cfg;
    std::string err;
    if (!dream::translator::load_game_config(config, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::unique_ptr<dream::gdrom::Disc> disc;
    if (disc_override)
        cfg.disc_image = std::filesystem::path(disc_override);
    // A port that lists its disc images ([[disc.chd]]) runs from one of them and nothing else,
    // checked by the SHA-1 the CHD's header records, before the image is read.
    if (!cfg.chd_images.empty()) {
        std::string sha, why;
        if (cfg.disc_image)
            sha = dream::gdrom::chd_header_sha1(*cfg.disc_image, why);
        const dream::translator::ChdImage* match = nullptr;
        for (const auto& img : cfg.chd_images)
            if (!sha.empty() && img.sha1 == sha)
                match = &img;
        if (!match) {
            if (cfg.disc_image)
                std::fprintf(stderr, "disc: %s is not a supported image (%s).\n",
                             cfg.disc_image->string().c_str(),
                             sha.empty() ? why.c_str() : ("CHD SHA-1 " + sha).c_str());
            else
                std::fprintf(stderr, "disc: no disc image given (--disc).\n");
            std::fprintf(stderr, "%s runs only from these disc images:\n", cfg.title.c_str());
            for (const auto& img : cfg.chd_images)
                std::fprintf(stderr, "  %s.chd (SHA-1 %s)\n", img.name.empty() ? "?" : img.name.c_str(),
                             img.sha1.c_str());
            return 3;
        }
        std::printf("disc: supported image %s\n", match->name.empty() ? match->sha1.c_str() : match->name.c_str());
    }
    if (cfg.disc_image) {
        disc = dream::gdrom::open_disc(*cfg.disc_image, err);
        if (!disc) {
            std::fprintf(stderr, "disc: %s\n", err.c_str());
            return 2;
        }
        // Which image the run read, for its log.
        std::printf("disc: %s (%zu tracks, lead-out LBA %u)\n", disc->path().c_str(), disc->tracks().size(),
                    disc->leadout_lba());
    }
    std::vector<char> bytes;
    if (std::ifstream in{cfg.binary_path, std::ios::binary}) {
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else if (disc) {
        // no extracted copy beside the config, so take the boot file off the disc and
        // refuse anything but the exact build this executable was translated from.
        std::vector<std::uint8_t> boot;
        std::string boot_name;
        if (!dream::gdrom::read_root_file(*disc, "", boot, &boot_name)) {
            std::fprintf(stderr, "disc: no boot file in the image's filesystem\n");
            return 2;
        }
        const std::string sha = dream::sha1_hex(boot.data(), boot.size());
        if (!cfg.sha1_1st_read.empty() && sha != cfg.sha1_1st_read) {
            std::fprintf(stderr,
                         "disc: %s has SHA-1 %s, but this build was made from %s.\n"
                         "This is a different release or revision of the game; it cannot run "
                         "here.\n",
                         boot_name.c_str(), sha.c_str(), cfg.sha1_1st_read.c_str());
            return 3;
        }
        bytes.assign(boot.begin(), boot.end());
    } else {
        std::fprintf(stderr, "cannot read %s\n", cfg.binary_path.string().c_str());
        return 2;
    }
    // file-replacement mods. Applied after the boot file was read and verified, so
    // the executable always comes from the disc this build was translated from.
    if (!mod_roots.empty()) {
        if (!disc) {
            std::fprintf(stderr, "--mod needs a disc image (--disc or [disc] image)\n");
            return 2;
        }
        std::string mod_log;
        disc = dream::gdrom::ModDisc::wrap(std::move(disc), mod_roots, mod_log);
        std::printf("%s", mod_log.c_str());
        if (!mod_log.empty() && mod_log.back() != '\n')
            std::printf("\n");
    }

    dream::System sys;
    dream::hle::Bios bios(sys);
    bios.attach_disc(disc.get());
    sys.disc = disc.get();
    // Load the flash before setup_boot: Bios::install() formats a synthetic one only when the
    // factory string is not already present, so an image loaded here is used as it stands.
    if (!flash_image.empty()) {
        std::ifstream f(flash_image, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (raw.size() != dream::hle::Flash::kSize) {
            std::fprintf(stderr, "--flash %s: expected %u bytes, got %zu\n", flash_image.c_str(),
                         dream::hle::Flash::kSize, raw.size());
            return 2;
        }
        std::memcpy(sys.memory.flash(), raw.data(), raw.size());
        std::printf("flash: loaded %s\n", flash_image.c_str());
    }
    // The console's locale, before setup_boot() installs and may synthesise a flash. The disc's
    // region is the only evidence here of which console the title was sold for, and a title that
    // reads the factory partition should not be told it is running on a Japanese machine because
    // that happened to be the hardcoded default.
    const auto region = dream::hle::region_from_name(cfg.region);
    bios.set_locale(dream::hle::Language::English, region, dream::hle::broadcast_for(region));
    dream::sh4::set_cpu_speed(cpu_speed);
    std::printf("cpu: speed x%u (guest code charged 1/%u of the SH-4's cycle estimates)\n",
                dream::sh4::cpu_speed(), dream::sh4::cpu_speed());
    sys.install();
    bios.setup_boot(cfg.entry);
    std::memcpy(sys.memory.ram() + (cfg.load_address & (dream::mem::DcMemory::kRamSize - 1)),
                bytes.data(), bytes.size());
    // PowerVR core: register block and TA FIFO (headless: lists are parsed, nothing drawn).
    dream::pvr::Core pvr(sys.sched, sys.holly, sys.spg, sys.memory);
    sys.pvr_core = &pvr;
    sys.memory.map_mmio(dream::pvr::Core::kRegBase, dream::pvr::Core::kRegEnd, &pvr);
    sys.memory.map_mmio(dream::pvr::Core::kFifoBase, dream::pvr::Core::kFifoEnd, &pvr);
    // SH-4 DMAC and the Holly system block: channel-2 DMA is how Kamui submits display lists and
    // textures. The other system-bus DMA blocks (G1 at 0x005F7400, G2 at 0x005F7800, PVR DMA at
    // 0x005F7C00) are stored registers.
    dream::sh4::Dmac dmac(sys.memory);
    dream::holly::SysBlock sysblock(dmac, sys.holly, sys.memory, pvr);
    sys.memory.map_p4(dream::sh4::Dmac::kBase, dream::sh4::Dmac::kEnd, &dmac);
    sys.memory.map_mmio(dream::holly::SysBlock::kBase, dream::holly::SysBlock::kEnd, &sysblock);
    dream::holly::G2Dma g2(sys.sched, sys.holly, sys.memory);
    dream::mem::RegisterFile g1(0x005F7400u, 0x100 / 4), pvrdma(0x005F7C00u, 0x100 / 4),
        dma_triggers(0x005F6940u, 0x20 / 4);
    sys.memory.map_mmio(0x005F6940u, 0x005F6960u, &dma_triggers);  // SB_PDTNRM .. SB_G2DTEXT
    sys.memory.map_mmio(0x005F7400u, 0x005F7500u, &g1);
    sys.memory.map_mmio(dream::holly::G2Dma::kBase, dream::holly::G2Dma::kEnd, &g2);
    sys.memory.map_mmio(0x005F7C00u, 0x005F7D00u, &pvrdma);
    // On-chip modules the startup code programs but the runtime does not model yet: stored so the
    // writes neither fault nor abort (BSC, UBC, CPG/WDT, on-chip RTC), plus the serial port and
    // the cache address/data arrays, which cache-maintenance loops write.
    dream::mem::RegisterFile bsc(0xFF800000u, 0x50 / 4), ubc(0xFF200000u, 0x24 / 4),
        cpg(0xFFC00000u, 0x10 / 4), rtc(0xFFC80000u, 0x40 / 4);
    dream::mem::NullRegion cache_arrays;
    SerialPort scif;
    sys.memory.map_p4(0xF0000000u, 0xF8000000u, &cache_arrays);
    sys.memory.map_p4(0xFF800000u, 0xFF800050u, &bsc);
    sys.memory.map_p4(0xFF200000u, 0xFF200024u, &ubc);
    sys.memory.map_p4(0xFFC00000u, 0xFFC00010u, &cpg);
    sys.memory.map_p4(0xFFC80000u, 0xFFC80040u, &rtc);
    sys.memory.map_p4(0xFFE80000u, 0xFFE80028u, &scif);
    // Maple bus with a standard controller in port A (no buttons pressed); VBlank-triggered
    // transfers come from the SPG.
    dream::maple::Bus maple(sys.sched, sys.holly, sys.memory);
    sys.maple = &maple;
    auto pad = std::make_unique<dream::maple::Controller>();
    dream::maple::Controller* pad_ptr = pad.get();
    sys.pad_state[0] = &pad_ptr->state;
    // A memory card in the controller's first expansion slot, where the hardware puts one.
    if (!vmu.empty()) {
        auto card = std::make_unique<dream::maple::MemoryCard>();
        switch (card->load(vmu)) {
            case dream::maple::CardStatus::Ok:
                std::printf("memory card %s: %s\n", vmu.c_str(),
                            card->formatted() ? "formatted" : "present but not formatted");
                break;
            case dream::maple::CardStatus::Missing:
                // Made only when nothing is there, and formatted: titles do not all offer to format
                // a blank card, so an empty file would be no better than none.
                card->format();
                if (!card->save_as(vmu)) {
                    std::fprintf(stderr, "cannot create a memory card at %s\n", vmu.c_str());
                    return 2;
                }
                std::printf("memory card %s: created, formatted and empty\n", vmu.c_str());
                break;
            case dream::maple::CardStatus::WrongSize:
                // Never overwritten. save() rewrites the whole 128 KB after every block write, so
                // adopting a file that is not a card destroys it the moment the title saves.
                std::fprintf(stderr,
                             "%s is not a memory card image: a card is exactly %zu bytes.\n"
                             "Refusing to touch it. Point --vmu at a card, or at a path that does "
                             "not exist yet and one will be made.\n",
                             vmu.c_str(), dream::maple::MemoryCard::kImageSize);
                return 2;
            case dream::maple::CardStatus::Unreadable:
                std::fprintf(stderr, "cannot read the memory card image %s\n", vmu.c_str());
                return 2;
        }
        maple.attach_expansion(0, 0, std::move(card));
    }
    maple.attach(0, std::move(pad));
    // a vibration pack in every attached controller's slot 1, reported to the window
    // as gamepad rumble for that player. `rumble_fn` stays empty headless.
    std::function<void(unsigned, float, unsigned)> rumble_fn;
    sys.rumble = [&rumble_fn](unsigned port, float strength, unsigned ms) {
        if (rumble_fn)
            rumble_fn(port, strength, ms);
    };
    auto make_pack = [&sys](unsigned port) {
        auto pack = std::make_unique<dream::maple::VibrationPack>();
        pack->on_vibrate = [&sys, port](float strength, unsigned ms) {
            if (sys.rumble)
                sys.rumble(port, strength, ms);
        };
        return pack;
    };
    maple.attach_expansion(0, 1, make_pack(0));
    // Players 2-4: a controller (and pack) on ports B-D exists only while a pad drives that
    // player, so a game sees the second controller appear when it is plugged in.
    std::array<dream::maple::Controller*, dream::maple::Bus::kPorts> port_pads{pad_ptr};
    auto sync_ports = [&](unsigned players) {
        for (unsigned port = 1; port < dream::maple::Bus::kPorts; ++port) {
            const bool want = port < players;
            if (want == (port_pads[port] != nullptr))
                continue;
            if (want) {
                auto c = std::make_unique<dream::maple::Controller>();
                port_pads[port] = c.get();
                maple.attach(port, std::move(c));
                maple.attach_expansion(port, 1, make_pack(port));
            } else {
                port_pads[port] = nullptr;
                maple.attach(port, nullptr);
                maple.attach_expansion(port, 1, nullptr);
            }
            std::printf("maple: controller %s on port %c\n", want ? "connected" : "removed",
                        static_cast<char>('A' + port));
        }
    };
    sys.memory.map_mmio(dream::maple::Bus::kBase, dream::maple::Bus::kEnd, &maple);
    // Scripted controller presses (--press start@120,a@300): each is held for 8 frames.
    std::vector<std::pair<std::uint64_t, std::uint16_t>> scripted;
    {
        static const struct {
            const char* name;
            std::uint16_t bit;
        } kNames[] = {{"c", dream::maple::kC},       {"b", dream::maple::kB},
                      {"a", dream::maple::kA},       {"start", dream::maple::kStart},
                      {"up", dream::maple::kUp},     {"down", dream::maple::kDown},
                      {"left", dream::maple::kLeft}, {"right", dream::maple::kRight},
                      {"z", dream::maple::kZ},       {"y", dream::maple::kY},
                      {"x", dream::maple::kX}};
        std::string rest = presses;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            std::string one = rest.substr(0, comma);
            rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
            const std::size_t at = one.find('@');
            if (at == std::string::npos)
                continue;
            const std::string name = one.substr(0, at);
            const std::uint64_t frame = std::strtoull(one.c_str() + at + 1, nullptr, 0);
            for (const auto& n : kNames)
                if (name == n.name)
                    scripted.push_back({frame, n.bit});
        }
    }
    sys.spg.on_vblank_out = [&] {
        for (auto* e : dream::host::extensions()) e->on_vblank(sys);
        if (!scripted.empty()) {
            const std::uint64_t f = sys.spg.frames();
            std::uint16_t held = 0;
            for (const auto& [start, bit] : scripted)
                if (f >= start && f < start + 8)
                    held |= bit;
            pad_ptr->state.buttons = static_cast<std::uint16_t>(0xFFFFu & ~held);
        }
        maple.vblank();
    };
    // A port can end the run (a game's exit item, a NextUI build's start screen), in every build,
    // with or without a renderer: the watchdog stops at its next tick.
    dream::host::host_controls().quit = [&quit_requested] { quit_requested = true; };
#ifdef DREAM_WITH_RENDERER
    // --window: the renderer, the offscreen target and the window, hung off the render and vblank
    // hooks. The scripted presses already run at vblank, so that hook is chained.
    std::unique_ptr<Live> live;
    if (window_mode) {
        live = std::make_unique<Live>(sys.memory, pvr);
        live->interpolate = interpolate_frames || interpolate_auto;
        live->interpolate_only_above_60 = interpolate_auto && !interpolate_frames;
        live->pace_interpolation = !unthrottled;
        live->sync.enabled = sync_display;
        // A frame is copied to the host only when something reads it (a screenshot).
        live->offscreen.readback = false;
        live->show_fps = start_with_fps;
        live->window.set_present_mode(present_mode);
        live->window.set_present_path(present_path);
        live->screenshot_at = screenshot_at;
        live->screenshot_dir = screenshot_dir;
        // The window names the title being run. The config is the only place that name is written
        // down for a human, and a config without one still gets a window with a name on it.
        if (!live->start(
                scale, cfg.title.empty() ? std::string("Rez Recompiled") : cfg.title,
                render_aspect, fullscreen)) {
            std::fprintf(stderr, "window: %s\n", live->error.c_str());
            return 2;
        }
        auto& replacer = live->renderer.textures().replacer();
        if (!texture_pack.empty()) {
            std::string pack_err;
            const auto t0 = std::chrono::steady_clock::now();
            if (replacer.open_pack(texture_pack, &pack_err))
                std::printf("texture pack: %s, %zu textures (indexed in %.1f ms)\n",
                            texture_pack.c_str(), replacer.pack_size(),
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
            else
                std::fprintf(stderr, "texture pack: %s\n", pack_err.c_str());
        }
        if (!dump_textures.empty()) {
            std::string dump_err;
            if (replacer.set_dump_dir(dump_textures, &dump_err))
                std::printf("texture dump: writing new textures to %s\n", dump_textures.c_str());
            else
                std::fprintf(stderr, "texture dump: %s\n", dump_err.c_str());
        }
        // After the window, because resolving a binding's name to a code needs SDL initialised.
        live->bindings_path =
            bindings_file_set ? bindings_file : dream::render::vk::default_bindings_path();
        live->load_bindings();
        live->window.rumble_scale = static_cast<float>(rumble_percent) / 100.0f;
        // an extension's in-game overlay takes Escape and the pad's Back button, sees
        // events first, and may ask the launcher to quit or open the binding screen.
        for (auto* e : dream::host::extensions())
            if (e->wants_overlay())
                live->overlays.push_back(e);
        // The resolution at run time, whoever asks: an overlay's menu, or a port's own settings
        // page (on NextUI, where there is no overlay).
        {
            Live* lp = live.get();
            auto& hc = dream::host::host_controls();
            hc.set_render_scale = [lp](unsigned sc) {
                lp->want_scale = sc == dream::host::HostControls::kScalePanel ? sc : std::clamp(sc, 1u, 8u);
            };
            hc.render_size = [lp](unsigned sc, unsigned& w, unsigned& h) {
                std::uint32_t tw = 0, th = 0;
                lp->target_size(sc, lp->render_aspect_, tw, th);
                w = tw, h = th;
            };
        }
        if (!live->overlays.empty()) {
            live->window.set_escape_quits(false);
            live->window.set_pad_menu_button(false);
            Live* lp = live.get();
            live->window.event_filter = [lp](const void* ev) {
                if (lp->menu.is_open())
                    return false;  // the binding screen keeps its own keys
                for (auto* e : lp->overlays)
                    if (e->on_event(ev))
                        return true;
                return false;
            };
            auto& hc = dream::host::host_controls();
            hc.quit = [lp] { lp->window.request_close(); };
            hc.open_bindings = [lp] {
                if (!lp->menu.is_open())
                    lp->open_menu();
            };
            hc.set_rumble = [lp](float v) { lp->window.rumble_scale = std::clamp(v, 0.0f, 1.0f); };
            hc.set_fullscreen = [lp](bool on) { lp->window.set_fullscreen(on); };
            hc.fullscreen = [lp] { return lp->window.fullscreen(); };
            hc.window_aspect = [lp] {
                const VkExtent2D e = lp->window.extent();
                return e.height ? static_cast<float>(e.width) / static_cast<float>(e.height) : 0.0f;
            };
            hc.set_render_aspect = [lp](float a) { lp->want_render_aspect = a; };
            hc.bindings_path = [lp] { return lp->bindings_path; };
            hc.reload_bindings = [lp] { lp->load_bindings(); };
            hc.set_volume = [&volume_percent](float v) {
                volume_percent = static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 100.0f + 0.5f);
            };
            hc.set_soft_clip = [&soft_clip](bool on) { soft_clip = on; };
        }
        if (rumble_percent != 100)
            std::printf("rumble: %d%%\n", rumble_percent);
        rumble_fn = [&live](unsigned port, float strength, unsigned ms) {
            live->window.rumble(port, strength, ms);
        };
        if (live->overlays.empty())
            std::printf("F1 (or a pad's select button) opens the controller bindings\n");
        else
            std::printf("Escape or a pad's select button opens the menu\n");
        pvr.on_render = [&live](const dream::pvr::RenderWords& words) { live->render(words); };
        pvr.on_lists_begin = [&live] { live->lists_begin(); };  // borrowed words (Stream::borrow)
        // The render thread (Live::start_render_thread) takes the fast path's Vulkan work off the
        // guest's thread. Off with interpolation, which draws its in-between frames inline.
        if (!live->interpolate)
            live->start_render_thread();
        // Real time, so the title runs at the speed it was written for. The guest clock is the
        // reference: sleep only while ahead of it, never speed anything up to catch up, because a
        // frame that took too long is gone and pretending otherwise makes the audio stutter.
        // The pacing clock: the wall-clock time the current guest frame is due,
        // advanced by each guest frame's length (divided by the display-sync factor). Held by the
        // callback itself: this block ends before the first vblank (a reference to a local here
        // once read a dead stack slot and pacing never slept). It restarts from now after a pause
        // or a long stall, so the run never sprints to catch up.
        auto previous_vblank = std::move(sys.spg.on_vblank_out);
        sys.spg.on_vblank_out = [&, previous_vblank,
                                 due = std::chrono::steady_clock::time_point{}]() mutable {
            if (previous_vblank)
                previous_vblank();
            const auto vblank_at = std::chrono::steady_clock::now();
            auto& sync = live->sync;
            if (live->pads_read_at.time_since_epoch().count() != 0)
                sync.note_emulation(
                    std::chrono::duration<float, std::milli>(vblank_at - live->pads_read_at).count());
            const double frame_s = sync.note_guest_frame(sys.ctx.cycles);
            // Early: hold the frame until its moment, so presents keep one per refresh at a fixed
            // point before the vblank (a frame presented early would otherwise replace the one
            // before it while that still waits for its flip).
            if (sync.target.time_since_epoch().count() != 0 && vblank_at <= sync.target - sync.margin)
                precise_sleep_until(sync.target - sync.margin);
            if (!live->present(sys.spg.frames(), sys.ctx.cycles))
                throw StopRun{"the window was closed"};
            // The binding screen stops the guest rather than drawing over a running one: nobody
            // rebinds a control mid-corner, and a control being captured cannot also be played.
            // The window keeps presenting, so the screen still draws and still answers the player.
            if (live->menu.is_open()) {
                while (live->menu.is_open()) {
                    if (!live->present(sys.spg.frames(), sys.ctx.cycles))
                        throw StopRun{"the window was closed"};
                    // Enough to keep the screen responsive without spinning a core on a still
                    // picture; the guest clock is not advancing, so there is nothing to keep up
                    // with.
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                }
                due = {};  // start the clock again from now
                sync.target = {};
            }
            // An extension's pause (the HUD editor holds a HUD on screen while it is
            // edited). The last display list is drawn again each pass, so changes an extension
            // makes in on_frame() show on the still picture.
            auto ext_paused = [] {
                for (auto* e : dream::host::extensions())
                    if (e->guest_paused())
                        return true;
                return false;
            };
            if (ext_paused()) {
                std::printf("paused by an extension at frame %llu\n",
                            static_cast<unsigned long long>(sys.spg.frames()));
                while (ext_paused()) {
                    for (auto* e : dream::host::extensions()) e->on_paused_tick();
                    if (live->last_stream && !live->last_stream->empty()) {
                        const dream::pvr::RenderWords words = live->last_stream;
                        live->render(words);
                    }
                    if (!live->present(sys.spg.frames(), sys.ctx.cycles))
                        throw StopRun{"the window was closed"};
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                std::printf("resumed\n");
                due = {};
                sync.target = {};
            }
            // Pads are read after the pacing sleep, right before the guest runs the
            // frame that uses them: read before the sleep, the input was a sleep older (about
            // 10 ms at scale 2) by the time the game saw it.
            auto read_pads = [&] {
                if (!live->poll_input())
                    throw StopRun{"the window was closed"};
                // Scripted presses win while they are held, so --press still works with a window
                // open.
                if (!no_live_input && (scripted.empty() || pad_ptr->state.buttons == 0xFFFFu))
                    live->read_controls(pad_ptr->state, 0);
                // A scripted run keeps one player whatever pads are plugged in.
                sync_ports(no_live_input ? 1u : live->window.players());
                for (unsigned port = 1; port < port_pads.size(); ++port)
                    if (port_pads[port] && !no_live_input)
                        live->read_controls(port_pads[port]->state, port);
                live->pads_read_at = std::chrono::steady_clock::now();
            };
            if (unthrottled) {
                read_pads();
                return;
            }
            // Display sync: the clock's rate.
            sync.factor = 1.0;
            bool phased = false;
            if (sync.enabled && !live->interpolate && sync.guest_frame_s > 0) {
                std::chrono::steady_clock::time_point vb;
                std::chrono::nanoseconds period{};
                if (live->window.vblank_grid(vb, period) && period.count() > 0) {
                    sync.display_hz = 1e9 / static_cast<double>(period.count());
                    const double guest_hz = 1.0 / sync.guest_frame_s;
                    if (std::abs(sync.display_hz - guest_hz) / guest_hz < 0.005) {
                        sync.factor = sync.display_hz * (1.0 - 1e-4) / guest_hz;
                        // Only while one frame waits: deeper (frames arriving late, e.g. on
                        // battery) the slot grid never makes up a late frame's time, while the
                        // rate clock below catches up, and the phase buys no latency at that depth.
                        if (live->window.flips() && live->window.flip_queue_depth() == 1) {
                            // The next vblank on the grid after the last target; started afresh
                            // (or after falling a whole refresh behind) from the first vblank a
                            // frame starting now could make.
                            const auto now = std::chrono::steady_clock::now();
                            const auto per = std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                            auto snap = [&](std::chrono::steady_clock::time_point t) {
                                const double k = std::round(static_cast<double>((t - vb).count()) /
                                                            static_cast<double>(per.count()));
                                return vb + per * static_cast<std::chrono::steady_clock::rep>(k);
                            };
                            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                sync.budget(period));
                            const auto lead = budget + sync.margin;
                            auto next = sync.target.time_since_epoch().count() == 0
                                            ? sync.target
                                            : snap(sync.target + per);
                            if (next.time_since_epoch().count() == 0 || next - lead < now - per)
                                next = snap(now + lead + per / 2);
                            sync.target = next;
                            const auto wake = next - lead;
                            if (wake > now)
                                precise_sleep_until(wake);
                            due = std::chrono::steady_clock::now();  // the rate clock follows
                            phased = true;
                        }
                    }
                }
            }
            if (!phased)
                sync.target = {};
            const auto now = std::chrono::steady_clock::now();
            const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(frame_s / sync.factor));
            // A stall of more than a few frames (loading, the window dragged) is not caught up:
            // the frames that took too long are gone, and sprinting would only stutter the audio.
            if (!phased) {
                if (due.time_since_epoch().count() == 0 ||
                    now - (due + step) > std::chrono::milliseconds(50))
                    due = now;
                else
                    due += step;
                if (due > now)
                    std::this_thread::sleep_until(due);
            }
            read_pads();
        };
    }
#endif
    // The AICA: the sound chip's registers, timers and mixer.
    dream::aica::Aica aica(sys.sched, sys.holly, sys.memory);
    aica.mixer.soft_clip = &soft_clip;
    FILE* keyon_file = nullptr;
    if (!keyon_trace.empty()) {
        keyon_file = std::fopen(keyon_trace.c_str(), "w");
        if (!keyon_file) {
            std::fprintf(stderr, "cannot write %s\n", keyon_trace.c_str());
            return 2;
        }
        std::fprintf(keyon_file, "# frame sample channel sa oct fns\n");
        // The output position is the samples already produced: the mixer's count while the ARM7
        // runs inside a tick, the tick's index less one when a port's source makes the sample (the
        // source keys voices on before it mixes), so both modes count the same way.
        aica.mixer.on_key_on = [&](unsigned channel, std::uint32_t sa, unsigned oct, unsigned fns) {
            const std::uint64_t at = aica.source ? aica.samples - 1 : aica.mixer.samples;
            std::fprintf(keyon_file, "%llu %llu %u %06x %x %03x\n",
                         static_cast<unsigned long long>(sys.spg.frames()), static_cast<unsigned long long>(at),
                         channel, sa, oct, fns);
        };
    }
    sys.memory.map_mmio(dream::aica::Aica::kRegBase, dream::aica::Aica::kRegEnd, &aica);
    sys.aica = &aica;
    std::vector<std::int16_t> pcm;  // interleaved L/R
#ifdef DREAM_WITH_AUDIO
    // Playback follows the window by default: a run with a picture should have sound, and a
    // headless one would otherwise be paced by a device consuming at its own rate.
    dream::audio::Sink sink;
    const bool want_audio = !no_audio && (audio || window_mode);
    if (want_audio) {
        if (sink.open())
            std::printf("audio: playing through the default device\n");
        else
            std::fprintf(stderr, "audio: %s\n", sink.error().c_str());
    }
#else
    // Asking for sound from a build that cannot make any should say so, but it is not worth
    // refusing the run over: --window implies audio, and a build without SDL3 should still show a
    // picture. --wav still records what the mixer produced.
    if (audio && !no_audio)
        std::fprintf(stderr,
                     "audio: this build has no playback (SDL3 was not found); --wav still records "
                     "the mixer's output\n");
    (void)no_audio;
#endif
    aica.on_sample = [&](std::int16_t l, std::int16_t r) {
#ifdef DREAM_WITH_AUDIO
        if (volume_percent != 100) {  // --volume; --wav keeps the mixer's own level
            sink.push(static_cast<std::int16_t>(l * volume_percent / 100),
                      static_cast<std::int16_t>(r * volume_percent / 100));
        } else {
            sink.push(l, r);
        }
#endif
        if (!wav.empty() && pcm.size() < 44100u * 2u * 600u) {  // cap at ten minutes
            pcm.push_back(l);
            pcm.push_back(r);
        }
    };
    // The console's clock: the host date at start (or --rtc-seed's, for runs that must repeat),
    // then one tick per guest second.
    dream::aica::Rtc aica_rtc((rtc_seed ? static_cast<std::uint32_t>(rtc_seed)
                                        : static_cast<std::uint32_t>(std::time(nullptr))) +
                              dream::aica::Rtc::kUnixEpochOffset);
    sys.memory.map_mmio(dream::aica::Rtc::kBase, dream::aica::Rtc::kEnd, &aica_rtc);
    sys.rtc = &aica_rtc;
    int rtc_tick = -1;
    rtc_tick = sys.sched.add("rtc", [&](std::uint64_t, std::uint64_t) {
        aica_rtc.tick();
        sys.sched.request(rtc_tick, 200'000'000ull);
    });
    sys.sched.request(rtc_tick, 200'000'000ull);
    // The frame limit and a port's quit, checked once per scanline.
    int tick = -1;
    const std::uint64_t period = sys.spg.line_cycles();
    // --write-hash: one line per frame, so two runs can be lined up against each other.
    FILE* hash_file = nullptr;
    std::uint64_t hashed_frame = ~0ull;
    if (!write_hash.empty()) {
        hash_file = std::fopen(write_hash.c_str(), "w");
        if (!hash_file) {
            std::fprintf(stderr, "cannot write %s\n", write_hash.c_str());
            return 2;
        }
        sys.memory.hash_writes = true;
        std::fprintf(hash_file, "# frame cycles hash writes\n");
    }
    tick = sys.sched.add("watchdog", [&](std::uint64_t now, std::uint64_t) {
        if (hash_file && sys.spg.frames() != hashed_frame) {
            hashed_frame = sys.spg.frames();
            std::fprintf(hash_file, "%llu %llu %016llx %llu\n",
                         static_cast<unsigned long long>(hashed_frame),
                         static_cast<unsigned long long>(now),
                         static_cast<unsigned long long>(sys.memory.write_hash),
                         static_cast<unsigned long long>(sys.memory.writes_hashed));
        }
        if (quit_requested)
            throw StopRun{"quit by the game"};
        if (max_frames && sys.spg.frames() >= max_frames)
            throw StopRun{"frame limit"};
        sys.sched.request(tick, period);
    });
    // Nothing here changes what the guest sees, so an idle skip may jump past it.
    sys.sched.set_host_only(tick);
    sys.sched.request(tick, period);
    dream::sh4::GuestFn entry = dream::sh4::find_function(cfg.entry);
    if (!entry) {
        std::fprintf(stderr, "no translated function at entry 0x%08x\n", cfg.entry);
        return 2;
    }
    const char* stop = "returned from the entry function";
    int rc = 0;
    sys.memory.refresh_fast_path();  // after the write hash flag is final
    for (auto* e : dream::host::extensions()) e->on_start(sys);
    auto run_title = [&] {
        try {
            dream::sh4::run_guest(sys.ctx, sys.guest_memory ? *sys.guest_memory : sys.memory, cfg.entry);
        } catch (const StopRun& s) {
            stop = s.why;
        } catch (const std::exception& e) {
            stop = "fault";
            std::fprintf(stderr, "fault: %s\n", e.what());
            rc = 1;
        }
    };
#ifdef DREAM_WITH_RENDERER
    // With a window, the guest runs on its own thread and this one only pumps window
    // events, so moving or resizing the window (a modal loop on Windows) does not stop the game.
    if (live) {
        live->window.set_threaded(true);
        std::atomic<bool> guest_done{false};
        std::thread guest([&] {
            run_title();
            guest_done = true;
        });
        while (!guest_done.load()) live->window.pump_events(5);
        guest.join();
        live->window.set_threaded(false);
    } else
#endif
        run_title();
#ifdef DREAM_WITH_RENDERER
    if (live)
        live->stop_render_thread();  // what is queued is drawn first
#endif
    for (auto* e : dream::host::extensions()) e->on_stop(sys, stop);
#ifdef DREAM_WITH_AUDIO
    if (sink.is_open())
        sink.flush();
#endif
    if (hash_file) {
        std::fprintf(hash_file, "%llu %llu %016llx %llu\n",
                     static_cast<unsigned long long>(sys.spg.frames()),
                     static_cast<unsigned long long>(sys.ctx.cycles),
                     static_cast<unsigned long long>(sys.memory.write_hash),
                     static_cast<unsigned long long>(sys.memory.writes_hashed));
        std::fclose(hash_file);
    }
    if (keyon_file) {
        aica.mixer.on_key_on = nullptr;
        std::fclose(keyon_file);
    }
#ifdef DREAM_WITH_RENDERER
    // Vulkan objects are destroyed now rather than at scope exit, while everything they were
    // created from is still alive.
    live.reset();
#endif
    if (!wav.empty()) {
        if (FILE* f = std::fopen(wav.c_str(), "wb")) {
            const std::uint32_t data_bytes = static_cast<std::uint32_t>(pcm.size() * 2);
            auto put32 = [&](std::uint32_t v) {
                const unsigned char b[4] = {
                    static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                    static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
                std::fwrite(b, 1, 4, f);
            };
            auto put16 = [&](std::uint32_t v) {
                const unsigned char b[2] = {static_cast<unsigned char>(v),
                                            static_cast<unsigned char>(v >> 8)};
                std::fwrite(b, 1, 2, f);
            };
            std::fwrite("RIFF", 1, 4, f);
            put32(36 + data_bytes);
            std::fwrite("WAVEfmt ", 1, 8, f);
            put32(16);
            put16(1);      // PCM
            put16(2);      // stereo
            put32(44100);  // rate
            put32(44100 * 4);
            put16(4);
            put16(16);
            std::fwrite("data", 1, 4, f);
            put32(data_bytes);
            for (std::int16_t v : pcm) put16(static_cast<std::uint16_t>(v));
            std::fclose(f);
        }
    }
    std::printf("stop: %s at guest frame %llu\n", stop, static_cast<unsigned long long>(sys.spg.frames()));
    return rc;
}

// ---- crash reports ------------------------------------------------------------------
// stdout is unbuffered, so what was printed before a crash is in the log after it. On Windows an
// unhandled exception on any thread prints what, where (module and offset, for the map file) and on
// which thread, and writes a minidump beside the bindings file
// (%APPDATA%/rez_recompiled/crash-<time>.dmp) for a debugger.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

#pragma pack(push, 4)  // as dbghelp.h declares MINIDUMP_EXCEPTION_INFORMATION
struct MiniDumpExceptionInfo {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};
#pragma pack(pop)
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, MiniDumpExceptionInfo*,
                                          void*, void*);

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    void* addr = r->ExceptionAddress;
    char module[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    std::uintptr_t offset = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, module, sizeof module);
        offset = reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(mod);
    }
    std::fprintf(stderr,
                 "\ncrash: exception 0x%08lX at %p (%s + 0x%llx) on thread %lu",
                 static_cast<unsigned long>(r->ExceptionCode), addr, module,
                 static_cast<unsigned long long>(offset),
                 static_cast<unsigned long>(GetCurrentThreadId()));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        std::fprintf(stderr, ", %s of %p", r->ExceptionInformation[0] == 1 ? "write" : "read",
                     reinterpret_cast<void*>(r->ExceptionInformation[1]));
    std::fprintf(stderr, "\n");
    // The dump: where the bindings live, which every port has and the player can find.
    std::string dir = dream::render::vk::default_bindings_path();
    const auto slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    char name[64];
    std::snprintf(name, sizeof name, "crash-%llu.dmp",
                  static_cast<unsigned long long>(std::time(nullptr)));
    const std::string path = dir + name;
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        auto write = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write && file != INVALID_HANDLE_VALUE) {
            MiniDumpExceptionInfo info{GetCurrentThreadId(), ep, FALSE};
            // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo: small, with stacks.
            if (write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x40 | 0x1000, &info,
                      nullptr, nullptr))
                std::fprintf(stderr, "crash: minidump written to %s\n", path.c_str());
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
    std::fflush(stderr);
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void install_crash_handler() {
    // Unbuffered: what was printed before a crash is in the log after it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(on_crash);
}

__declspec(noinline) void precise_sleep_until(std::chrono::steady_clock::time_point t) {
    // One high-resolution timer per thread (Windows 10 1803+); without one, plain sleep.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto now = std::chrono::steady_clock::now();
    if (t <= now)
        return;
    if (!timer) {
        std::this_thread::sleep_until(t);
        return;
    }
    // Relative due time in 100 ns units (negative = relative).
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(t - now).count() / 100);
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
        WaitForSingleObject(timer, INFINITE);
    else
        std::this_thread::sleep_until(t);
}
#else
void install_crash_handler() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
}
void precise_sleep_until(std::chrono::steady_clock::time_point t) { std::this_thread::sleep_until(t); }
#endif
