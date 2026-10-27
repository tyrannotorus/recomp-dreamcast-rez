// Texture upload and caching. A guest texture is identified by the two hardware
// words that describe it plus the palette window it uses; the cache decodes it once, uploads it,
// and hands back a descriptor set the renderer can bind.
//
// Invalidation is deliberately coarse for now: the whole cache is dropped when the guest changes
// palette memory or when the caller says video memory has moved on. Tracking which texels a write
// touched is a later optimisation, and doing it wrong shows as textures that fail to update, which
// is a far worse failure than decoding one again.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dream/render/texture.h"
#include "dream/render/texture_pack.h"
#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

class TextureCache {
public:
    TextureCache() = default;
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    bool create(Context& ctx, VkDescriptorSetLayout layout);
    void destroy();

    // Points the cache at the guest's video and palette memory. Both are borrowed, not copied.
    void set_memory(const std::uint8_t* vram, std::size_t vram_size,
                    const std::uint32_t* palette_ram, PaletteFormat palette_format);
    // Drops everything, for instance after the guest rewrites palette memory.
    void invalidate();
    // Re-reads palette memory and drops everything only if it actually changed. A title rewrites
    // the palette registers far more often than it changes what is in them, and dropping the cache
    // every frame would decode every texture every frame. Returns true when it invalidated.
    bool set_palette(const std::uint32_t* palette_ram, PaletteFormat format);
    // Drops the textures whose pixels lie in [begin, end) of video memory. This is what makes a
    // render to texture work: the frame the renderer just wrote back into video memory has to
    // replace whatever the cache decoded from those bytes before. It is also the test that tells a
    // render to texture apart from ordinary double buffering, because a double-buffered title
    // never samples the buffer it just wrote. Returns the number dropped.
    // `rendered`: the bytes are a frame the renderer wrote back, so textures there are
    // never hashed for a pack or dump. False for the guest's own writes (dirty VRAM pages): those
    // are ordinary texture uploads and must stay replaceable.
    std::size_t invalidate_range(std::uint32_t begin, std::uint32_t end, bool rendered = true);
    // The descriptor set for a polygon's texture, or VK_NULL_HANDLE when it cannot be decoded.
    // `cmd` is a command buffer outside a render pass, used for the upload.
    //
    // `tiled` is whether any polygon in the frame actually samples this texture outside the unit
    // square (dream::render::polygon_tiles). A texture that is only ever mapped once across its
    // polygons is addressed clamp-to-edge whatever its TSP word says, because above the guest's
    // resolution repeat addressing wraps the outermost half-texel to the opposite edge and outlines
    // every quad. It is part of the key: the same texture can be tiled by one polygon and not by
    // another, and the two need different samplers.
    VkDescriptorSet get(VkCommandBuffer cmd, std::uint32_t tcw, std::uint32_t tsp, bool tiled);

    // Render to texture on the GPU: a render into texture memory (FB_W_SOF1 bit 24) is
    // copied from the render target (`src`, a transfer source, `src_w` x `src_h` for the guest's
    // 640x480) into an image kept per `address` (64-bit view offset), which later scan-order
    // textures there sample instead of video memory. `width` x `height`: the guest's write clip.
    // The image is the texture's power-of-two size at the render scale, the render top-left.
    void capture_rtt(VkCommandBuffer cmd, VkImage src, std::uint32_t src_w, std::uint32_t src_h,
                     std::uint32_t address, std::uint32_t width, std::uint32_t height);
    // Frees every captured image (the render scale changed). Waits for the device.
    void drop_rtt();
    // A 1x1 opaque white texture, bound when a polygon's own texture could not be decoded. The
    // shader is told not to sample, but Vulkan still wants a valid set bound, and a white texel
    // means a stray sample changes nothing.
    VkDescriptorSet fallback(VkCommandBuffer cmd);

    std::size_t size() const noexcept { return entries_.size(); }

