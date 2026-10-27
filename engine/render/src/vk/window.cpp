// See window.h.
#include "dream/render/vk/window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace dream::render::vk {

namespace {
constexpr std::uint32_t kFramesInFlight = 2;
constexpr std::uint32_t kMaxSwapchainImages = 8;

// The keyboard layout, in Control order. Scancodes rather than key codes, so the physical key is
// the same wherever the layout puts the letter on it.
constexpr SDL_Scancode kScancodes[static_cast<unsigned>(Control::Count)] = {
    SDL_SCANCODE_UP,     SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_Z,      SDL_SCANCODE_X,    SDL_SCANCODE_A,    SDL_SCANCODE_S,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_Q,    SDL_SCANCODE_W,    SDL_SCANCODE_F12,
    SDL_SCANCODE_F10,    SDL_SCANCODE_F1,   SDL_SCANCODE_ESCAPE,
};

// The same controls on a pad, so somebody rebinding a pad does not have to reach for the keyboard
// to do it. Fixed like the keys above and for the same reason: these are the way out of a layout
// that no longer works. Back (select) opens the screen, south confirms, east goes back -- SDL's
// positional names, so the physical button is the same whatever the face labels say. INVALID means
// the control is keyboard-only: a screenshot is not something to hit by accident mid-race.
constexpr SDL_GamepadButton kNavButtons[static_cast<unsigned>(Control::Count)] = {
    SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_SOUTH,     SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_INVALID,    SDL_GAMEPAD_BUTTON_INVALID,   SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_INVALID,    SDL_GAMEPAD_BUTTON_INVALID,   SDL_GAMEPAD_BUTTON_INVALID,
    SDL_GAMEPAD_BUTTON_INVALID,    SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_EAST,
};
}  // namespace

struct Window::EventQueue {
    std::mutex mutex;
    std::vector<SDL_Event> events;
};

void Window::pump_events(int timeout_ms) {
    if (!queue_)
        return;
    SDL_Event ev;
    if (!SDL_WaitEventTimeout(&ev, timeout_ms))
        return;
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->events.push_back(ev);
    while (SDL_PollEvent(&ev)) queue_->events.push_back(ev);
}

Window::~Window() {
    destroy();
}

bool Window::create(const char* title, int width, int height) {
    // Gamepads are optional: a machine with none, or an SDL built without the subsystem, still
    // gets a window and a keyboard. Failing the whole launch over a missing joystick driver would
    // be a poor trade.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        SDL_ClearError();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error_ = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    window_ = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    queue_ = std::make_shared<EventQueue>();
    if (!window_) {
        error_ = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }

    std::uint32_t ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    std::vector<const char*> extensions(sdl_exts, sdl_exts + ext_count);
    if (!ctx_.create(extensions)) {
        error_ = ctx_.error();
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window_, ctx_.instance(), nullptr, &surface_)) {
        error_ = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx_.physical_device(), ctx_.queue_family(), surface_,
                                         &supported);
    if (!supported) {
        error_ = "the graphics queue cannot present to this surface";
        return false;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx_.queue_family();
    vkCreateCommandPool(ctx_.device(), &pci, nullptr, &pool_);

    cmds_.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFramesInFlight;
    vkAllocateCommandBuffers(ctx_.device(), &cai, cmds_.data());

    fences_.resize(kFramesInFlight);
    acquired_.resize(kFramesInFlight);
    // "Rendered" semaphores are per swapchain image, not per frame in flight: the presentation
    // engine releases one only when that image is presented again, so reusing it by frame index
    // signalled a semaphore still pending (VUID-vkQueueSubmit-pSignalSemaphores-00067).
    rendered_.resize(kMaxSwapchainImages);
    for (std::uint32_t i = 0; i < kMaxSwapchainImages; ++i) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &rendered_[i]);
    }
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_.device(), &fci, nullptr, &fences_[i]);
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(ctx_.device(), &sci, nullptr, &acquired_[i]);
    }
    // Whatever is already plugged in, plus the default bindings, so a pad works on a first run
    // with no configuration file and no visit to any UI.
    refresh_devices();
    set_bindings(Bindings::defaults());
