// See context.h.
#include "dream/render/vk/context.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace dream::render::vk {

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

}  // namespace

Context::~Context() {
    destroy();
}

bool Context::create(const std::vector<const char*>& surface_extensions) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Rez Recompiled";
    app.apiVersion = VK_API_VERSION_1_1;

    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    std::vector<const char*> extensions = surface_extensions;
    VkInstanceCreateFlags flags = 0;
    // MoltenVK is a portability driver: a Vulkan loader from 1.3.216 on will not enumerate it
    // unless the application says it accepts portability drivers.
    if (has_extension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.flags = flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        error_ = "vkCreateInstance failed (no Vulkan driver?)";
        return false;
    }

    if (!pick_device()) {
        destroy();
        return false;
    }
    return true;
}

bool Context::pick_device() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        error_ = "no Vulkan device";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Prefer a device that can do per-pixel transparency, then a discrete one.
    int best_score = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(d, &features);
        int score = 0;
        if (features.fragmentStoresAndAtomics)
            score += 4;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 2;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score += 1;
        if (score > best_score) {
            best_score = score;
            physical_ = d;
        }
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical_, &features);
    caps_.device_name = props.deviceName;
    caps_.max_texture_size = props.limits.maxImageDimension2D;
    caps_.fragment_stores_and_atomics = features.fragmentStoresAndAtomics;
    caps_.independent_blend = features.independentBlend;
    caps_.sampler_anisotropy = features.samplerAnisotropy;

    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> device_exts(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count, device_exts.data());
    caps_.fragment_shader_interlock =
        has_extension(device_exts, "VK_EXT_fragment_shader_interlock");

    std::vector<const char*> enabled{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Required by the specification whenever the device is a portability driver (MoltenVK).
    if (has_extension(device_exts, "VK_KHR_portability_subset"))
        enabled.push_back("VK_KHR_portability_subset");
#ifdef DREAM_RENDER_FB0
    // importing a NextUI handheld's framebuffer memory, to present into it
    // (fb0_present.h).
    if (has_extension(device_exts, "VK_KHR_external_memory_fd") &&
        has_extension(device_exts, "VK_EXT_external_memory_dma_buf")) {
        enabled.push_back("VK_KHR_external_memory_fd");
        enabled.push_back("VK_EXT_external_memory_dma_buf");
        caps_.dma_buf = true;
    }
#endif
#ifdef _WIN32
    // sharing images with D3D11, for presenting through DXGI (dxgi_present.h).
    if (has_extension(device_exts, "VK_KHR_external_memory_win32") &&
        has_extension(device_exts, "VK_KHR_win32_keyed_mutex")) {
        enabled.push_back("VK_KHR_external_memory_win32");
        enabled.push_back("VK_KHR_win32_keyed_mutex");
        caps_.win32_interop = true;
    }
#endif

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
    bool found = false;
    for (std::uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            found = true;
            break;
        }
    }
    if (!found) {
        error_ = "no graphics queue";
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures wanted{};
    wanted.fragmentStoresAndAtomics = features.fragmentStoresAndAtomics;
    wanted.independentBlend = features.independentBlend;
    wanted.samplerAnisotropy = features.samplerAnisotropy;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    dci.pEnabledFeatures = &wanted;
    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
        error_ = "vkCreateDevice failed";
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

void Context::destroy() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physical_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

}  // namespace dream::render::vk
