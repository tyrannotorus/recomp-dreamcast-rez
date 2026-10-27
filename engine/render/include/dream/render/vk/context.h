// Vulkan device setup. Deliberately small and explicit rather than a port of Flycast's
// context: this is the layer where the three host platforms differ, and the differences are worth
// reading. macOS has no native Vulkan, so the loader talks to MoltenVK, which is a "portability"
// driver: the instance must opt in through VK_KHR_portability_enumeration or the loader will not
// enumerate it at all, and the device must enable VK_KHR_portability_subset.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace dream::render::vk {

// What the renderer can do on this machine. Feature support is a property of the GPU, not of the
// operating system: the per-pixel transparency path needs fragment stores and atomics, which a
// weak or old device may lack, and then the per-strip fallback is used instead.
struct DeviceCapabilities {
    bool fragment_stores_and_atomics = false;  // per-pixel order-independent transparency
    bool fragment_shader_interlock = false;    // ordered per-pixel access without a sort pass
    bool independent_blend = false;
    bool sampler_anisotropy = false;
    // VK_KHR_external_memory_win32 + VK_KHR_win32_keyed_mutex enabled: images can be shared with
    // D3D11 (DxgiPresenter).
    bool win32_interop = false;
    // dma-buf memory can be imported (VK_EXT_external_memory_dma_buf), enabled in builds
    // that present to a NextUI handheld's framebuffer (fb0_present.h, DREAM_RENDER_FB0).
    bool dma_buf = false;
    std::uint32_t max_texture_size = 0;
    std::string device_name;
};

class Context {
public:
    Context() = default;
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Creates the instance, picks a device, and creates the logical device and queue.
    // `surface_extensions` come from the window (SDL3 supplies them); an empty list makes a
    // context with no surface, as a NextUI handheld's window uses. Returns false and fills
    // error() rather than throwing: no usable GPU is an ordinary outcome on a build machine.
    bool create(const std::vector<const char*>& surface_extensions);
    void destroy();

    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physical_device() const noexcept { return physical_; }
    VkDevice device() const noexcept { return device_; }
    VkQueue queue() const noexcept { return queue_; }
    std::uint32_t queue_family() const noexcept { return queue_family_; }
    const DeviceCapabilities& caps() const noexcept { return caps_; }
    const std::string& error() const noexcept { return error_; }

private:
    bool pick_device();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    DeviceCapabilities caps_;
    std::string error_;
};

}  // namespace dream::render::vk