    // Marks the start of a render. Everything a previous render used has finished on
    // the GPU by now (the offscreen pass waits on its fence), so entries not used since can be
    // freed without a device wait; the least recently used are evicted here when the cache is
    // near the descriptor pool's limit.
    void begin_frame();
    const std::string& error() const noexcept { return error_; }

    // Texture dumping and replacement packs (texture_pack.h). Inactive unless the
    // launcher opened a pack or set a dump directory. A replaced texture is an ordinary entry
    // with different pixels: it is keyed, invalidated by a palette change or a write-back, and
    // freed exactly as the decoded one would be, and the next decode looks it up again.
    TextureReplacer& replacer() noexcept { return replacer_; }

private:
    // HostBuffer owns a mapping, so an Entry moves but never copies.
    struct Entry {
        Entry() = default;
        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry&&) noexcept = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        HostBuffer staging;
        TextureInfo info;
        std::uint64_t last_frame = 0;
    };
    // Per hardware key: what the key needs to know before the texture is decoded.
    struct Meta {
        bool ok = false, indexed = false;
        std::uint32_t palette_base = 0, palette_count = 0;
    };
    std::unordered_map<std::uint64_t, Meta> meta_;
    // Captured renders (capture_rtt), by address, with a descriptor set per sampler.
    struct Rtt {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t image_w = 0, image_h = 0;  // pixels
        std::uint32_t bytes = 0;                 // the 16-bit texture's extent in video memory
        std::unordered_map<VkSampler, VkDescriptorSet> sets;
    };
    std::unordered_map<std::uint32_t, Rtt> rtt_;
    std::vector<std::pair<std::uint64_t, Rtt>> rtt_retired_;
    void free_rtt(Rtt& r);
    VkDescriptorSet rtt_set(std::uint32_t tcw, std::uint32_t tsp, bool tiled);
    // A hash of each 16-entry palette bank, updated when palette memory changes. An indexed
    // texture's cache key includes the hash of the banks it reads, so a palette that cycles
    // between a few states keeps hitting the cache instead of re-decoding.
    std::uint64_t bank_hash_[64]{};
    void rehash_banks();
    std::uint64_t frame_ = 1;
    // Upload staging: one ring buffer per in-flight render instead of a host buffer
    // allocated -- and kept -- per texture. A texture that does not fit falls back to its own.
    static constexpr unsigned kStageSlots = 4;
    static constexpr VkDeviceSize kStageBytes = 8u << 20;
    HostBuffer stage_ring_[kStageSlots];
    // Entries the guest overwrote: out of the cache at once, destroyed once no render
    // still in flight can sample them, instead of a device-wide wait on every overwrite.
    std::vector<std::pair<std::uint64_t, Entry>> retired_;
    void free_retired(bool all);
    unsigned stage_slot_ = 0;
    VkDeviceSize stage_used_ = 0;

    void free_entry(Entry& e);
    VkSampler sampler_for(std::uint32_t tsp, bool tiled);
    // `width` x `height` is the image uploaded, which for a pack replacement is not the guest's
    // size; `entry.info` keeps the guest's, which is what invalidation needs.
    // `mips`: the smaller levels of a mipmapped texture, each half the last.
    bool upload(VkCommandBuffer cmd, Entry& entry, const std::vector<std::uint32_t>& pixels,
                std::uint32_t width, std::uint32_t height,
                const std::vector<std::vector<std::uint32_t>>* mips = nullptr);
    // True when the texture's bytes overlap a region the renderer has written a frame into: a
    // render to texture, whose content changes every frame and is never dumped or replaced.
    bool rendered_into(const TextureInfo& info) const;

    Context* ctx_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::unordered_map<std::uint32_t, VkSampler> samplers_;
    Entry fallback_{};
    const std::uint8_t* vram_ = nullptr;
    std::size_t vram_size_ = 0;
    std::uint32_t palette_[1024]{};
    std::string error_;
    TextureReplacer replacer_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> written_ranges_;
};

}  // namespace dream::render::vk