#ifdef _WIN32
    if (wanted_path_ == PresentPath::Dxgi) {
        std::string why;
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        dxgi_ = hwnd ? DxgiPresenter::create(ctx_, hwnd, why) : nullptr;
        if (!dxgi_)
            std::fprintf(stderr, "window: not presenting through DXGI: %s\n",
                         hwnd ? why.c_str() : "no HWND");
    }
#endif
    if (!create_swapchain() && dxgi_) {
        // The shared images did not work out on this machine: present through Vulkan instead.
        std::fprintf(stderr, "window: DXGI presentation failed (%s); using the Vulkan swapchain\n",
                     error_.c_str());
        destroy_swapchain();
        dxgi_.reset();
        error_.clear();
        return create_swapchain();
    }
    return true;
}

std::string Window::present_path() const {
    return dxgi_ ? dxgi_->description() : std::string("Vulkan swapchain");
}

// FIFO is the only mode the specification requires a surface to support, so everything else is
// asked for and accepted only if the surface offers it: a present mode the surface lacks falls
// back to FIFO rather than refusing to open the window.
VkPresentModeKHR Window::choose_present_mode() const {
    if (wanted_present_ == PresentMode::Fifo)
        return VK_PRESENT_MODE_FIFO_KHR;
    const VkPresentModeKHR want = wanted_present_ == PresentMode::Mailbox
                                      ? VK_PRESENT_MODE_MAILBOX_KHR
                                      : VK_PRESENT_MODE_IMMEDIATE_KHR;
    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical_device(), surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    if (count)
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical_device(), surface_, &count,
                                                  modes.data());
    for (VkPresentModeKHR m : modes)
        if (m == want)
            return want;
    std::fprintf(stderr, "present mode not available on this surface; using vsync\n");
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool Window::create_swapchain() {
    std::uint32_t count = 0;
    if (dxgi_) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent_ = {static_cast<std::uint32_t>(std::max(w, 0)), static_cast<std::uint32_t>(std::max(h, 0))};
        if (extent_.width == 0 || extent_.height == 0)
            return true;  // minimised: nothing to build yet
        format_ = VK_FORMAT_B8G8R8A8_UNORM;
        std::string why;
        if (!dxgi_->resize(extent_.width, extent_.height, why)) {
            error_ = why;
            return false;
        }
        images_ = dxgi_->images();
        count = static_cast<std::uint32_t>(images_.size());
    } else if (!create_vulkan_swapchain(count)) {
        return false;
    } else if (!swapchain_) {
        return true;  // minimised
    }
    return create_targets(count);
}

bool Window::create_vulkan_swapchain(std::uint32_t& count) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical_device(), surface_, &caps);

    std::uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical_device(), surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical_device(), surface_, &format_count,
                                         formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            chosen = f;
    format_ = chosen.format;

    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent_.width = std::clamp(static_cast<std::uint32_t>(w), caps.minImageExtent.width,
                                   caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<std::uint32_t>(h), caps.minImageExtent.height,
                                    caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0)
        return true;  // minimised: nothing to build yet

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0)
        image_count = std::min(image_count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = format_;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = choose_present_mode();
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(ctx_.device(), &sci, nullptr, &swapchain_) != VK_SUCCESS) {
        error_ = "vkCreateSwapchainKHR failed";
        return false;
    }

    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &count, images_.data());
    return true;
}

