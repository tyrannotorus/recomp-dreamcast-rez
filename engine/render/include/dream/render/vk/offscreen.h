// Rendering a frame at the guest's own resolution.
//
// The renderer draws into an image of its own rather than straight into the window. Two reasons,
// and both matter more than the extra copy costs:
//
//   * The guest expects the frame to end up in video memory, at the address FB_W_SOF1 names. A
//     title that renders geometry and then writes pixels over it, or samples what it just drew as
//     a texture, only works when the rendered frame is really there. That needs the pixels back on
//     the host, which needs somewhere to read them from.
//   * Render size and window size stop being the same number. Drawing at two or four times the
//     guest's resolution is then a change to one argument rather than a change to the design.
//
// The readback is synchronous: record, submit, wait. A frame is a megabyte or so and the guest is
// blocked on the render anyway, which is exactly what the hardware makes it do.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "dream/render/vk/context.h"
#include "dream/render/vk/renderer.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class Offscreen {
public:
    Offscreen() = default;
    ~Offscreen();
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;

    // Colour and depth attachments at `width` by `height`, a render pass that leaves the colour
    // image ready to copy, and the command buffer and fence used to submit one frame.
    bool create(Context& ctx, std::uint32_t width, std::uint32_t height);
    void destroy();

    // The render pass the renderer's pipelines are built against. Valid after create().
    VkRenderPass render_pass() const noexcept { return render_pass_; }
    VkExtent2D extent() const noexcept { return extent_; }
    std::uint32_t width() const noexcept { return extent_.width; }
    std::uint32_t height() const noexcept { return extent_.height; }

    // Draws one decoded frame and waits for it to finish. Afterwards pixels() holds the result.
    bool render(Renderer& renderer, const Frame& frame, const FrameGeometry& geometry);

    // The last rendered frame as RGBA8888, width() * height() pixels, row 0 at the top. Null
    // before the first successful render().
    // The rendered frame on the host. With `readback` off (the direct present) the copy
    // is made here, on the first call after a render, so anything that needs the pixels --
    // screenshots, F11, overlays, framebuffer write-back -- still gets them, and a frame nobody
    // asks for never leaves the GPU.
    const std::uint32_t* pixels() noexcept;
    // false: render() leaves the colour image ready to sample (sampled_view()) and skips the copy.
    bool readback = true;
    VkImageView sampled_view() const noexcept { return colour_.view(); }
    // The same image, for blitting into a NextUI panel's page; in
    // SHADER_READ_ONLY_OPTIMAL after a render with `readback` off.
    VkImage colour_image() const noexcept { return colour_.image(); }
    // Called inside render() right after the render pass, with the command buffer, the
    // colour image (a transfer source then) and its size: where a render to texture copies its
    // result out (TextureCache::capture_rtt).
    std::function<void(VkCommandBuffer cmd, VkImage colour, VkExtent2D extent)> after_pass;
    // The part of the target the next render() draws, clears and stores, from its top-left corner
    // (a render to texture's region). Zero: all of it.
    VkExtent2D area{};
    bool has_frame() const noexcept { return frames > 0; }

    const std::string& error() const noexcept { return error_; }
    std::uint64_t frames = 0;

private:
    Context* ctx_ = nullptr;
    VkExtent2D extent_{};
    AttachmentImage colour_, depth_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    // Two frames in flight: each render records into the slot the previous one did not
    // use, so the CPU records frame N+1 while the GPU still draws frame N. The colour image is
    // shared; the barrier at the start of each render orders the GPU's work on it.
    static constexpr unsigned kSlots = 2;
    unsigned slot_ = 0;  // the slot of the last submitted render
    VkCommandBuffer cmd_[kSlots]{};
    VkFence fence_[kSlots]{};
    HostBuffer readback_;
    bool have_pixels_ = false;
    bool copy_to_host();
    // Pipelining: render() returns once the frame is submitted; the CPU waits for it
    // only when it next needs the command buffer or the pixels.
    bool pending_[kSlots]{};
    bool host_frame_ = false;  // the last render already copied the frame to the host
    bool wait_slot(unsigned s);
public:
    // Blocks until every submitted render has finished on the GPU. False on a device timeout.
    bool wait();
    std::string error_;
};

}  // namespace dream::render::vk
