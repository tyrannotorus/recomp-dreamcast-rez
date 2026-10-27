// SDL3 window and Vulkan swapchain. Presentation only: what to draw comes from the
// renderer.
#pragma once

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dream/render/input.h"
#include "dream/render/vk/context.h"
#include "dream/render/vk/dxgi_present.h"
#include "dream/render/vk/fb0_present.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

struct SDL_Window;
struct SDL_Gamepad;

namespace dream::render::vk {

// The controls a Dreamcast pad offers, as the window sees them. SDL's key codes stop here: the
// launcher maps these onto the Maple controller, so the keyboard layout lives in one place and the
// runtime never learns what a keyboard is.
//
//   arrow keys   d-pad          Z X A S   the A, B, X and Y buttons
//   return       start          Q W       the left and right analogue triggers
//   F12          screenshot     F10       the frame-rate counter
//   F1           the binding screen
//   escape       quit, or back while the binding screen is up
//
// These are the launcher's own keys and are deliberately not bindable. Menu and Back in particular
// are the way out of a layout that no longer works, so they cannot be rebound away.
enum class Control : unsigned {
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    X,
    Y,
    Start,
    LeftTrigger,
    RightTrigger,
    Screenshot,
    ToggleFps,
    Menu,
    Back,
    Count
};

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // How finished frames reach the display. FIFO is vsync and is the only mode Vulkan guarantees
    // exists, so it is the default and the fallback. Under FIFO the swapchain paces the whole run
    // to the panel; the others let an unthrottled run go faster.
    enum class PresentMode { Fifo, Mailbox, Immediate };
    // Call before create(). A mode the surface does not offer falls back to FIFO rather than
    // failing.
    void set_present_mode(PresentMode m) noexcept { wanted_present_ = m; }
    // Where frames go. Vulkan (Auto, the default): the Vulkan swapchain. Dxgi: on
    // Windows, a DXGI flip-model swapchain (dxgi_present.h) -- about a refresh less latency where
    // the machine finishes every frame in time, falling back to Vulkan when it cannot initialise.
    // Call before create().
    enum class PresentPath { Auto, Vulkan, Dxgi };
    void set_present_path(PresentPath p) noexcept { wanted_path_ = p; }
    // "DXGI flip model (...)" or "Vulkan swapchain", once created.
    std::string present_path() const;
    // Frames are flipped to the display at the vblank after they are presented (the DXGI path),
    // so when within a refresh a frame is presented decides its latency and whether it makes
    // that vblank.
    bool flips() const noexcept { return dxgi_ != nullptr; }
    // Frames allowed to wait for the display on the DXGI path (adapts 1-3); 0 otherwise.
    int flip_queue_depth() const noexcept { return dxgi_ ? dxgi_->queue_depth() : 0; }

    // Opens the window and creates the context, surface and swapchain. False on failure with
    // error() set; a machine with no display is an ordinary failure, not an exception.
    bool create(const char* title, int width, int height);
    void destroy();

    // Acquires the next image. Returns false when the swapchain needs rebuilding (a resize), in
    // which case the caller should call recreate_swapchain() and try again.
    bool begin_frame(std::uint32_t& image_index, VkCommandBuffer& cmd);
    bool end_frame(std::uint32_t image_index);
    // NextUI builds: between begin_frame and end_frame, blits `src` (any size, in
    // TRANSFER_SRC_OPTIMAL) straight into the panel's next page. blits() is false in other builds
    // and where the device cannot blit into the pages: draw into the frame image instead.
    bool blits() const noexcept;
    void blit_frame(VkCommandBuffer cmd, VkImage src, VkExtent2D size, bool smooth);
    bool recreate_swapchain();

    // True while the user has not closed the window; pumps the event queue and refreshes the
    // control state below.
    bool poll();
    // Borderless fullscreen on the window's display. Alt+Enter toggles it in poll().
    void set_fullscreen(bool on) noexcept;
    bool fullscreen() const noexcept { return fullscreen_; }
    // Refresh rate of the display the window is on, in Hz; 0 when unknown.
    float refresh_rate() const noexcept;
    // The display's vblank grid: the time of a recent vblank and the refresh period,
    // on std::chrono::steady_clock. Windows: from the compositor (DWM). False where the platform
    // does not say, and then nothing can be phase-locked to the display.
    bool vblank_grid(std::chrono::steady_clock::time_point& vblank,
                     std::chrono::nanoseconds& period) const noexcept;