// The render pass, depth buffer, views and framebuffers over images_ (either path).
bool Window::create_targets(std::uint32_t count) {
    // Colour and depth. The renderer writes depth from the fragment shader (the hardware's depth
    // is 1/w), so a depth attachment is needed even for opaque geometry.
    depth_format_ = pick_depth_format(ctx_.physical_device());
    if (depth_format_ == VK_FORMAT_UNDEFINED) {
        error_ = "no supported depth format";
        return false;
    }
    if (!depth_.create(ctx_, depth_format_, extent_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT)) {
        error_ = "could not create the depth buffer";
        return false;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = format_;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // A DXGI shared image is handed to D3D11, not to a presentation engine.
    attachments[0].finalLayout = dxgi_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[1].format = depth_format_;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    // The depth image is shared by the frames in flight: its clear (a layout transition from
    // UNDEFINED) must also wait for the previous frame's depth writes (late tests), not only for
    // colour output (synchronization validation: WRITE_AFTER_WRITE).
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci{};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 2;
    rci.pAttachments = attachments;
    rci.subpassCount = 1;
    rci.pSubpasses = &subpass;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    vkCreateRenderPass(ctx_.device(), &rci, nullptr, &render_pass_);

    views_.resize(count);
    framebuffers_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx_.device(), &vci, nullptr, &views_[i]);
        const VkImageView fb_attachments[2] = {views_[i], depth_.view()};
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = render_pass_;
        fci.attachmentCount = 2;
        fci.pAttachments = fb_attachments;
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        vkCreateFramebuffer(ctx_.device(), &fci, nullptr, &framebuffers_[i]);
    }
    return true;
}

