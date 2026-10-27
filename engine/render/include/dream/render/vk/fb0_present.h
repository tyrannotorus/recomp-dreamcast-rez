// Presentation to a NextUI handheld's framebuffer, used by window_nextui.cpp in place of a
// swapchain (the TrimUI Brick's VK_KHR_display swapchain tears). /dev/fb0 (BGRA8, 21 pages,
// NextUI's in the first ones) is imported into Vulkan as a dma-buf (Allwinner's FBIOGET_DMABUF);
// each frame goes into one of pages 16-20 and is shown with FBIOPAN_DISPLAY, which returns at the
// vblank that shows it. NextUI's page is shown again at the end. Without fb0, create() fails.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class Context;

class Fb0Presenter {
public:
    // Opens /dev/fb0 and imports its memory. Null with `error` set when any of it is missing.
    static std::unique_ptr<Fb0Presenter> create(Context& ctx, std::string& error);
    ~Fb0Presenter();

    VkExtent2D extent() const noexcept { return extent_; }
    float refresh_hz() const noexcept { return refresh_hz_; }

    // Records, into `cmd`, the copy of `image` (the panel's size, B8G8R8A8, in
    // TRANSFER_SRC_OPTIMAL) into the next page; `page` says which, for queue().
    void record_copy(VkCommandBuffer cmd, VkImage image, std::uint32_t& page);
    // Each page is also a linear image on fb0's memory, when the device can blit into one: a frame
    // of any size and colour format goes straight into its page in one pass. `image` is in
    // TRANSFER_SRC_OPTIMAL.
    bool can_blit() const noexcept;
    void record_blit(VkCommandBuffer cmd, VkImage image, VkExtent2D size, VkFilter filter, std::uint32_t& page);
    // Records the copy of `page` (as shown: BGRA8, rows line() bytes apart) into `dst`, for a
    // screenshot of what is on the panel.
    void record_read(VkCommandBuffer cmd, std::uint32_t page, VkBuffer dst);
    std::uint32_t line() const noexcept;
    // Hands a submitted frame to the present thread: once `fence` signals it pans to `page`, which
    // returns at the vblank that shows it. The caller never waits for the GPU or the panel here.
    void queue(std::uint32_t slot, VkFence fence, std::uint32_t page);
    // Blocks until the frame last queued for `slot` has been shown, so its fence and image can be
    // used again.
    void wait_slot(std::uint32_t slot);

private:
    Fb0Presenter() = default;
    struct State;
    std::unique_ptr<State> s_;
    VkExtent2D extent_{};
    float refresh_hz_ = 60.0f;
};

}  // namespace dream::render::vk