    // Held down as of the last poll().
    bool held(Control c) const noexcept;
    // Went down since the previous poll(): for controls that act once rather than hold.
    bool pressed(Control c) const noexcept;

    // Escape closes the window by default. The binding screen turns that off while it is up, so
    // that backing out of a capture is not also the end of the run; Control::Back still reports
    // the press either way.
    void set_escape_quits(bool yes) noexcept { escape_quits_ = yes; }
    bool escape_quits() const noexcept { return escape_quits_; }

    // Host overlays. `event_filter`, when set, sees every SDL_Event first (passed as
    // `const SDL_Event*`); returning true consumes it, so poll() does nothing else with it. The
    // keyboard and pad *state* the game reads is unaffected: the caller pauses the guest instead.
    std::function<bool(const void* sdl_event)> event_filter;
    // Whether a pad's Back/Select button counts as Control::Menu (the binding screen). An overlay
    // that takes that button for itself turns this off; F1 still opens the binding screen.
    void set_pad_menu_button(bool yes) noexcept { pad_menu_button_ = yes; }
    // While set, the guest pad reads as released (pad_held/pad_pressed false, pad_value 0) for
    // every player: an overlay that owns the keyboard and pads keeps the game running without
    // the game also acting on its input.
    void set_game_input_blocked(bool yes) noexcept { game_input_blocked_ = yes; }
    bool game_input_blocked() const noexcept { return game_input_blocked_; }
    // Threaded mode: the thread that created the window pumps SDL with
    // pump_events() while another thread runs the guest and calls poll(), which then works
    // through what was pumped instead of reading SDL itself. The OS's move and resize loops block
    // only the pumping thread, so the game keeps running while the window is dragged.
    void set_threaded(bool yes) noexcept { threaded_ = yes; }
    // The pumping thread: waits up to `timeout_ms` for events and queues them for poll().
    void pump_events(int timeout_ms);
    // Ends the run at the next poll(), as closing the window would.
    void request_close() noexcept { running_ = false; }
    // The window's size in pixels (0 x 0 before create()).
    void pixel_size(int& w, int& h) const noexcept;

    // --- the guest pad, through the player's bindings ------------------------------------------
    //
    // Control above is the launcher's own fixed keys (screenshot, capture, the menu). PadControl is
    // what the player binds, and the two are separate so that rebinding the game's buttons can
    // never take the menu key away and leave no way back.

    // Resolves the portable names in `b` to SDL codes once, here, rather than per frame.
    void set_bindings(const Bindings& b);
    const Bindings& bindings() const noexcept { return bindings_; }

    // Players: player 0 is the keyboard plus the first pad; pad N drives player N,
    // in connection order. The one-argument forms are player 0.
    static constexpr unsigned kMaxPlayers = 4;
    unsigned players() const noexcept {
        return pads_.empty() ? 1u : static_cast<unsigned>(std::min<std::size_t>(pads_.size(), kMaxPlayers));
    }
    bool pad_held(unsigned player, PadControl c) const noexcept;
    float pad_value(unsigned player, PadControl c) const noexcept;
    // Rumble on the pad driving `player`, strength 0..1 scaled by rumble_scale, for `ms`
    // milliseconds (0 stops). No-op without a pad or rumble motor.
    void rumble(unsigned player, float strength, unsigned ms) noexcept;
    float rumble_scale = 1.0f;

    bool pad_held(PadControl c) const noexcept;
    bool pad_pressed(PadControl c) const noexcept;
    // 0 to 1. A digital source reads 1 while held, except that a trigger with the ramp enabled
    // rises over about 150 ms, because a keyboard accelerator that is only ever fully down or
    // fully up is most of why a driving game needs a pad.
    float pad_value(PadControl c) const noexcept;

    // "KEYBOARD" first, then one row per connected pad. Rebuilt on hot-plug.
    const std::vector<std::string>& devices() const noexcept { return devices_; }
    bool gamepad_connected() const noexcept { return !pads_.empty(); }

