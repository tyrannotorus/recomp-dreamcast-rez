// See window.h. The Window of a NextUI handheld build, compiled in place of the SDL3 window.cpp:
// no window system, SDL3 or swapchain. Frames render into images of the panel's size and the
// Fb0Presenter copies each into a page of /dev/fb0 and pans to it. The pad is read from its input
// device: MENU ends the run, the rest goes to the game's pad.
#include "dream/render/vk/window.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace dream::render::vk {

namespace {
constexpr std::uint32_t kFramesInFlight = 2;

// The Brick's pad ("TRIMUI Player1"): buttons by evdev code (BTN_SOUTH is B, BTN_EAST A, as
// labelled), the d-pad on the hat, L2 and R2 on two axes.
int open_pad() {
    char path[32], name[128];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && std::strcmp(name, "TRIMUI Player1") == 0)
            return fd;
        close(fd);
    }
    return -1;
}

bool button(std::uint32_t bits, int code) {
    return code >= BTN_SOUTH && code < BTN_SOUTH + 32 && (bits >> (code - BTN_SOUTH) & 1u);
}

// The vibration motor, as NextUI's tg5040 PLAT_setRumble drives it: strength as the driver's
// voltage (0.5 to 3.3 V, in microvolts), on and off by GPIO 227 (exported by NextUI).
constexpr const char* kRumblePower = "/sys/class/gpio/gpio227/value";
constexpr const char* kRumbleVoltage = "/sys/class/motor/voltage";
constexpr int kRumbleMinUv = 500000, kRumbleMaxUv = 3300000;

void write_int(const char* path, int value) noexcept {
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    char text[16];
    const int n = std::snprintf(text, sizeof text, "%d", value);
    if (write(fd, text, static_cast<std::size_t>(n)) != n) {
        // nothing to do: the motor just does not run
    }
    close(fd);
}

int read_int(const char* path) noexcept {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    char text[16] = {};
    const ssize_t n = read(fd, text, sizeof text - 1);
    close(fd);
    return n > 0 ? std::atoi(text) : -1;
}
}  // namespace

void Window::pump_events(int timeout_ms) {
    // Nothing to pump: poll() reads the pad itself. The caller only waits for the run to end, so
    // a coarse sleep keeps it off the CPU.
    (void)timeout_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

Window::~Window() {
    destroy();
}

bool Window::create(const char* title, int width, int height) {
    (void)title, (void)width, (void)height;  // the panel's size, always
    if (!ctx_.create({})) {
        error_ = ctx_.error();
        return false;
    }
    fb0_ = Fb0Presenter::create(ctx_, error_);
    if (!fb0_)
        return false;

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
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_.device(), &fci, nullptr, &fences_[i]);
    }
    fb0_pages_.assign(kFramesInFlight, 0);

    input_fd_ = open_pad();
    if (input_fd_ < 0)
        std::fprintf(stderr, "window: no pad (TRIMUI Player1); the game gets no input\n");
    // The motor off, in case an earlier run left it on; NextUI's voltage, to restore on exit.
    write_int(kRumblePower, 0);
    rumble_found_uv_ = read_int(kRumbleVoltage);
    set_bindings(Bindings::defaults());
    return create_swapchain();
}

std::string Window::present_path() const {
    return "fb0";
}

VkPresentModeKHR Window::choose_present_mode() const {
    return VK_PRESENT_MODE_FIFO_KHR;  // the pan waits for the vblank
}

bool Window::create_swapchain() {
    extent_ = fb0_->extent();
    format_ = VK_FORMAT_B8G8R8A8_UNORM;  // fb0's own layout
    fb0_targets_.clear();
    images_.clear();
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        auto img = std::make_unique<AttachmentImage>();
        if (!img->create(ctx_, format_, extent_,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT)) {
            error_ = "could not create the frame images";
            return false;
        }
        images_.push_back(img->image());
        fb0_targets_.push_back(std::move(img));
    }
    return create_targets(kFramesInFlight);
}

