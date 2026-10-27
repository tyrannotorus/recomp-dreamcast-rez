// Windows presentation through a DXGI flip-model swapchain; see dxgi_present.h.
#include "dream/render/vk/dxgi_present.h"

#include "dream/render/vk/context.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstring>
#include <cwchar>
#endif

namespace dream::render::vk {

#ifdef _WIN32

namespace {

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kVkFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr std::uint32_t kImages = 2;  // one per frame in flight (Window's kFramesInFlight)

}  // namespace

struct DxgiPresenter::Impl {
    Context* ctx = nullptr;
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain2* swapchain = nullptr;
    HANDLE waitable = nullptr;
    UINT swap_flags = 0;
    bool tearing = false;
    std::string adapter;
    PFN_vkGetMemoryWin32HandlePropertiesKHR get_handle_props = nullptr;

    struct Shared {
        ID3D11Texture2D* texture = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
        HANDLE handle = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    std::vector<Shared> shared;
    // Keyed-mutex chains, one per image, kept alive for the submit that uses them.
    std::uint64_t key0 = 0, key1 = 1;
    std::uint32_t timeout_ms = 1000;
    VkWin32KeyedMutexAcquireReleaseInfoKHR render_km[kImages]{}, read_km[kImages]{};
    // Adaptive queue depth (see Mode::Queued).
    int depth = 1;  // frames allowed to wait
    std::deque<std::chrono::steady_clock::time_point> recent_skips;
    std::chrono::steady_clock::time_point retry_at{}, shallow_since{};
    std::chrono::seconds backoff{10};

    void free_shared() {
        VkDevice dev = ctx->device();
        for (auto& s : shared) {
            if (s.image)
                vkDestroyImage(dev, s.image, nullptr);
            if (s.memory)
                vkFreeMemory(dev, s.memory, nullptr);
            if (s.handle)
                CloseHandle(s.handle);
            release(s.mutex);
            release(s.texture);
        }
        shared.clear();
    }
};

std::unique_ptr<DxgiPresenter> DxgiPresenter::create(Context& ctx, void* hwnd, std::string& error) {
    if (!ctx.caps().win32_interop) {
        error = "the Vulkan device lacks VK_KHR_external_memory_win32 / VK_KHR_win32_keyed_mutex";
        return nullptr;
    }
    std::unique_ptr<DxgiPresenter> self(new DxgiPresenter);
    self->impl_ = std::make_unique<Impl>();
    Impl& m = *self->impl_;
    m.ctx = &ctx;
    m.hwnd = static_cast<HWND>(hwnd);
    m.get_handle_props = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vkGetDeviceProcAddr(ctx.device(), "vkGetMemoryWin32HandlePropertiesKHR"));
    if (!m.get_handle_props) {
        error = "vkGetMemoryWin32HandlePropertiesKHR missing";
        return nullptr;
    }

    // The D3D11 device must be on the adapter Vulkan renders with, or the textures cannot be
    // shared: matched by LUID.
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &id;
    vkGetPhysicalDeviceProperties2(ctx.physical_device(), &props);
    if (!id.deviceLUIDValid) {
        error = "the Vulkan device has no LUID";
        return nullptr;
    }
    LUID luid;
    std::memcpy(&luid, id.deviceLUID, sizeof luid);

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        error = hr_text("CreateDXGIFactory2", hr);
        return nullptr;
    }
    IDXGIAdapter1* adapter = nullptr;
    hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    if (FAILED(hr)) {
        release(factory);
        error = hr_text("EnumAdapterByLuid", hr);
        return nullptr;
    }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name - 1, nullptr, nullptr);
    m.adapter = name;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           levels, 2, D3D11_SDK_VERSION, &m.device, nullptr, &m.context);
    release(adapter);
    if (FAILED(hr)) {
        release(factory);
        error = hr_text("D3D11CreateDevice", hr);
        return nullptr;
    }

    // Tearing (for --present-mode immediate) where the system offers it.
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            m.tearing = allow == TRUE;
        release(f5);
    }

    RECT rc{};
    GetClientRect(m.hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    d.Height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    d.Format = kFormat;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // One shown, up to three waiting (the adaptive depth), one being written: Present never waits.
    d.BufferCount = 5;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    m.swap_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                   (m.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    d.Flags = m.swap_flags;
    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(m.device, m.hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        release(factory);
        error = hr_text("CreateSwapChainForHwnd", hr);
        return nullptr;
    }
    // The window handles Alt+Enter itself (borderless fullscreen).
    factory->MakeWindowAssociation(m.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    release(factory);
    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&m.swapchain));
    release(sc1);
    if (FAILED(hr)) {
        error = hr_text("IDXGISwapChain2", hr);
        return nullptr;
    }
    // Never waited on: blocking the game thread on the latency object delays emulation, and in
    // this mode it is signalled well after the flip. present() reads the queue from the frame
    // statistics instead; the limit only has to stay above what that allows.
    m.swapchain->SetMaximumFrameLatency(5);
    m.waitable = m.swapchain->GetFrameLatencyWaitableObject();
    return self;
}

