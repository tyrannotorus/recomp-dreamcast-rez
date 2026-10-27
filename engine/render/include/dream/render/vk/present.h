// Putting the guest's framebuffer on screen.
//
// The window shows what the video hardware would be scanning out, which is the buffer FB_R_SOF1
// points at, decoded from video memory. Deliberately not "whatever the renderer drew last": a
// title that writes pixels into the framebuffer directly, for a video sequence or a 2D overlay, is
// then shown correctly without the renderer knowing anything about it, and double buffering works
// because the guest nominates the buffer rather than the renderer guessing.
//
// One texture, one fullscreen triangle. The image keeps the guest's aspect ratio inside whatever
// the window happens to be, so a resized window letterboxes instead of stretching.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

// How the guest picture fills the window. Process-wide so a launcher
// extension can change it between frames without reaching into the window.
struct PresentOptions {
    enum class Fit : std::uint8_t {
        Letterbox,  // whole picture visible, bars where the shapes differ (the engine's default)
        Crop,       // fill the window, cutting what overflows (no bars)
        Stretch,    // fill the window, distorting the shape
    };
    Fit fit = Fit::Letterbox;
    // Shape the picture is shown at; 0 means its own (640x480 -> 4:3). An anamorphic widescreen
    // mode renders a 16:9 view into the 4:3 framebuffer and sets 16/9 here to undo the squeeze.
    float display_aspect = 0.0f;
};

inline PresentOptions& present_options() {
    static PresentOptions o;
    return o;
}

class Presenter {
public:
    Presenter() = default;
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // `render_pass` is the window's, since that is where the triangle is drawn.
    bool create(Context& ctx, VkRenderPass render_pass);
    void destroy();

    // Uploads one decoded framebuffer. `rgba` is width * height pixels, row 0 at the top. Call
    // outside the render pass: it records a copy. A changed size reallocates the image.
    //
    // `decorate`, when given, is called with the frame already copied into the staging buffer and
    // before it is handed to the GPU, so an overlay can be composited in place. Doing it here
    // rather than in the caller saves copying the whole frame a second time to change a corner of
    // it, and leaves the caller's own buffer untouched: what it hands in is still the guest's
    // picture, which is what a screenshot wants.
    bool upload(VkCommandBuffer cmd, const std::uint32_t* rgba, std::uint32_t width,
                std::uint32_t height,
                const std::function<void(std::uint32_t* pixels, std::uint32_t width,
                                         std::uint32_t height)>& decorate = {});
    // Draws the uploaded image, fitted to `target`. Call inside the window's render pass. False
    // when nothing has been uploaded yet, which leaves the cleared background showing.
    bool draw(VkCommandBuffer cmd, VkExtent2D target);
    // Draws an image that already lives on the GPU (the offscreen render target, in
    // SHADER_READ_ONLY layout) instead of the uploaded one: no host round trip.
    // Forgets which views draw_view's sets hold. Call when an image they may show is
    // destroyed: its replacement can get the same handle value (the driver reuses the memory), and
    // the set would then go on sampling the destroyed image (after the render target is rebuilt
    // for a new aspect ratio or scale). The device must be idle.
    void forget_views() {
        for (auto& v : ext_views_)
            v = VK_NULL_HANDLE;
    }
    bool draw_view(VkCommandBuffer cmd, VkImageView view, std::uint32_t width,
                   std::uint32_t height, VkExtent2D target);

    // The sampler's own filter, which only decides how a *magnified* image is stretched: the
    // guest's pixels shown as pixels, or blurred between them. Minification does not go through
    // it -- draw() works out how many source pixels each drawn pixel covers and the fragment
    // shader averages them, so --scale gives anti-aliasing.
    bool smooth = false;
    // An overlay layer: set before create(). The image is premultiplied RGBA blended
    // over what is already drawn, and it covers the target exactly (fit and display aspect from
    // present_options() are ignored), so a UI rendered at the window's size lands 1:1.
    bool overlay_layer = false;

    const std::string& error() const noexcept { return error_; }

private:
    bool draw_set(VkCommandBuffer cmd, VkDescriptorSet set, std::uint32_t width,
                  std::uint32_t height, VkExtent2D target);
    // draw_view's sets, one per draw in a ring. A set may not be rewritten while a
    // command buffer that uses it is in flight: with one set, the in-between frame and the real
    // frame each rewrote it before the other had executed, and drivers that read descriptors at
    // execution (RADV) then showed the image the next frame was still being drawn into. The ring
    // is longer than the draws that can be in flight (two frames in flight, at most one
    // draw_view each).
    static constexpr unsigned kExtSets = 8;
    VkDescriptorSet ext_sets_[kExtSets] = {};
    VkImageView ext_views_[kExtSets] = {};
    unsigned ext_next_ = 0;
    bool ensure_image(std::uint32_t width, std::uint32_t height);
    void destroy_image();

    Context* ctx_ = nullptr;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    HostBuffer staging_;
    std::uint32_t width_ = 0, height_ = 0;
    bool uploaded_ = false;
    std::string error_;
};

}  // namespace dream::render::vk
