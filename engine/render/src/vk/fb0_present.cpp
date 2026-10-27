// See fb0_present.h.
#include "dream/render/vk/fb0_present.h"

#include "dream/render/vk/context.h"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#endif

namespace dream::render::vk {

#if defined(__linux__)

namespace {
// Pages 16-20 of fb0's 21; NextUI draws in the first ones.
constexpr std::uint32_t kFirstPage = 16, kPages = 5;
// Allwinner's dma-buf export of the framebuffer's memory.
struct FbDmabuf {
    std::uint32_t fd, flags;
};
constexpr unsigned long kFbGetDmabuf = _IOR('F', 0x21, FbDmabuf);
}  // namespace

struct Fb0Presenter::State {
    VkDevice device = VK_NULL_HANDLE;
    int fd = -1;
    fb_var_screeninfo as_found{};  // NextUI's page, shown again at the end
    std::uint32_t line = 0;
    VkDeviceSize page_bytes = 0;
    VkBuffer buffer = VK_NULL_HANDLE;  // the pages, as memory the GPU copies into
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // The pages as linear images on that memory, where the device blits into them (can_blit).
    VkImage page_image[kPages] = {};
    bool page_used[kPages] = {};  // written before: its layout is TRANSFER_DST_OPTIMAL
    bool direct = false;
    std::uint32_t next = 0;  // the next of kPages to write

    // The present thread: frames in submission order, each shown once its fence has signalled.
    struct Frame {
        std::uint32_t slot;
        VkFence fence;
        std::uint32_t page;
    };
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Frame> queued;
    std::uint64_t busy_slots = 0;  // bit per slot with a frame not yet shown
    bool stop = false;

    void run() {
        for (;;) {
            Frame f{};
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return stop || !queued.empty(); });
                if (queued.empty())
                    return;  // stopping, and every frame shown
                f = queued.front();
                queued.pop_front();
            }
            vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
            pan(f.page);
            {
                std::lock_guard<std::mutex> lock(mutex);
                busy_slots &= ~(std::uint64_t{1} << f.slot);
            }
            cv.notify_all();
        }
    }

    void pan(std::uint32_t page) {
        fb_var_screeninfo v = as_found;
        v.xoffset = 0;
        v.yoffset = page * v.yres;
        if (ioctl(fd, FBIOPAN_DISPLAY, &v)) {
            static bool told = false;
            if (!told) {
                told = true;
                std::fprintf(stderr, "window: FBIOPAN_DISPLAY failed (%s)\n", std::strerror(errno));
            }
        }
    }
};