void Window::destroy_swapchain() {
    if (!ctx_.device())
        return;
    vkDeviceWaitIdle(ctx_.device());
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(ctx_.device(), f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(ctx_.device(), v, nullptr);
    framebuffers_.clear();
    views_.clear();
    images_.clear();
    depth_.destroy();
    if (render_pass_) {
        vkDestroyRenderPass(ctx_.device(), render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
    if (swapchain_) {
        vkDestroySwapchainKHR(ctx_.device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    if (dxgi_)
        dxgi_->release_images();
}

bool Window::recreate_swapchain() {
    destroy_swapchain();
    return create_swapchain();
}

void Window::refresh_devices() {
    for (SDL_Gamepad* g : pads_)
        if (g)
            SDL_CloseGamepad(g);
    pads_.clear();
    devices_.clear();
    devices_.push_back("KEYBOARD");
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) {
                pads_.push_back(g);
                const char* name = SDL_GetGamepadName(g);
                devices_.push_back(name && *name ? name : "GAMEPAD");
            }
        }
        SDL_free(ids);
    }
}

void Window::set_bindings(const Bindings& b) {
    bindings_ = b;
    // Names to codes once, here. Doing it per frame would mean a string lookup per control per
    // frame, and would also mean a typo in the file costing performance rather than being noticed.
    auto resolve = [](const DeviceBindings& d, Resolved* out, unsigned slot, bool keyboard) {
        for (unsigned i = 0; i < kPadControlCount; ++i) {
            const Binding& bind = d.slot(static_cast<PadControl>(i), slot);
            out[i] = Resolved{};
            if (!bind.bound())
                continue;
            if (keyboard && bind.source == BindSource::Key) {
                const SDL_Scancode sc = SDL_GetScancodeFromName(bind.code.c_str());
                if (sc != SDL_SCANCODE_UNKNOWN)
                    out[i] = Resolved{BindSource::Key, static_cast<int>(sc), 1};
            } else if (!keyboard && bind.source == BindSource::Button) {
                const SDL_GamepadButton bt = SDL_GetGamepadButtonFromString(bind.code.c_str());
                if (bt != SDL_GAMEPAD_BUTTON_INVALID)
                    out[i] = Resolved{BindSource::Button, static_cast<int>(bt), 1};
            } else if (!keyboard && bind.source == BindSource::Axis) {
                const SDL_GamepadAxis ax = SDL_GetGamepadAxisFromString(bind.code.c_str());
                if (ax != SDL_GAMEPAD_AXIS_INVALID)
                    out[i] = Resolved{BindSource::Axis, static_cast<int>(ax), bind.sign};
            }
            // Anything that did not resolve stays unbound: an unknown name must not throw, and
            // must not silently become scancode 0, which is a real key.
        }
    };
    for (unsigned s = 0; s < kBindingSlots; ++s) {
        resolve(bindings_.keyboard, key_[s], s, true);
        resolve(bindings_.gamepad, gpad_[s], s, false);
    }
    SDL_ClearError();
}

void Window::begin_capture() noexcept {
    capturing_ = true;
    captured_ = false;
    capture_cancelled_ = false;
    capture_ = Binding{};
}

void Window::cancel_capture() noexcept {
    capturing_ = false;
    captured_ = false;
}

bool Window::take_capture(Binding& out, bool& cancelled) noexcept {
    if (!captured_)
        return false;
    captured_ = false;
    out = capture_;
    cancelled = capture_cancelled_;
    return true;
}

void Window::pixel_size(int& w, int& h) const noexcept {
    w = h = 0;
    if (window_)
        SDL_GetWindowSizeInPixels(window_, &w, &h);
}

bool Window::poll() {
    // A capture consumes the physical input that ends it, so the key being bound does not also
    // reach the game on the same frame.
    bool captured_this_poll = false;
    std::vector<SDL_Event> batch;
    if (threaded_) {
        if (queue_) {
            std::lock_guard<std::mutex> lock(queue_->mutex);
            batch.swap(queue_->events);
        }
    } else {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) batch.push_back(ev);
    }
    for (const SDL_Event& ev : batch) {
        // a host overlay sees each event first and may keep it.
        if (event_filter && ev.type != SDL_EVENT_QUIT &&
            ev.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED && event_filter(&ev))
            continue;
        switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running_ = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                // Hot-plug: rebuild the list and drop every held state. A pad yanked mid-race must
                // release the accelerator rather than leave it held at its last value.
                refresh_devices();
                for (unsigned p = 0; p < kMaxPlayers; ++p)
                    for (unsigned i = 0; i < kPadControlCount; ++i) {
                        pad_held_[p][i] = false;
                        pad_pressed_[p][i] = false;
                        pad_value_[p][i] = 0.0f;
                        ramp_[p][i] = 0.0f;
                    }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (capturing_ && !captured_this_poll) {
                    captured_this_poll = true;
                    captured_ = true;
                    capturing_ = false;
                    if (ev.key.key == SDLK_ESCAPE) {
                        capture_cancelled_ = true;
                    } else if (const char* n = SDL_GetScancodeName(ev.key.scancode); n && *n) {
                        capture_ = Binding{BindSource::Key, n, 1};
                    } else {
                        capture_cancelled_ = true;
                    }
                } else if (ev.key.key == SDLK_ESCAPE && escape_quits_) {
                    running_ = false;
                } else if (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT) &&
                           !ev.key.repeat) {
                    set_fullscreen(!fullscreen_);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (capturing_ && !captured_this_poll) {
                    if (const char* n = SDL_GetGamepadStringForButton(
                            static_cast<SDL_GamepadButton>(ev.gbutton.button));
                        n && *n) {
                        captured_this_poll = true;
                        captured_ = true;
                        capturing_ = false;
                        capture_ = Binding{BindSource::Button, n, 1};
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                // Well past any plausible dead zone, so resting drift never binds an axis by
                // itself while the user is thinking about which button to press.
                if (capturing_ && !captured_this_poll && std::abs(ev.gaxis.value) > 20000) {
                    if (const char* n = SDL_GetGamepadStringForAxis(
                            static_cast<SDL_GamepadAxis>(ev.gaxis.axis));
                        n && *n) {
                        captured_this_poll = true;
                        captured_ = true;
                        capturing_ = false;
                        capture_ = Binding{BindSource::Axis, n, ev.gaxis.value < 0 ? -1 : 1};
                    }
                }
                break;
            default:
                break;
        }
    }

    // Read the whole keyboard rather than tracking key events: a frame wants the state as it is
    // now, and a key pressed and released between two polls should not be missed or repeated.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    for (unsigned i = 0; i < static_cast<unsigned>(Control::Count); ++i) {
        const bool was = held_[i];
        bool now = keys && keys[kScancodes[i]];
        if (kNavButtons[i] != SDL_GAMEPAD_BUTTON_INVALID &&
            (pad_menu_button_ || i != static_cast<unsigned>(Control::Menu)))
            for (SDL_Gamepad* g : pads_)
                if (g && SDL_GetGamepadButton(g, kNavButtons[i]))
                    now = true;
        held_[i] = now;
        pressed_[i] = now && !was;
    }

    // Seconds since the last poll, for the trigger ramp. Measured rather than assumed, so the ramp
    // takes the same wall time on a fast host as on a slow one.
    const std::uint64_t now_ns = SDL_GetTicksNS();
    float dt = last_poll_ns_ ? static_cast<float>(now_ns - last_poll_ns_) * 1e-9f : 0.0f;
    last_poll_ns_ = now_ns;
    dt = std::clamp(dt, 0.0f, 0.25f);  // a long stall must not jump the ramp to full

    const float dead = static_cast<float>(bindings_.deadzone_percent) / 100.0f;
    for (unsigned p = 0; p < kMaxPlayers; ++p) {
    // Keyboard drives player 0; pad N drives player N (connection order).
    SDL_Gamepad* const player_pad = p < pads_.size() ? pads_[p] : nullptr;
    for (unsigned i = 0; i < kPadControlCount; ++i) {
        // Digital and analogue sources are gathered separately, because only a digital one ramps.
        // A pad's real trigger position must pass straight through, or a pad would feel worse than
        // the keyboard it is replacing.
        bool digital_on = false;
        float analogue = 0.0f;
        // While capturing, the game gets nothing: a key held down to bind it must not also drive.
        if (!capturing_) {
            for (unsigned s = 0; s < kBindingSlots; ++s) {
            const Resolved& kb = key_[s][i];
            const Resolved& gb = gpad_[s][i];
            if (p == 0 && keys && kb.source == BindSource::Key && keys[kb.code])
                digital_on = true;
            if (SDL_Gamepad* g = player_pad) {
                if (gb.source == BindSource::Button) {
                    if (SDL_GetGamepadButton(g, static_cast<SDL_GamepadButton>(gb.code)))
                        digital_on = true;
                } else if (gb.source == BindSource::Axis) {
                    const float raw = static_cast<float>(SDL_GetGamepadAxis(
                                          g, static_cast<SDL_GamepadAxis>(gb.code))) /
                                      32767.0f;
                    // Only the bound half counts, so left and right are separate bindings on one
                    // stick.
                    const float half = gb.sign < 0 ? -raw : raw;
                    if (half > dead) {
                        // Rescaled from the dead zone's edge rather than from zero, or the stick
                        // would jump to `dead` the instant it left the centre.
                        analogue =
                            std::max(analogue, std::min(1.0f, (half - dead) / (1.0f - dead)));
                    }
                }
            }
            }
        }

        const bool is_trigger = i == static_cast<unsigned>(PadControl::LeftTrigger) ||
                                i == static_cast<unsigned>(PadControl::RightTrigger);
        float value = analogue;
        if (is_trigger && bindings_.trigger_ramp) {
            constexpr float kRampSeconds = 0.15f;
            ramp_[p][i] = digital_on ? std::min(1.0f, ramp_[p][i] + dt / kRampSeconds) : 0.0f;
            value = std::max(value, ramp_[p][i]);
        } else {
            ramp_[p][i] = 0.0f;
            if (digital_on)
                value = 1.0f;
        }

        const bool was = pad_held_[p][i];
        pad_held_[p][i] = value > 0.0f;
        pad_pressed_[p][i] = pad_held_[p][i] && !was;
        pad_value_[p][i] = value;
    }
    }
    return running_;
}

float Window::refresh_rate() const noexcept {
    if (!window_)
        return 0.0f;
    const SDL_DisplayID id = SDL_GetDisplayForWindow(window_);
    const SDL_DisplayMode* mode = id ? SDL_GetCurrentDisplayMode(id) : nullptr;
    return mode ? mode->refresh_rate : 0.0f;
}

bool Window::vblank_grid(std::chrono::steady_clock::time_point& vblank,
                         std::chrono::nanoseconds& period) const noexcept {
#ifdef _WIN32
    // DwmGetCompositionTimingInfo, loaded at run time so nothing links against dwmapi.
    using Fn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
    static const Fn get = [] {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? reinterpret_cast<Fn>(GetProcAddress(m, "DwmGetCompositionTimingInfo")) : nullptr;
    }();
    static const long long freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    if (!get)
        return false;
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    if (FAILED(get(nullptr, &ti)) || ti.qpcRefreshPeriod == 0 || ti.qpcVBlank == 0)
        return false;
    // MSVC's steady_clock is the performance counter in nanoseconds, so QPC values convert
    // directly (same epoch).
    auto to_ns = [](long long qpc) {
        return std::chrono::nanoseconds(static_cast<long long>(
            static_cast<double>(qpc) * 1e9 / static_cast<double>(freq)));
    };
    vblank = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            to_ns(static_cast<long long>(ti.qpcVBlank))));
    period = to_ns(static_cast<long long>(ti.qpcRefreshPeriod));
    return true;