DxgiPresenter::~DxgiPresenter() {
    if (!impl_)
        return;
    Impl& m = *impl_;
    release_images();
    if (m.waitable)
        CloseHandle(m.waitable);
    if (m.context) {
        m.context->ClearState();
        m.context->Flush();
    }
    release(m.swapchain);
    release(m.context);
    release(m.device);
}

void DxgiPresenter::release_images() {
    if (!impl_)
        return;
    impl_->free_shared();
    images_.clear();
}

bool DxgiPresenter::resize(std::uint32_t width, std::uint32_t height, std::string& error) {
    Impl& m = *impl_;
    release_images();
    m.context->ClearState();
    m.context->Flush();
    HRESULT hr = m.swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m.swap_flags);
    if (FAILED(hr)) {
        error = hr_text("ResizeBuffers", hr);
        return false;
    }
    VkDevice dev = m.ctx->device();
    m.shared.resize(kImages);
    for (std::uint32_t i = 0; i < kImages; ++i) {
        auto& s = m.shared[i];
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = kFormat;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = m.device->CreateTexture2D(&td, nullptr, &s.texture);
        if (FAILED(hr)) {
            error = hr_text("CreateTexture2D (shared)", hr);
            return false;
        }
        s.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&s.mutex));
        IDXGIResource1* res = nullptr;
        s.texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res));
        hr = res ? res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &s.handle)
                 : E_NOINTERFACE;
        release(res);
        if (FAILED(hr) || !s.mutex) {
            error = hr_text("CreateSharedHandle", hr);
            return false;
        }

        // The Vulkan side: an image over the same memory.
        VkExternalMemoryImageCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &ext;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kVkFormat;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ici, nullptr, &s.image) != VK_SUCCESS) {
            error = "vkCreateImage (shared)";
            return false;
        }
        VkMemoryWin32HandlePropertiesKHR hp{};
        hp.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
        if (m.get_handle_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, s.handle, &hp) !=
            VK_SUCCESS) {
            error = "vkGetMemoryWin32HandlePropertiesKHR";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, s.image, &req);
        const std::uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits;
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(m.ctx->physical_device(), &mp);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = t;
        for (std::uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; ++t)
            if (bits & (1u << t))
                type = t;
        if (type == UINT32_MAX) {
            error = "no memory type can import the shared texture";
            return false;
        }
        VkMemoryDedicatedAllocateInfo ded{};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.image = s.image;
        VkImportMemoryWin32HandleInfoKHR imp{};
        imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        imp.pNext = &ded;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle = s.handle;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &imp;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev, &mai, nullptr, &s.memory) != VK_SUCCESS ||
            vkBindImageMemory(dev, s.image, s.memory, 0) != VK_SUCCESS) {
            error = "importing the shared texture into Vulkan";
            return false;
        }

        auto& r = m.render_km[i];
        r = {};
        r.sType = VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR;
        r.acquireCount = 1;
        r.pAcquireSyncs = &s.memory;
        r.pAcquireKeys = &m.key0;
        r.pAcquireTimeouts = &m.timeout_ms;
        r.releaseCount = 1;
        r.pReleaseSyncs = &s.memory;
        r.pReleaseKeys = &m.key1;
        auto& rd = m.read_km[i];
        rd = r;
        rd.pReleaseKeys = &m.key0;
        images_.push_back(s.image);
    }
    return true;
}

const void* DxgiPresenter::submit_chain(std::uint32_t i) {
    return i < kImages ? &impl_->render_km[i] : nullptr;
}