std::unique_ptr<Fb0Presenter> Fb0Presenter::create(Context& ctx, std::string& error) {
    if (!ctx.caps().dma_buf) {
        error = "the device has no dma-buf import (VK_EXT_external_memory_dma_buf)";
        return nullptr;
    }
    const int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        error = std::string("/dev/fb0: ") + std::strerror(errno);
        return nullptr;
    }
    std::unique_ptr<Fb0Presenter> p(new Fb0Presenter);
    p->s_ = std::make_unique<State>();
    State& s = *p->s_;
    s.device = ctx.device();
    s.fd = fd;
    fb_fix_screeninfo fix{};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &s.as_found) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
        error = "fb0: its geometry";
        return nullptr;
    }
    const fb_var_screeninfo& v = s.as_found;
    if (v.bits_per_pixel != 32 || v.red.offset != 16 || v.green.offset != 8 || v.blue.offset != 0) {
        error = "fb0 is not BGRA8";
        return nullptr;
    }
    s.line = fix.line_length;
    s.page_bytes = static_cast<VkDeviceSize>(s.line) * v.yres;
    const VkDeviceSize size = s.page_bytes * (kFirstPage + kPages);
    if (s.line < v.xres * 4 || size > fix.smem_len || v.yres_virtual < v.yres * (kFirstPage + kPages)) {
        error = "fb0 has too few pages";
        return nullptr;
    }
    FbDmabuf ex{};
    if (ioctl(fd, kFbGetDmabuf, &ex)) {
        error = "fb0 has no dma-buf export";
        return nullptr;
    }
    const int dmabuf = static_cast<int>(ex.fd);

    auto fd_props = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(s.device, "vkGetMemoryFdPropertiesKHR"));
    VkMemoryFdPropertiesKHR props{};
    props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (!fd_props ||
        fd_props(s.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf, &props) != VK_SUCCESS ||
        !props.memoryTypeBits) {
        close(dmabuf);
        error = "fb0's dma-buf: no memory type";
        return nullptr;
    }
    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &ext;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // and read back
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s.device, &bci, nullptr, &s.buffer) != VK_SUCCESS) {
        close(dmabuf);
        error = "fb0: vkCreateBuffer";
        return nullptr;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(s.device, s.buffer, &req);
    const std::uint32_t bits = req.memoryTypeBits & props.memoryTypeBits;
    std::uint32_t type = 0;
    while (type < 32 && !(bits >> type & 1u)) ++type;
    VkImportMemoryFdInfoKHR import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = dmabuf;  // owned by the memory once imported
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &import;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (!bits || vkAllocateMemory(s.device, &mai, nullptr, &s.memory) != VK_SUCCESS) {
        close(dmabuf);
        error = "fb0: the dma-buf's import";
        return nullptr;
    }
    vkBindBufferMemory(s.device, s.buffer, s.memory, 0);

    // Each page as a linear BGRA8 image at its offset in that memory, rows fb0's line apart, if the
    // device blits into linear images; otherwise a panel-sized frame image is copied in.
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(ctx.physical_device(), VK_FORMAT_B8G8R8A8_UNORM, &fp);
    if (fp.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) {
        bool ok = true;
        for (std::uint32_t i = 0; i < kPages && ok; ++i) {
            VkExternalMemoryImageCreateInfo eic{};
            eic.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            eic.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.pNext = &eic;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_B8G8R8A8_UNORM;
            ici.extent = {v.xres, v.yres, 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_LINEAR;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(s.device, &ici, nullptr, &s.page_image[i]) != VK_SUCCESS) {
                s.page_image[i] = VK_NULL_HANDLE;
                ok = false;
                break;
            }
            VkMemoryRequirements ir{};
            vkGetImageMemoryRequirements(s.device, s.page_image[i], &ir);
            const VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
            VkSubresourceLayout sl{};
            vkGetImageSubresourceLayout(s.device, s.page_image[i], &sub, &sl);
            const VkDeviceSize at = static_cast<VkDeviceSize>(kFirstPage + i) * s.page_bytes;
            ok = sl.rowPitch == s.line && sl.offset == 0 && (ir.memoryTypeBits >> type & 1u) && ir.alignment &&
                 at % ir.alignment == 0 && at + ir.size <= req.size &&
                 vkBindImageMemory(s.device, s.page_image[i], s.memory, at) == VK_SUCCESS;
        }
        if (ok) {
            s.direct = true;
        } else {
            for (VkImage& im : s.page_image) {
                if (im)
                    vkDestroyImage(s.device, im, nullptr);
                im = VK_NULL_HANDLE;
            }
        }
    }

    p->extent_ = {v.xres, v.yres};
    const double line_px = v.xres + v.left_margin + v.right_margin + v.hsync_len;
    const double rows = v.yres + v.upper_margin + v.lower_margin + v.vsync_len;
    const double hz = v.pixclock ? 1e12 / (static_cast<double>(v.pixclock) * line_px * rows) : 60.0;
    p->refresh_hz_ = hz >= 20 && hz <= 250 ? static_cast<float>(hz) : 60.0f;
    s.thread = std::thread([&s] { s.run(); });
    return p;
}

Fb0Presenter::~Fb0Presenter() {
    if (!s_)
        return;
    if (s_->thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(s_->mutex);
            s_->stop = true;
        }
        s_->cv.notify_all();
        s_->thread.join();
    }
    for (VkImage im : s_->page_image)
        if (im)
            vkDestroyImage(s_->device, im, nullptr);
    if (s_->buffer)
        vkDestroyBuffer(s_->device, s_->buffer, nullptr);
    if (s_->memory)
        vkFreeMemory(s_->device, s_->memory, nullptr);
    if (s_->fd >= 0) {
        if (s_->memory) {
            fb_var_screeninfo v = s_->as_found;
            ioctl(s_->fd, FBIOPAN_DISPLAY, &v);  // NextUI's page again
        }
        close(s_->fd);
    }
}