#else
    (void)vblank;
    (void)period;
    return false;
#endif
}

void Window::set_fullscreen(bool on) noexcept {
    if (!window_)
        return;
    if (threaded_ && !SDL_IsMainThread()) {
        // Window state belongs to the thread that pumps its events; ask that one to do it.
        struct Request {
            SDL_Window* window;
            bool on;
        };
        fullscreen_ = on;
        SDL_RunOnMainThread(
            [](void* p) {
                auto* r = static_cast<Request*>(p);
                SDL_SetWindowFullscreen(r->window, r->on);
                delete r;
            },
            new Request{window_, on}, false);
        return;
    }
    if (SDL_SetWindowFullscreen(window_, on))
        fullscreen_ = on;
    // The swapchain follows on the next acquire (out of date) or present (suboptimal).
}

bool Window::pad_held(PadControl c) const noexcept {
    return !game_input_blocked_ && pad_held_[0][static_cast<unsigned>(c)];
}

bool Window::pad_pressed(PadControl c) const noexcept {
    return !game_input_blocked_ && pad_pressed_[0][static_cast<unsigned>(c)];
}

float Window::pad_value(PadControl c) const noexcept {
    return game_input_blocked_ ? 0.0f : pad_value_[0][static_cast<unsigned>(c)];
}

