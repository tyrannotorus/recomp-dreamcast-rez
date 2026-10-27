// See texture_cache.h.
#include "dream/render/vk/texture_cache.h"

#include "dream/render/png.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dream/render/tsp.h"

namespace dream::render::vk {

namespace {

constexpr std::uint32_t kMaxTextures = 2048;

// Repeat and mirror only describe what happens outside the unit square. A polygon that never goes
// there cannot tell them apart from clamping at the guest's own resolution, and above it clamping
// is the only one that does not fetch the far edge of the texture for the outermost half-texel.
VkSamplerAddressMode address_mode(bool clamp, bool flip, bool tiled) {
    if (clamp || !tiled)
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return flip ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

}  // namespace

TextureCache::~TextureCache() {
    destroy();
}

bool TextureCache::create(Context& ctx, VkDescriptorSetLayout layout) {
    ctx_ = &ctx;
    layout_ = layout;
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = kMaxTextures;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kMaxTextures;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(ctx.device(), &dpci, nullptr, &pool_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorPool failed";
        return false;
    }
    return true;
}

void TextureCache::set_memory(const std::uint8_t* vram, std::size_t vram_size,
                              const std::uint32_t* palette_ram, PaletteFormat palette_format) {
    vram_ = vram;
    vram_size_ = vram_size;
    if (palette_ram)
        unpack_palette(palette_ram, palette_format, palette_);
    rehash_banks();
    invalidate();
}

void TextureCache::rehash_banks() {
    for (unsigned b = 0; b < 64; ++b) {
        std::uint64_t h = 0xCBF29CE484222325ull;
        for (unsigned i = 0; i < 16; ++i) h = (h ^ palette_[b * 16 + i]) * 0x100000001B3ull;
        bank_hash_[b] = h;
    }
}

void TextureCache::free_retired(bool all) {
    std::size_t keep = 0;
    for (std::size_t i = 0; i < retired_.size(); ++i) {
        if (all || retired_[i].first + kStageSlots < frame_)
            free_entry(retired_[i].second);
        else if (keep != i)
            retired_[keep++] = std::move(retired_[i]);
        else
            ++keep;
    }
    retired_.resize(keep);
    keep = 0;
    for (std::size_t i = 0; i < rtt_retired_.size(); ++i) {
        if (all || rtt_retired_[i].first + kStageSlots < frame_)
            free_rtt(rtt_retired_[i].second);
        else if (keep != i)
            rtt_retired_[keep++] = std::move(rtt_retired_[i]);
        else
            ++keep;
    }
    rtt_retired_.resize(keep);
}

void TextureCache::begin_frame() {
    ++frame_;
    stage_slot_ = (stage_slot_ + 1) % kStageSlots;
    stage_used_ = 0;
    free_retired(false);
    // Leave headroom under the pool (kMaxTextures descriptor sets) for one frame's new textures.
    constexpr std::size_t kHigh = 1536, kLow = 1024;
    if (entries_.size() < kHigh || !ctx_ || !ctx_->device())
        return;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> age;  // last_frame, key
    age.reserve(entries_.size());
    for (const auto& [key, e] : entries_) age.emplace_back(e.last_frame, key);
    std::sort(age.begin(), age.end());
    for (std::size_t i = 0; i < age.size() && entries_.size() > kLow; ++i) {
        // Used by this render or one that may still be in flight (up to kStageSlots): keep.
        if (age[i].first + kStageSlots >= frame_)
            break;
        auto it = entries_.find(age[i].second);
        free_entry(it->second);
        entries_.erase(it);
    }
}

void TextureCache::invalidate() {
    if (!ctx_ || !ctx_->device())
        return;
    vkDeviceWaitIdle(ctx_->device());
    for (auto& [key, e] : entries_) free_entry(e);
    entries_.clear();
    for (auto& [address, r] : rtt_) free_rtt(r);
    rtt_.clear();
    free_retired(true);
}

void TextureCache::free_entry(Entry& e) {
    if (e.set)
        vkFreeDescriptorSets(ctx_->device(), pool_, 1, &e.set);
    if (e.view)
        vkDestroyImageView(ctx_->device(), e.view, nullptr);
    if (e.image)
        vkDestroyImage(ctx_->device(), e.image, nullptr);
    if (e.memory)
        vkFreeMemory(ctx_->device(), e.memory, nullptr);
    e.staging.destroy();
}

bool TextureCache::set_palette(const std::uint32_t* palette_ram, PaletteFormat format) {
    if (!palette_ram)
        return false;
    std::uint32_t unpacked[1024]{};
    unpack_palette(palette_ram, format, unpacked);
    if (std::memcmp(unpacked, palette_, sizeof palette_) == 0)
        return false;
    std::memcpy(palette_, unpacked, sizeof palette_);
    // Nothing is freed: indexed entries are keyed by the content of the palette banks
    // they read (bank_hash_), so the textures for the new palette are different keys and the old
    // ones stay cached for when the palette comes back. LRU eviction in begin_frame() bounds it.
    rehash_banks();
    return true;
}

std::size_t TextureCache::invalidate_range(std::uint32_t begin, std::uint32_t end, bool rendered) {
    if (!ctx_ || !ctx_->device() || end <= begin)
        return 0;
    // A captured render the guest has since written over: what is there now is the
    // guest's, and decoding it is right again.
    if (!rendered)
        for (auto it = rtt_.begin(); it != rtt_.end();) {
            if (it->first < end && begin < it->first + it->second.bytes) {
                rtt_retired_.emplace_back(frame_, std::move(it->second));
                it = rtt_.erase(it);
            } else {
                ++it;
            }
        }
    // Remembered so a render target is never dumped or replaced: its content is a frame, and
    // every frame would be a new texture.
    if (rendered && replacer_.active() &&
        std::find(written_ranges_.begin(), written_ranges_.end(), std::make_pair(begin, end)) ==
            written_ranges_.end() &&
        written_ranges_.size() < 64)
        written_ranges_.emplace_back(begin, end);
    std::vector<std::uint64_t> doomed;
    for (const auto& [key, e] : entries_) {
        const std::uint32_t lo = e.info.address;
        const std::uint32_t bytes = e.info.size_bytes();
        // An entry the decoder rejected has no pixels and no size; it is kept so the failure is
        // not retried every frame, and a write cannot make it stale.
        if (bytes == 0)
            continue;
        if (lo < end && begin < lo + bytes)
            doomed.push_back(key);
    }
    if (doomed.empty())
        return 0;
    for (std::uint64_t key : doomed) {
        auto it = entries_.find(key);
        if (it == entries_.end())
            continue;
        retired_.emplace_back(frame_, std::move(it->second));
        entries_.erase(it);
    }
    return doomed.size();
}

VkSampler TextureCache::sampler_for(std::uint32_t tsp, bool tiled) {
    const std::uint32_t key =
        tsp_sampler_key(tsp) | (tiled ? 0x40u : 0u) | (((tsp >> 8) & 0xFu) << 7);
    if (auto it = samplers_.find(key); it != samplers_.end())
        return it->second;

    // Filter mode 0 is point sampling; anything else is bilinear (the hardware's trilinear modes
    // need mipmaps, which are decoded but not yet uploaded as a chain).
    const bool bilinear = tsp_filter_mode(tsp) != 0;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    // Trilinear filter modes blend between levels; the others take the nearest.
    sci.mipmapMode = tsp_filter_mode(tsp) >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                               : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // MipMapD, as Flycast's D_Adjust_LoD_Bias table.
    static constexpr float kLodBias[16] = {0.f, -4.f, -2.f, -1.f, 0.f, 0.f, 0.f, 0.f,
                                           0.f, 0.f,  0.f,  0.f,  0.f, 0.f, 0.f, 0.f};
    sci.mipLodBias = kLodBias[(tsp >> 8) & 0xFu];
    sci.addressModeU = address_mode(tsp_clamp_u(tsp) != 0, tsp_flip_u(tsp) != 0, tiled);
    sci.addressModeV = address_mode(tsp_clamp_v(tsp) != 0, tsp_flip_v(tsp) != 0, tiled);
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(ctx_->device(), &sci, nullptr, &sampler);
    samplers_.emplace(key, sampler);
    return sampler;
}

bool TextureCache::rendered_into(const TextureInfo& info) const {
    const std::uint32_t lo = info.address, hi = info.address + info.size_bytes();
    for (const auto& [begin, end] : written_ranges_)
        if (lo < end && begin < hi)
            return true;
    return false;
}

bool TextureCache::upload(VkCommandBuffer cmd, Entry& e, const std::vector<std::uint32_t>& pixels,
                          std::uint32_t width, std::uint32_t height,
                          const std::vector<std::vector<std::uint32_t>>* mips) {
    // Every level goes into one staging area, base level first.
    const std::uint32_t levels = 1u + (mips ? static_cast<std::uint32_t>(mips->size()) : 0u);
    std::vector<VkDeviceSize> offsets(levels);
    VkDeviceSize bytes = 0;
    for (std::uint32_t l = 0; l < levels; ++l) {
        offsets[l] = bytes;
        const std::uint32_t lw = std::max(1u, width >> l), lh = std::max(1u, height >> l);
        bytes += (static_cast<VkDeviceSize>(lw) * lh * 4 + 15u) & ~VkDeviceSize{15};
    }
    auto level_data = [&](std::uint32_t l) -> const std::vector<std::uint32_t>& {
        return l == 0 ? pixels : (*mips)[l - 1];
    };
    VkBuffer source = VK_NULL_HANDLE;
    VkDeviceSize source_offset = 0;
    HostBuffer& ring = stage_ring_[stage_slot_];
    const VkDeviceSize at = (stage_used_ + 15u) & ~VkDeviceSize{15};
    std::uint8_t* dst = nullptr;
    if (at + bytes <= kStageBytes && ring.ensure(*ctx_, kStageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) &&
        ring.mapped()) {
        dst = static_cast<std::uint8_t*>(ring.mapped()) + at;
        source = ring.handle();
        source_offset = at;
        stage_used_ = at + bytes;
    } else {
        if (!e.staging.ensure(*ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !e.staging.mapped())
            return false;
        dst = static_cast<std::uint8_t*>(e.staging.mapped());
        source = e.staging.handle();
    }
    for (std::uint32_t l = 0; l < levels; ++l) {
        const auto& data = level_data(l);
        std::memcpy(dst + offsets[l], data.data(), data.size() * 4);
    }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx_->device(), &ici, nullptr, &e.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx_->device(), e.image, &req);
    const std::uint32_t type = find_memory_type(ctx_->physical_device(), req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx_->device(), &mai, nullptr, &e.memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(ctx_->device(), e.image, e.memory, 0);

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = e.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_dst);

    std::vector<VkBufferImageCopy> copies(levels);
    for (std::uint32_t l = 0; l < levels; ++l) {
        copies[l] = VkBufferImageCopy{};
        copies[l].bufferOffset = source_offset + offsets[l];
        copies[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        copies[l].imageExtent = {std::max(1u, width >> l), std::max(1u, height >> l), 1};
    }
    vkCmdCopyBufferToImage(cmd, source, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels,
                           copies.data());

    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_read);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = e.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (vkCreateImageView(ctx_->device(), &vci, nullptr, &e.view) != VK_SUCCESS)
        return false;
    return true;
}

VkDescriptorSet TextureCache::get(VkCommandBuffer cmd, std::uint32_t tcw, std::uint32_t tsp,
                                  bool tiled) {
    if (!vram_ || !ctx_)
        return VK_NULL_HANDLE;
    if (!rtt_.empty())
        if (VkDescriptorSet set = rtt_set(tcw, tsp, tiled))
            return set;
    // The size lives in the TSP word, the rest in the TCW; together they identify the texture, and
    // the addressing the polygons need decides the sampler bound with it.
    std::uint64_t key = (static_cast<std::uint64_t>(tcw) << 32) | (tsp & 0x3Fu) |
                        (tiled ? 0x40u : 0u) |
                        // The sampler state (filter, clamp, flip, mip D-adjust: TSP bits 8-18)
                        // is part of the descriptor set, so it is part of the key: the first
                        // polygon to use a texture does not decide it for all.
                        (static_cast<std::uint64_t>((tsp >> 8) & 0x7FFu) << 7);
    // Indexed textures: fold in the content of the palette banks they read, so a
    // palette change selects a different entry instead of invalidating this one.
    {
        auto [mit, fresh] = meta_.try_emplace(key);
        Meta& meta = mit->second;
        if (fresh) {
            TextureInfo probe;
            meta.ok = describe_texture(tcw, tsp, 0, probe);
            meta.indexed = meta.ok && probe.indexed();
            meta.palette_base = probe.palette_base;
            meta.palette_count = probe.format == PixelFormat::Palette4 ? 16u : 256u;
        }
        if (meta.indexed) {
            std::uint64_t h = 0x9E3779B97F4A7C15ull;
            for (std::uint32_t b = meta.palette_base / 16;
                 b < (meta.palette_base + meta.palette_count) / 16 && b < 64; ++b)
                h = (h ^ bank_hash_[b]) * 0x100000001B3ull;
            key ^= h & 0xFFFFFFFFFFFFFF80ull;  // keeps the low bits (tsp size, tiled) readable
        }
    }
    if (auto it = entries_.find(key); it != entries_.end()) {
        it->second.last_frame = frame_;
        return it->second.set;
    }

    Entry e;
    e.last_frame = frame_;
    if (!describe_texture(tcw, tsp, 0, e.info)) {
        entries_.emplace(key, std::move(e));  // remember the failure so it is not retried per frame
        return VK_NULL_HANDLE;
    }
    // Pack and dump: the identity is a content hash, worked out only when either is on.
    std::uint64_t hash = 0;
    bool hashed = false;
    const png::Image* replacement = nullptr;
    if (replacer_.active() && !rendered_into(e.info)) {
        hashed = texture_hash(e.info, vram_, vram_size_, palette_, hash);
        if (hashed && replacer_.replacing()) {
            replacement = replacer_.find(e.info, hash);
            const std::uint32_t limit = ctx_->caps().max_texture_size;
            if (replacement && limit &&
                (replacement->width > limit || replacement->height > limit)) {
                std::fprintf(stderr, "texture pack: %ux%u is larger than this GPU allows (%u)\n",
                             replacement->width, replacement->height, limit);
                replacement = nullptr;
            }
        }
    }
    std::vector<std::uint32_t> pixels;
    if (!replacement || replacer_.dumping()) {
        if (!decode_texture(e.info, vram_, vram_size_, palette_, pixels)) {
            entries_.emplace(key, std::move(e));
            return VK_NULL_HANDLE;
        }
        if (hashed)
            replacer_.dump(e.info, hash, pixels);
    }
    // The rest of a mipmapped texture's chain, down to 8x8: level l is the texture
    // of half the size again, which mipmap_base_offset places in the chain.
    std::vector<std::vector<std::uint32_t>> mips;
    if (!replacement && e.info.mipmapped) {
        for (std::uint32_t size = e.info.width / 2; size >= 8; size /= 2) {
            TextureInfo level = e.info;
            level.width = level.height = size;
            std::vector<std::uint32_t> px;
            if (!decode_texture(level, vram_, vram_size_, palette_, px))
                break;
            mips.push_back(std::move(px));
        }
    }
    const bool uploaded =
        replacement
            ? upload(cmd, e, replacement->pixels, replacement->width, replacement->height)
            : upload(cmd, e, pixels, e.info.width, e.info.height, mips.empty() ? nullptr : &mips);
    if (!uploaded) {
        entries_.emplace(key, std::move(e));
        return VK_NULL_HANDLE;
    }
    e.sampler = sampler_for(tsp, tiled);

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(ctx_->device(), &dsai, &e.set) != VK_SUCCESS) {
        error_ = "the descriptor pool is exhausted";
        entries_.emplace(key, std::move(e));
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo image{};
    image.sampler = e.sampler;
    image.imageView = e.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = e.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);