bool Fb0Presenter::can_blit() const noexcept {
    return s_->direct;
}

void Fb0Presenter::record_blit(VkCommandBuffer cmd, VkImage image, VkExtent2D size, VkFilter filter,
                               std::uint32_t& page) {
    State& s = *s_;
    const std::uint32_t i = s.next;
    page = kFirstPage + i;
    s.next = (s.next + 1) % kPages;
    // The frame's shape kept (a 4:3 frame on a wider panel between black bars), centred.
    const double fit = std::min(static_cast<double>(extent_.width) / size.width,
                                static_cast<double>(extent_.height) / size.height);
    const auto w = static_cast<std::int32_t>(size.width * fit + 0.5), h = static_cast<std::int32_t>(size.height * fit + 0.5);
    const std::int32_t x = (static_cast<std::int32_t>(extent_.width) - w) / 2,
                       y = (static_cast<std::int32_t>(extent_.height) - h) / 2;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = s.page_used[i] ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.page_image[i];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    if (w < static_cast<std::int32_t>(extent_.width) || h < static_cast<std::int32_t>(extent_.height)) {
        VkClearColorValue black{};
        black.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, s.page_image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &b.subresourceRange);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(size.width), static_cast<std::int32_t>(size.height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.page_image[i],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
    s.page_used[i] = true;
    // In memory when the frame's fence signals: the display engine reads it, not the GPU.
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

void Fb0Presenter::record_read(VkCommandBuffer cmd, std::uint32_t page, VkBuffer dst) {
    const VkBufferCopy region{static_cast<VkDeviceSize>(page) * s_->page_bytes, 0, s_->page_bytes};
    vkCmdCopyBuffer(cmd, s_->buffer, dst, 1, &region);
}

std::uint32_t Fb0Presenter::line() const noexcept {
    return s_->line;
}

void Fb0Presenter::record_copy(VkCommandBuffer cmd, VkImage image, std::uint32_t& page) {
    State& s = *s_;
    if (s.direct) {  // the frame image's copy as a blit of the same size: no buffer in between
        record_blit(cmd, image, extent_, VK_FILTER_NEAREST, page);
        return;
    }
    page = kFirstPage + s.next;
    s.next = (s.next + 1) % kPages;
    VkBufferImageCopy copy{};
    copy.bufferOffset = static_cast<VkDeviceSize>(page) * s.page_bytes;
    copy.bufferRowLength = s.line / 4;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &copy);
    // In memory when the frame's fence signals: the display engine reads it, not the GPU.
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

void Fb0Presenter::queue(std::uint32_t slot, VkFence fence, std::uint32_t page) {
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->queued.push_back({slot, fence, page});
        s_->busy_slots |= std::uint64_t{1} << slot;
    }
    s_->cv.notify_all();
}

void Fb0Presenter::wait_slot(std::uint32_t slot) {
    std::unique_lock<std::mutex> lock(s_->mutex);
    s_->cv.wait(lock, [&] { return !(s_->busy_slots >> slot & 1u); });
}

#else

struct Fb0Presenter::State {};

std::unique_ptr<Fb0Presenter> Fb0Presenter::create(Context&, std::string& error) {
    error = "no framebuffer presentation on this platform";
    return nullptr;
}
Fb0Presenter::~Fb0Presenter() = default;
void Fb0Presenter::record_copy(VkCommandBuffer, VkImage, std::uint32_t& page) { page = 0; }
bool Fb0Presenter::can_blit() const noexcept { return false; }
void Fb0Presenter::record_blit(VkCommandBuffer, VkImage, VkExtent2D, VkFilter, std::uint32_t& page) { page = 0; }
void Fb0Presenter::record_read(VkCommandBuffer, std::uint32_t, VkBuffer) {}
std::uint32_t Fb0Presenter::line() const noexcept { return 0; }
void Fb0Presenter::queue(std::uint32_t, VkFence, std::uint32_t) {}
void Fb0Presenter::wait_slot(std::uint32_t) {}

#endif

}  // namespace dream::render::vk