bool Window::pad_held(unsigned player, PadControl c) const noexcept {
    return !game_input_blocked_ && player < kMaxPlayers && pad_held_[player][static_cast<unsigned>(c)];
}

float Window::pad_value(unsigned player, PadControl c) const noexcept {
    return (!game_input_blocked_ && player < kMaxPlayers) ? pad_value_[player][static_cast<unsigned>(c)]
                                                          : 0.0f;
}

void Window::rumble(unsigned player, float strength, unsigned ms) noexcept {
    if (player >= pads_.size() || !pads_[player])
        return;
    const float s = std::clamp(strength * rumble_scale, 0.0f, 1.0f);
    const auto level = static_cast<Uint16>(s * 65535.0f);
    SDL_RumbleGamepad(pads_[player], level, level, s > 0.0f ? ms : 0u);
}

bool Window::held(Control c) const noexcept {
    return held_[static_cast<unsigned>(c)];
}

bool Window::pressed(Control c) const noexcept {
    return pressed_[static_cast<unsigned>(c)];
}

bool Window::begin_frame(std::uint32_t& image_index, VkCommandBuffer& cmd) {
    if (!swapchain_ && !(dxgi_ && !images_.empty()))
        return false;
    // A resized window: some drivers keep presenting the old size without ever
    // reporting the swapchain out of date, and the picture then lands offset and smeared. Rebuild
    // when the window's pixel size and the swapchain's differ (not while minimised).
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        if (w > 0 && h > 0 &&
            (static_cast<std::uint32_t>(w) != extent_.width ||
             static_cast<std::uint32_t>(h) != extent_.height)) {
            recreate_swapchain();
            if (!swapchain_ && !(dxgi_ && !images_.empty()))
                return false;
        }
    }
    if (dxgi_) {
        // DXGI: one shared image per frame in flight; its fence says Vulkan is done with it, its
        // keyed mutex (in the submit) that D3D11 is.
        // (The latency object is checked by present(), never waited on here.)
        vkWaitForFences(ctx_.device(), 1, &fences_[frame_], VK_TRUE, UINT64_MAX);
        image_index = frame_;
    } else {
    vkWaitForFences(ctx_.device(), 1, &fences_[frame_], VK_TRUE, UINT64_MAX);
    const VkResult r = vkAcquireNextImageKHR(ctx_.device(), swapchain_, UINT64_MAX,
                                             acquired_[frame_], VK_NULL_HANDLE, &image_index);
    // SUBOPTIMAL still acquired the image and will signal the semaphore: carry on with this frame
    // (vkQueuePresentKHR reports it again and the swapchain is rebuilt then). Returning here left
    // the semaphore signalled with nobody waiting, and its next use was a validation error.
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
        return false;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        error_ = "vkAcquireNextImageKHR failed";
        return false;
    }
    }
    vkResetFences(ctx_.device(), 1, &fences_[frame_]);
    cmd = cmds_[frame_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return true;
}