    VkDescriptorSet set = e.set;
    entries_.emplace(key, std::move(e));
    return set;
}

// Render to texture on the GPU; see capture_rtt in the header.
void TextureCache::free_rtt(Rtt& r) {
    for (auto& [sampler, set] : r.sets)
        if (set)
            vkFreeDescriptorSets(ctx_->device(), pool_, 1, &set);
    r.sets.clear();
    if (r.view)
        vkDestroyImageView(ctx_->device(), r.view, nullptr);
    if (r.image)
        vkDestroyImage(ctx_->device(), r.image, nullptr);
    if (r.memory)
        vkFreeMemory(ctx_->device(), r.memory, nullptr);
    r.view = VK_NULL_HANDLE;
    r.image = VK_NULL_HANDLE;
    r.memory = VK_NULL_HANDLE;
}

void TextureCache::drop_rtt() {
    if (!ctx_ || !ctx_->device())
        return;
    vkDeviceWaitIdle(ctx_->device());
    for (auto& [address, r] : rtt_) free_rtt(r);
    rtt_.clear();
    for (auto& [frame, r] : rtt_retired_) free_rtt(r);
    rtt_retired_.clear();
}

void TextureCache::capture_rtt(VkCommandBuffer cmd, VkImage src, std::uint32_t src_w,
                               std::uint32_t src_h, std::uint32_t address, std::uint32_t width,
                               std::uint32_t height) {
    if (!ctx_ || !ctx_->device() || !src || width == 0 || height == 0 || src_w == 0 || src_h == 0)
        return;
    width = std::min(width, 640u);
    height = std::min(height, 480u);
    const auto pow2 = [](std::uint32_t v) {
        std::uint32_t p = 8;
        while (p < v && p < 1024)
            p <<= 1;
        return p;
    };
    const std::uint32_t pw = pow2(width), ph = pow2(height);
    // The target holds the guest's 640x480 at src_h / 480 pixels per guest pixel down, and
    // src_w / 640 across (more when the picture is widened). The captured image keeps the
    // vertical scale both ways, which undoes any widening.
    const double scale = static_cast<double>(src_h) / 480.0;
    const auto scaled = [&](std::uint32_t v) {
        return std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(v * scale + 0.5));
    };
    const std::uint32_t iw = scaled(pw), ih = scaled(ph);
    auto it = rtt_.find(address);
    if (it != rtt_.end() && (it->second.image_w != iw || it->second.image_h != ih)) {
        rtt_retired_.emplace_back(frame_, std::move(it->second));
        rtt_.erase(it);
        it = rtt_.end();
    }
    bool fresh = false;
    if (it == rtt_.end()) {
        Rtt r;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {iw, ih, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx_->device(), &ici, nullptr, &r.image) != VK_SUCCESS)
            return;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(ctx_->device(), r.image, &req);
        const std::uint32_t type = find_memory_type(ctx_->physical_device(), req.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(ctx_->device(), &mai, nullptr, &r.memory) != VK_SUCCESS) {
            free_rtt(r);
            return;
        }
        vkBindImageMemory(ctx_->device(), r.image, r.memory, 0);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = r.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(ctx_->device(), &vci, nullptr, &r.view) != VK_SUCCESS) {
            free_rtt(r);
            return;
        }
        r.image_w = iw;
        r.image_h = ih;
        r.bytes = pw * ph * 2;
        it = rtt_.emplace(address, std::move(r)).first;
        fresh = true;
    }
    Rtt& r = it->second;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = r.image;
    b.subresourceRange = range;
    // Earlier renders sampled it (in submissions before this one, on the same queue).
    b.oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    if (fresh) {
        // Outside the rendered region the texture is what video memory held: unknown. Clear.
        const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(cmd, r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             1, &mb, 0, nullptr, 0, nullptr);
    }
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(std::min(src_w, (width * src_w + 639) / 640)),
                          static_cast<std::int32_t>(std::min(src_h, (height * src_h + 479) / 480)), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<std::int32_t>(std::min(iw, scaled(width))),
                          static_cast<std::int32_t>(std::min(ih, scaled(height))), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