    // Rebinding: the next physical input becomes a Binding rather than reaching the game. Escape
    // cancels. While capturing, pad_held() reports nothing, so the key being bound cannot also be
    // played.
    void begin_capture() noexcept;
    void cancel_capture() noexcept;
    bool capturing() const noexcept { return capturing_; }
    // True once, on the poll that captured something. `cancelled` distinguishes escape from a
    // binding, because those mean different things to the caller.
    bool take_capture(Binding& out, bool& cancelled) noexcept;


    Context& context() noexcept { return ctx_; }
    VkRenderPass render_pass() const noexcept { return render_pass_; }
    VkFramebuffer framebuffer(std::uint32_t i) const noexcept { return framebuffers_[i]; }
    VkExtent2D extent() const noexcept { return extent_; }
    const std::string& error() const noexcept { return error_; }

private:
    bool create_swapchain();
    bool create_vulkan_swapchain(std::uint32_t& count);
    bool create_targets(std::uint32_t count);
    void destroy_swapchain();

    SDL_Window* window_ = nullptr;
    Context ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    AttachmentImage depth_;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds_;
    std::vector<VkFence> fences_;
    std::vector<VkSemaphore> acquired_, rendered_;
    std::uint32_t frame_ = 0;
    bool held_[static_cast<unsigned>(Control::Count)]{};
    bool pressed_[static_cast<unsigned>(Control::Count)]{};
    bool running_ = true;
    bool threaded_ = false;
    struct EventQueue;
    std::shared_ptr<EventQueue> queue_;  // events pumped for poll() in threaded mode
    bool fullscreen_ = false;
    std::string error_;

    // One binding resolved to the codes SDL actually compares against.
    struct Resolved {
        BindSource source = BindSource::None;
        int code = 0;
        int sign = 1;
    };
    Bindings bindings_;
    Resolved key_[kBindingSlots][kPadControlCount]{};
    Resolved gpad_[kBindingSlots][kPadControlCount]{};
    bool pad_held_[kMaxPlayers][kPadControlCount]{};
    bool pad_pressed_[kMaxPlayers][kPadControlCount]{};
    float pad_value_[kMaxPlayers][kPadControlCount]{};
    // How long each trigger has been held, for the ramp. In poll ticks rather than seconds, scaled
    // by the measured frame interval, so a fast host does not ramp faster than a slow one.
    float ramp_[kMaxPlayers][kPadControlCount]{};
    std::uint64_t last_poll_ns_ = 0;

    std::vector<SDL_Gamepad*> pads_;
    std::vector<std::string> devices_;
    void refresh_devices();
    VkPresentModeKHR choose_present_mode() const;

    bool capturing_ = false, captured_ = false, capture_cancelled_ = false;
    bool escape_quits_ = true;
    bool pad_menu_button_ = true;
    bool game_input_blocked_ = false;
    PresentMode wanted_present_ = PresentMode::Fifo;
    PresentPath wanted_path_ = PresentPath::Auto;
    std::unique_ptr<DxgiPresenter> dxgi_;  // set when presenting through DXGI
    // NextUI builds (window_nextui.cpp): frames rendered into images of the panel's
    // size and copied into the framebuffer's pages; the pad read from its own input device.
    std::unique_ptr<Fb0Presenter> fb0_;
    std::vector<std::unique_ptr<AttachmentImage>> fb0_targets_;
    std::vector<std::uint32_t> fb0_pages_;  // per frame in flight: the page its frame went to
    bool fb0_blitted_ = false;              // this frame went into its page by blit_frame
    int input_fd_ = -1;
    std::uint32_t input_buttons_ = 0;  // bit per evdev button code from BTN_SOUTH
    int input_hat_x_ = 0, input_hat_y_ = 0, input_l2_ = 0, input_r2_ = 0;
    // The handheld's vibration motor (NextUI builds): on until rumble_until_ (poll()
    // stops it), at rumble_uv_ microvolts; written to the device only when they change.
    bool rumble_on_ = false;
    int rumble_uv_ = 0;
    int rumble_found_uv_ = -1;  // the voltage before the run, put back at its end
    std::chrono::steady_clock::time_point rumble_until_{};
    void rumble_off() noexcept;
    Binding capture_{};
};

// Where a player's bindings are kept when nothing else is asked for: the host's own per-user
// settings directory, so one layout follows them across every title rather than being written
// beside whichever game happened to be running. Empty when SDL cannot name such a place, which the
// caller should treat as "do not persist" rather than as a failure.
std::string default_bindings_path();

}  // namespace dream::render::vk
