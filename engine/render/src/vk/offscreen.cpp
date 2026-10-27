// See offscreen.h.
#include "dream/render/vk/offscreen.h"

#include <algorithm>

namespace dream::render::vk {

Offscreen::~Offscreen() {
    destroy();
}

bool Offscreen::create(Context& ctx, std::uint32_t width, std::uint32_t height) {
    destroy();
    ctx_ = &ctx;
    extent_ = {width, height};
    if (width == 0 || height == 0) {
        error_ = "an offscreen target needs a non-empty size";
        return false;
    }

    // TRANSFER_SRC as well as COLOR_ATTACHMENT: the frame is copied back to the host after it is
    // drawn, and on some drivers an image without that usage cannot be a copy source at all.
    if (!colour_.create(ctx, VK_FORMAT_R8G8B8A8_UNORM, extent_,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        error_ = "could not create the offscreen colour attachment";
        return false;
    }
    const VkFormat depth_format = pick_depth_format(ctx.physical_device());
    if (!depth_.create(ctx, depth_format, extent_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       format_has_stencil(depth_format)
                           ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                           : VK_IMAGE_ASPECT_DEPTH_BIT)) {
        error_ = "could not create the offscreen depth attachment";
        return false;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // modifier volumes start clear
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    // The copy that follows the render pass reads the colour attachment, so the transfer stage has
    // to wait for the writes the subpass made.
    VkSubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    // Before: the attachments' clears are layout transitions from UNDEFINED, and they
    // must come after the presenter's reads of the previous frame (fragment shader), a readback
    // (transfer) and the previous frame's depth writes. A pipeline barrier before the render pass
    // does not order a transition the render pass makes; only this dependency does
    // (synchronization validation: WRITE_AFTER_READ, WRITE_AFTER_WRITE).
    VkSubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    before.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkSubpassDependency deps[2] = {before, after};

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    if (vkCreateRenderPass(ctx.device(), &rpci, nullptr, &render_pass_) != VK_SUCCESS) {
        error_ = "vkCreateRenderPass failed for the offscreen target";
        return false;
    }

    const VkImageView views[2] = {colour_.view(), depth_.view()};
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = render_pass_;
    fbci.attachmentCount = 2;
    fbci.pAttachments = views;
    fbci.width = width;
    fbci.height = height;
    fbci.layers = 1;
    if (vkCreateFramebuffer(ctx.device(), &fbci, nullptr, &framebuffer_) != VK_SUCCESS) {
        error_ = "vkCreateFramebuffer failed for the offscreen target";
        return false;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = ctx.queue_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(ctx.device(), &cpci, nullptr, &pool_) != VK_SUCCESS) {
        error_ = "vkCreateCommandPool failed for the offscreen target";
        return false;
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kSlots;
    if (vkAllocateCommandBuffers(ctx.device(), &cbai, cmd_) != VK_SUCCESS) {
        error_ = "vkAllocateCommandBuffers failed for the offscreen target";
        return false;
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (VkFence& f : fence_)
        if (vkCreateFence(ctx.device(), &fci, nullptr, &f) != VK_SUCCESS) {
            error_ = "vkCreateFence failed for the offscreen target";
            return false;
        }
    slot_ = 0;
    if (!readback_.ensure(ctx, static_cast<VkDeviceSize>(width) * height * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        error_ = "could not allocate the readback buffer";
        return false;
    }
    return true;
}

void Offscreen::destroy() {
    if (ctx_ && ctx_->device())
        wait();
    if (ctx_ && ctx_->device()) {
        VkDevice dev = ctx_->device();
        for (VkFence f : fence_)
            if (f)
                vkDestroyFence(dev, f, nullptr);
        if (pool_)
            vkDestroyCommandPool(dev, pool_, nullptr);  // frees cmd_ with it
        if (framebuffer_)
            vkDestroyFramebuffer(dev, framebuffer_, nullptr);
        if (render_pass_)
            vkDestroyRenderPass(dev, render_pass_, nullptr);
    }
    for (unsigned s = 0; s < kSlots; ++s) {
        fence_[s] = VK_NULL_HANDLE;
        cmd_[s] = VK_NULL_HANDLE;
        pending_[s] = false;
    }
    pool_ = VK_NULL_HANDLE;
    framebuffer_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    readback_.destroy();
    colour_.destroy();
    depth_.destroy();
    have_pixels_ = false;
    ctx_ = nullptr;
}

bool Offscreen::render(Renderer& renderer, const Frame& frame, const FrameGeometry& geometry) {
    if (!ctx_ || !cmd_[0])
        return false;
    // The slot the previous render did not use: its frame, two back, must be finished
    // before its command buffer (and the renderer's per-frame buffers, a ring of four, rewritten in
    // prepare) are reused. The previous frame may still be drawing.
    const unsigned s = (slot_ + 1) % kSlots;
    if (!wait_slot(s))
        return false;
    VkCommandBuffer cmd = cmd_[s];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        error_ = "vkBeginCommandBuffer failed";
        return false;
    }

    renderer.prepare(cmd, frame);  // texture uploads happen outside the render pass

    // The presenter may still be sampling the previous frame from this image in a command buffer
    // submitted earlier on the same queue: wait for those reads before drawing over it.
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 0,
                         nullptr);

    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {0.0f, 0};  // the hardware's depth grows towards the viewer
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = render_pass_;
    rpbi.framebuffer = framebuffer_;
    const VkExtent2D drawn{area.width ? std::min(area.width, extent_.width) : extent_.width,
                           area.height ? std::min(area.height, extent_.height) : extent_.height};
    rpbi.renderArea.extent = drawn;
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{
        0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
        0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, drawn};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    renderer.area = drawn;
    renderer.draw(cmd, frame, geometry, extent_);
    renderer.area = {};
    vkCmdEndRenderPass(cmd);
    if (after_pass)
        after_pass(cmd, colour_.image(), extent_);

    // The render pass left the colour image as a transfer source, so the copy needs no barrier of
    // its own; the subpass dependency above orders it.
    if (readback) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, colour_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_.handle(), 1, &copy);
    } else {
        // Direct present: hand the image to the fragment shader instead of copying it out.
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // the render pass's final layout
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = colour_.image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        error_ = "vkEndCommandBuffer failed";
        return false;
    }

    vkResetFences(ctx_->device(), 1, &fence_[s]);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(ctx_->queue(), 1, &si, fence_[s]) != VK_SUCCESS) {
        error_ = "vkQueueSubmit failed";
        return false;
    }
    pending_[s] = true;
    slot_ = s;
    // With readback on, callers read pixels() straight after: it waits there.
    have_pixels_ = false;
    host_frame_ = readback;
    ++frames;
    return true;
}

bool Offscreen::wait() {
    bool ok = true;
    for (unsigned s = 0; s < kSlots; ++s)
        ok = wait_slot(s) && ok;
    return ok;
}

bool Offscreen::wait_slot(unsigned s) {
    if (!pending_[s])
        return true;
    pending_[s] = false;
    // One second is far longer than any frame takes; waiting for ever would turn a lost device
    // into a hung launcher with nothing on screen to say so.
    if (vkWaitForFences(ctx_->device(), 1, &fence_[s], VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) {
        error_ = "the GPU did not finish the frame within a second";
        return false;
    }
    return true;
}

bool Offscreen::copy_to_host() {
    if (!ctx_ || !cmd_[0] || frames == 0 || !wait())
        return false;
    // Every slot is idle after wait(): the copy uses the last render's.
    VkCommandBuffer cmd = cmd_[slot_];
    VkFence fence = fence_[slot_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
        return false;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = colour_.image();
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, colour_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_.handle(), 1, &copy);
    std::swap(b.oldLayout, b.newLayout);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        return false;
    vkResetFences(ctx_->device(), 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(ctx_->queue(), 1, &si, fence) != VK_SUCCESS)
        return false;
    return vkWaitForFences(ctx_->device(), 1, &fence, VK_TRUE, 1'000'000'000ull) == VK_SUCCESS;
}

const std::uint32_t* Offscreen::pixels() noexcept {
    if (!have_pixels_ && host_frame_ && wait())
        have_pixels_ = true;  // the render itself copied it; it only had to finish
    if (!have_pixels_ && !host_frame_ && copy_to_host())
        have_pixels_ = true;
    if (!have_pixels_)
        return nullptr;
    return static_cast<const std::uint32_t*>(readback_.mapped());
}

}  // namespace dream::render::vk
