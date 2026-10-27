// Launcher extensions: code linked into a title's executable can add
// command-line flags and run at fixed points of a run without editing the launcher.
//
// An extension is any object that registers itself during static initialisation:
//
//     struct Widescreen final : dream::host::Extension { ... };
//     static Widescreen ws;
//     static dream::host::Register reg_ws(ws);
//
// Points, in run order: parse_arg (each argv entry the launcher does not know), on_start (RAM
// loaded, devices installed, before the first guest instruction), on_vblank (every vblank-out, in
// the guest thread, before the frame is presented), on_stop (after the guest stopped, before the
// report). All run on the guest thread; none may block.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace dream {
class System;
}
namespace dream::render {
struct Frame;
}

namespace dream::host {

class Extension {
public:
    virtual ~Extension() = default;
    virtual const char* name() const = 0;
    // Before anything is parsed: the whole command line (args[0] is the program) may be edited,
    // e.g. to turn a bare double-click into a configured windowed run from saved settings.
    virtual void adjust_args(std::vector<std::string>& args) { (void)args; }
    // argv[i] is a flag the launcher did not recognise. Return how many argv entries this
    // extension consumed (0: not mine).
    virtual int parse_arg(int i, int argc, char** argv) {
        (void)i, (void)argc, (void)argv;
        return 0;
    }
    virtual void usage(std::FILE* out) { (void)out; }
    virtual void on_start(System& sys) { (void)sys; }
    virtual void on_vblank(System& sys) { (void)sys; }
    // Each decoded display list, before it is drawn (window builds only). Vertices are in the
    // guest's 640x480 screen space and may be edited: widescreen HUD correction, debug overlays.
    virtual void on_frame(render::Frame& frame) { (void)frame; }
    // With the launcher's render thread, a display list can be decoded there and its frame handed
    // to on_frame_offthread() instead of on_frame(), when every extension's frame_offthread_ok()
    // is true (asked on the guest's thread at each render). Answer false while the frame work
    // reads state the guest's thread changes.
    virtual bool frame_offthread_ok() { return true; }
    virtual void on_frame_offthread(render::Frame& frame) { on_frame(frame); }
    virtual void on_stop(System& sys, const char* why) { (void)sys, (void)why; }

    // --- in-game overlay (window builds) -------------------------------------------------------
    // An extension that draws a menu over the game returns true from wants_overlay(). The
    // launcher then stops Escape from quitting and the pad's Back button from opening the binding
    // screen (F1 still does), and offers every SDL event to on_event() first (`sdl_event` is a
    // `const SDL_Event*`; return true to consume it). While overlay_open() is true the guest keeps
    // running but its pads read as released, and each presented frame goes through
    // draw_overlay(): `rgba` is the frame (RGBA, red in the low byte, `w` x `h`, row 0 at the
    // top), `view_w` x `view_h` the window it is shown in. on_event() runs inside the window's
    // event poll, draw_overlay() inside the present: an extension must not assume both are on the
    // guest thread.
    virtual bool wants_overlay() { return false; }
    virtual bool on_event(const void* sdl_event) {
        (void)sdl_event;
        return false;
    }
    virtual bool overlay_open() { return false; }
    // true holds the guest at its next vblank until it turns false again -- the clock,
    // sound and game logic stop. Meanwhile the last display list is drawn again every pass (so
    // on_frame() changes show on the still picture), the window keeps presenting, and
    // on_paused_tick() runs on the guest thread for work that would otherwise wait for a vblank.
    virtual bool guest_paused() { return false; }
    virtual void on_paused_tick() {}
    // The overlay as an image of the window's size (premultiplied RGBA, red in the low byte),
    // blended over the game on the GPU: the game frame never has to come back to the CPU.
    // `changed` says whether it differs from the last call, so it is uploaded only then.
    // Returning null falls back to draw_overlay().
    virtual const std::uint32_t* overlay_image(unsigned view_w, unsigned view_h, bool& changed) {
        (void)view_w, (void)view_h;
        changed = false;
        return nullptr;
    }
    virtual void draw_overlay(std::uint32_t* rgba, unsigned w, unsigned h, unsigned view_w,
                              unsigned view_h) {
        (void)rgba, (void)w, (void)h, (void)view_w, (void)view_h;
    }
};

// What an overlay may ask of the running launcher. Set by the launcher once its window is open;
// empty otherwise (headless), so callers check before calling.
struct HostControls {
    std::function<void()> quit;                  // end the run, as closing the window does
    std::function<void()> open_bindings;         // the controller binding screen (F1)
    std::function<void(float)> set_rumble;       // 0..1
    std::function<void(bool)> set_fullscreen;
    std::function<bool()> fullscreen;
    std::function<void(float)> set_volume;       // 0..1, master output level
    std::function<void(bool)> set_soft_clip;     // the AICA output's soft limiter (enhancement)
    // Live aspect: the window's width / height in pixels, and a new shape for the
    // render target (the widened view of an anamorphic title; 4/3 or less is the original). The
    // target is rebuilt at the next vblank.
    std::function<float()> window_aspect;
    std::function<void(float)> set_render_aspect;
    // A new resolution multiple (1-8) for the render target, rebuilt at the next vblank like
    // set_render_aspect (Resolution changes without a restart). kScalePanel: the
    // display's own resolution, the largest 4:3 picture its pixels hold.
    static constexpr unsigned kScalePanel = 0;
    std::function<void(unsigned)> set_render_scale;
    // The render target's size for a scale (kScalePanel included) at the current shape.
    std::function<void(unsigned scale, unsigned& w, unsigned& h)> render_size;
    // The controller bindings file: an overlay that edits bindings writes this file,
    // then calls reload_bindings so the game uses them at once.
    std::function<std::string()> bindings_path;
    std::function<void()> reload_bindings;
};

inline HostControls& host_controls() {
    static HostControls c;
    return c;
}

inline std::vector<Extension*>& extensions() {
    static std::vector<Extension*> list;
    return list;
}

struct Register {
    explicit Register(Extension& e) { extensions().push_back(&e); }
};

}  // namespace dream::host