bool Window::blits() const noexcept {
    return false;  // a swapchain's images are drawn into (NextUI builds blit into fb0's pages)
}

void Window::blit_frame(VkCommandBuffer, VkImage, VkExtent2D, bool) {}

bool Window::end_frame(std::uint32_t image_index) {
    VkCommandBuffer cmd = cmds_[frame_];
    vkEndCommandBuffer(cmd);
    if (dxgi_) {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext = dxgi_->submit_chain(image_index);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(ctx_.queue(), 1, &si, fences_[frame_]);
        // In order, with the queue kept short (Queued); tearing for --present-mode immediate.
        const auto mode = wanted_present_ == PresentMode::Immediate ? DxgiPresenter::Mode::Tearing
                                                                    : DxgiPresenter::Mode::Queued;
        const bool ok = dxgi_->present(image_index, mode);
        frame_ = (frame_ + 1) % kFramesInFlight;
        return ok;
    }
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_[frame_];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[image_index % kMaxSwapchainImages];
    vkQueueSubmit(ctx_.queue(), 1, &si, fences_[frame_]);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[image_index % kMaxSwapchainImages];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index;
    const VkResult r = vkQueuePresentKHR(ctx_.queue(), &pi);
    frame_ = (frame_ + 1) % kFramesInFlight;
    return r == VK_SUCCESS;
}

void Window::destroy() {
    if (ctx_.device()) {
        vkDeviceWaitIdle(ctx_.device());
        destroy_swapchain();
        dxgi_.reset();
        for (VkFence f : fences_) vkDestroyFence(ctx_.device(), f, nullptr);
        for (VkSemaphore s : acquired_) vkDestroySemaphore(ctx_.device(), s, nullptr);
        for (VkSemaphore s : rendered_) vkDestroySemaphore(ctx_.device(), s, nullptr);
        fences_.clear();
        acquired_.clear();
        rendered_.clear();
        if (pool_) {
            vkDestroyCommandPool(ctx_.device(), pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
        }
    }
    if (surface_ && ctx_.instance()) {
        vkDestroySurfaceKHR(ctx_.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    ctx_.destroy();
    for (SDL_Gamepad* g : pads_)
        if (g)
            SDL_CloseGamepad(g);
    pads_.clear();
    devices_.clear();
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        // Only the subsystem this class started. SDL_Quit() shuts down every subsystem in the
        // process, including the audio one the launcher's sink is still holding a stream from,
        // and the sink's destructor then runs against a torn-down subsystem and crashes the exit.
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

// SDL knows where each host keeps per-user settings, which is the only reason this lives in the
// SDL layer rather than beside the binding model: the model stays free of SDL. Used when no
// --bindings is given (the port layer passes one beside its settings).
std::string default_bindings_path() {
    char* dir = SDL_GetPrefPath("", "rez_recompiled");
    if (!dir)
        return {};
    std::string path = std::string(dir) + "bindings.txt";
    SDL_free(dir);
    return path;
}

}  // namespace dream::render::vk