bool Window::create_vulkan_swapchain(std::uint32_t& count) {
    count = 0;
    return false;  // never: there is no surface
}

// The render pass, depth buffer and framebuffers over the frame images. As window.cpp's, except
// that a finished frame is left ready to be copied into fb0, not presented.
bool Window::create_targets(std::uint32_t count) {
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
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // The copy into fb0 reads the colour after the pass has written it.
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rci{};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 2;
    rci.pAttachments = attachments;
    rci.subpassCount = 1;
    rci.pSubpasses = &subpass;
    rci.dependencyCount = 2;
    rci.pDependencies = deps;
    vkCreateRenderPass(ctx_.device(), &rci, nullptr, &render_pass_);

    framebuffers_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkImageView fb_attachments[2] = {fb0_targets_[i]->view(), depth_.view()};
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
    framebuffers_.clear();
    images_.clear();
    fb0_targets_.clear();
    depth_.destroy();
    if (render_pass_) {
        vkDestroyRenderPass(ctx_.device(), render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
}

bool Window::recreate_swapchain() {
    return true;  // the panel never changes size
}

void Window::refresh_devices() {
    devices_.clear();
    if (input_fd_ >= 0)
        devices_.push_back("TRIMUI Player1");
}

void Window::set_bindings(const Bindings& b) {
    bindings_ = b;  // kept for the launcher; the pad's layout here is fixed (poll())
}

void Window::begin_capture() noexcept {}
void Window::cancel_capture() noexcept {}
bool Window::take_capture(Binding& out, bool& cancelled) noexcept {
    (void)out;
    cancelled = true;
    return false;
}

void Window::pixel_size(int& w, int& h) const noexcept {
    w = static_cast<int>(extent_.width);
    h = static_cast<int>(extent_.height);
}

bool Window::poll() {
    if (input_fd_ >= 0) {
        input_event ev;
        while (read(input_fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) {
            if (ev.type == EV_KEY && ev.code >= BTN_SOUTH && ev.code < BTN_SOUTH + 32) {
                const std::uint32_t bit = 1u << (ev.code - BTN_SOUTH);
                input_buttons_ = ev.value ? input_buttons_ | bit : input_buttons_ & ~bit;
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_HAT0X)
                    input_hat_x_ = ev.value;
                else if (ev.code == ABS_HAT0Y)
                    input_hat_y_ = ev.value;
                else if (ev.code == ABS_Z)
                    input_l2_ = ev.value;
                else if (ev.code == ABS_RZ)
                    input_r2_ = ev.value;
            }
        }
    }
    // MENU: back to NextUI.
    if (button(input_buttons_, BTN_MODE))
        running_ = false;
    // The motor's time up, or the game no longer getting the pad (a menu over it, the run ending).
    if (rumble_on_ && (!running_ || game_input_blocked_ || std::chrono::steady_clock::now() >= rumble_until_))
        rumble_off();
    // The game's pad, by the labels on the device; L1/L2 and R1/R2 the triggers.
    const bool on[kPadControlCount] = {
        button(input_buttons_, BTN_EAST),  button(input_buttons_, BTN_SOUTH), button(input_buttons_, BTN_WEST),
        button(input_buttons_, BTN_NORTH), button(input_buttons_, BTN_START), input_hat_y_ < 0,
        input_hat_y_ > 0,                  input_hat_x_ < 0,                  input_hat_x_ > 0,
        button(input_buttons_, BTN_TL) || input_l2_ > 0,
        button(input_buttons_, BTN_TR) || input_r2_ > 0,
        false, false, false, false};
    for (unsigned i = 0; i < kPadControlCount; ++i) {
        const bool was = pad_held_[0][i];
        pad_held_[0][i] = on[i];
        pad_pressed_[0][i] = on[i] && !was;
        pad_value_[0][i] = on[i] ? 1.0f : 0.0f;
    }
    return running_;
}

float Window::refresh_rate() const noexcept {
    return fb0_ ? fb0_->refresh_hz() : 0.0f;
}

bool Window::vblank_grid(std::chrono::steady_clock::time_point& vblank,
                         std::chrono::nanoseconds& period) const noexcept {
    (void)vblank;
    (void)period;
    return false;
}

void Window::set_fullscreen(bool on) noexcept {
    fullscreen_ = on;  // always the whole panel
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

// The handheld's motor for player 1, the strength scaled by rumble_scale onto the driver's voltage
// range. Called on the guest's thread, as is poll(), which stops it when its time is up.
void Window::rumble(unsigned player, float strength, unsigned ms) noexcept {
    if (player != 0)
        return;
    const float s = std::clamp(strength * rumble_scale, 0.0f, 1.0f);
    if (s <= 0.0f || ms == 0) {
        rumble_off();
        return;
    }
    const int uv = kRumbleMinUv + static_cast<int>(s * static_cast<float>(kRumbleMaxUv - kRumbleMinUv));
    if (uv != rumble_uv_) {
        write_int(kRumbleVoltage, uv);
        rumble_uv_ = uv;
    }
    if (!rumble_on_) {
        write_int(kRumblePower, 1);
        rumble_on_ = true;
    }
    rumble_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

void Window::rumble_off() noexcept {
    if (!rumble_on_)
        return;
    write_int(kRumblePower, 0);
    rumble_on_ = false;
}

bool Window::held(Control c) const noexcept {
    return held_[static_cast<unsigned>(c)];
}

bool Window::pressed(Control c) const noexcept {
    return pressed_[static_cast<unsigned>(c)];
}

bool Window::begin_frame(std::uint32_t& image_index, VkCommandBuffer& cmd) {
    if (!fb0_ || images_.empty())
        return false;
    // This slot's last frame is on screen (or replaced there) before its image is drawn again.
    fb0_->wait_slot(frame_);
    vkWaitForFences(ctx_.device(), 1, &fences_[frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(ctx_.device(), 1, &fences_[frame_]);
    image_index = frame_;
    cmd = cmds_[frame_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return true;
}

// The rendered frame goes straight into the next page in one blit, which scales it and turns its
// RGBA into fb0's BGRA.
bool Window::blits() const noexcept {
    return fb0_ && fb0_->can_blit();
}

void Window::blit_frame(VkCommandBuffer cmd, VkImage src, VkExtent2D size, bool smooth) {
    fb0_->record_blit(cmd, src, size, smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, fb0_pages_[frame_]);
    fb0_blitted_ = true;
}

// The frame's copy into the next page goes into its own submission; the present thread pans once
// the fence says the page holds it, and the game goes on meanwhile.
bool Window::end_frame(std::uint32_t image_index) {
    VkCommandBuffer cmd = cmds_[frame_];
    if (!fb0_blitted_)
        fb0_->record_copy(cmd, images_[image_index], fb0_pages_[frame_]);
    fb0_blitted_ = false;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(ctx_.queue(), 1, &si, fences_[frame_]) != VK_SUCCESS) {
        error_ = "vkQueueSubmit failed";
        return false;
    }
    fb0_->queue(frame_, fences_[frame_], fb0_pages_[frame_]);
    frame_ = (frame_ + 1) % kFramesInFlight;
    return true;
}

void Window::destroy() {
    rumble_off();
    if (rumble_uv_ != 0 && rumble_found_uv_ > 0) {
        write_int(kRumbleVoltage, rumble_found_uv_);
        rumble_uv_ = 0;
    }
    if (ctx_.device()) {
        vkDeviceWaitIdle(ctx_.device());
        destroy_swapchain();
        fb0_.reset();  // NextUI's page shown again
        for (VkFence f : fences_) vkDestroyFence(ctx_.device(), f, nullptr);
        fences_.clear();
        if (pool_) {
            vkDestroyCommandPool(ctx_.device(), pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
        }
    }
    ctx_.destroy();
    if (input_fd_ >= 0) {
        close(input_fd_);
        input_fd_ = -1;
    }
    devices_.clear();
}

std::string default_bindings_path() {
    return {};  // the pad's layout is fixed on the device
}

}  // namespace dream::render::vk