VkDescriptorSet TextureCache::rtt_set(std::uint32_t tcw, std::uint32_t tsp, bool tiled) {
    TextureInfo info;
    // A render is written in scan order: only a scan-order (or stride) 16-bit texture is one.
    if (!describe_texture(tcw, tsp, 0, info) || info.twiddled || info.vq || info.indexed())
        return VK_NULL_HANDLE;
    auto it = rtt_.find(info.address);
    if (it == rtt_.end())
        return VK_NULL_HANDLE;
    Rtt& r = it->second;
    const VkSampler sampler = sampler_for(tsp, tiled);
    auto [sit, fresh] = r.sets.try_emplace(sampler, VK_NULL_HANDLE);
    if (fresh) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = pool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &layout_;
        if (vkAllocateDescriptorSets(ctx_->device(), &dsai, &sit->second) != VK_SUCCESS) {
            r.sets.erase(sit);
            return VK_NULL_HANDLE;
        }
        VkDescriptorImageInfo image{};
        image.sampler = sampler;
        image.imageView = r.view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sit->second;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
    }
    return sit->second;
}

VkDescriptorSet TextureCache::fallback(VkCommandBuffer cmd) {
    if (fallback_.set)
        return fallback_.set;
    fallback_.info = TextureInfo{};
    fallback_.info.width = 1;
    fallback_.info.height = 1;
    const std::vector<std::uint32_t> white{0xFFFFFFFFu};
    if (!upload(cmd, fallback_, white, 1, 1))
        return VK_NULL_HANDLE;
    fallback_.sampler = sampler_for(0, false);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(ctx_->device(), &dsai, &fallback_.set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorImageInfo image{};
    image.sampler = fallback_.sampler;
    image.imageView = fallback_.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = fallback_.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
    return fallback_.set;
}

void TextureCache::destroy() {
    if (!ctx_ || !ctx_->device())
        return;
    invalidate();
    if (fallback_.view)
        vkDestroyImageView(ctx_->device(), fallback_.view, nullptr);
    if (fallback_.image)
        vkDestroyImage(ctx_->device(), fallback_.image, nullptr);
    if (fallback_.memory)
        vkFreeMemory(ctx_->device(), fallback_.memory, nullptr);
    fallback_.staging.destroy();
    for (auto& b : stage_ring_) b.destroy();
    fallback_.set = VK_NULL_HANDLE;
    fallback_.sampler = VK_NULL_HANDLE;
    for (auto& [key, sampler] : samplers_) vkDestroySampler(ctx_->device(), sampler, nullptr);
    samplers_.clear();
    if (pool_) {
        vkDestroyDescriptorPool(ctx_->device(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

}  // namespace dream::render::vk