const void* DxgiPresenter::read_chain(std::uint32_t i) {
    return i < kImages ? &impl_->read_km[i] : nullptr;
}

bool DxgiPresenter::present(std::uint32_t i, Mode mode) {
    Impl& m = *impl_;
    if (i >= m.shared.size())
        return false;
    auto& s = m.shared[i];
    using clock = std::chrono::steady_clock;
    // Queued: how many presents are still waiting for the screen?
    int pending = -1;
    {
        DXGI_FRAME_STATISTICS fs{};
        UINT last = 0;
        if (SUCCEEDED(m.swapchain->GetFrameStatistics(&fs)) &&
            SUCCEEDED(m.swapchain->GetLastPresentCount(&last)))
            pending = static_cast<int>(last - fs.PresentCount);
    }
    // Frame statistics count one more than are really waiting on this path (a present is still
    // pending when the next one comes), so `depth` frames waiting reads as depth + 1.
    const auto now_c = clock::now();
    if (m.depth > 1 && now_c >= m.retry_at) {
        --m.depth;  // try one less again
        m.recent_skips.clear();
        m.shallow_since = now_c;
        m.retry_at = now_c + m.backoff;
    }
    const bool skip = mode == Mode::Queued && pending >= m.depth + 1;
    if (skip) {
        m.recent_skips.push_back(now_c);
        while (!m.recent_skips.empty() && now_c - m.recent_skips.front() > std::chrono::seconds(30))
            m.recent_skips.pop_front();
        if (m.depth < 3 && m.recent_skips.size() >= 2) {
            // Frames keep arriving late: let one more wait rather than repeat one every few seconds.
            // A retry that held for two minutes resets the back-off.
            if (m.shallow_since.time_since_epoch().count() != 0 &&
                now_c - m.shallow_since > std::chrono::minutes(2))
                m.backoff = std::chrono::seconds(10);
            ++m.depth;
            m.recent_skips.clear();
            m.retry_at = now_c + m.backoff;
            m.backoff = std::min<std::chrono::seconds>(m.backoff * 2, std::chrono::minutes(4));
        }
    }
    // Waits (on the GPU's timeline, through the kernel) for Vulkan's release of key 1.
    const HRESULT acq = s.mutex->AcquireSync(1, m.timeout_ms);
    if (acq != S_OK) {
        std::fprintf(stderr, "dxgi: shared image %u not handed over (0x%08lx); frame skipped\n", i,
                     static_cast<unsigned long>(acq));
        return false;
    }
    if (skip) {
        // Hand the image back to Vulkan untouched.
        s.mutex->ReleaseSync(0);
        return true;
    }
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(m.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
        m.context->CopyResource(back, s.texture);
        release(back);
    }
    s.mutex->ReleaseSync(0);
    // Sync interval 1 queues (and blocks the caller once the latency limit is reached); tearing
    // presents at once.
    const UINT interval = mode == Mode::Queued ? 1u : 0u;
    const UINT flags = (mode == Mode::Tearing && m.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT hr = m.swapchain->Present(interval, flags);
    return SUCCEEDED(hr);
}

int DxgiPresenter::queue_depth() const noexcept { return impl_ ? impl_->depth : 0; }

std::string DxgiPresenter::description() const {
    return "DXGI flip model (D3D11 on " + impl_->adapter + (impl_->tearing ? ", tearing allowed)" : ")");
}

#else  // !_WIN32

struct DxgiPresenter::Impl {};

std::unique_ptr<DxgiPresenter> DxgiPresenter::create(Context&, void*, std::string& error) {
    error = "DXGI exists only on Windows";
    return nullptr;
}
DxgiPresenter::~DxgiPresenter() = default;
void DxgiPresenter::release_images() {}
bool DxgiPresenter::resize(std::uint32_t, std::uint32_t, std::string&) { return false; }
const void* DxgiPresenter::submit_chain(std::uint32_t) { return nullptr; }
const void* DxgiPresenter::read_chain(std::uint32_t) { return nullptr; }
bool DxgiPresenter::present(std::uint32_t, Mode) { return false; }
std::string DxgiPresenter::description() const { return {}; }
int DxgiPresenter::queue_depth() const noexcept { return 0; }

#endif

}  // namespace dream::render::vk
