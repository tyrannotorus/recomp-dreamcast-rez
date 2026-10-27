// Windows presentation through a DXGI flip-model swapchain.
//
// Some Vulkan drivers on Windows (Intel's) present every swapchain, windowed
// or borderless fullscreen, through a compositor copy ("Composed: Copy with GPU GDI" in PresentMon),
// about one refresh slower than a flip. A D3D11 flip-model swapchain on the same window gets
// "Hardware Composed: Independent Flip". So Vulkan keeps rendering, into textures shared with a
// D3D11 device on the same GPU, and D3D11 copies the finished image into its back buffer and
// presents. Access to each shared texture alternates through its keyed mutex: Vulkan takes key 0
// and hands over key 1 when it submits, D3D11 takes key 1 and hands back key 0 after the copy.
//
// Window owns one and uses it in place of a VkSwapchainKHR when it initialises; nothing outside
// Window sees the difference. On other platforms create() always fails.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class Context;

class DxgiPresenter {
public:
    // Everything that does not depend on the window's size: the D3D11 device on the adapter
    // `ctx` renders on, and the swapchain on `hwnd` (an HWND). Null with `error` set when any of it
    // is missing; the caller then presents through Vulkan.
    static std::unique_ptr<DxgiPresenter> create(Context& ctx, void* hwnd, std::string& error);
    ~DxgiPresenter();

    // (Re)builds the back buffers and the shared images at a size. Images are VK_FORMAT_B8G8R8A8_UNORM,
    // usable as colour attachments and transfer sources, starting in VK_IMAGE_LAYOUT_UNDEFINED.
    bool resize(std::uint32_t width, std::uint32_t height, std::string& error);
    void release_images();
    const std::vector<VkImage>& images() const noexcept { return images_; }

    // Chained into the VkSubmitInfo that renders image `i`: takes key 0, hands over key 1.
    const void* submit_chain(std::uint32_t i);
    // The same for a read of image `i` that leaves it with D3D11's key (screenshots).
    const void* read_chain(std::uint32_t i);
    // How a present reaches the display.
    //   Queued:  in order (sync interval 1), with the queue kept short: when more frames wait
    //            than the current depth allows (frame statistics), this one is skipped and the
    //            panel shows the previous one once more, instead of queueing behind it -- a late
    //            frame costs one repeat rather than a refresh of latency from then on. The depth
    //            adapts: one frame waiting while frames arrive on time, one more (up to three)
    //            whenever skips come often (a slow or battery-powered machine, where emulating a
    //            frame can take longer than a refresh), retrying one less after 10 s, backing off
    //            to 4 min while that keeps failing. The default; the caller paces.
    //   Tearing: at once, tearing where the system allows it (--present-mode immediate).
    enum class Mode { Queued, Tearing };
    // Frames currently allowed to wait for the display (1-3; see Mode::Queued).
    int queue_depth() const noexcept;
    // Copies image `i` into the back buffer and presents.
    bool present(std::uint32_t i, Mode mode);

    std::string description() const;

private:
    DxgiPresenter() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<VkImage> images_;
};

}  // namespace dream::render::vk
