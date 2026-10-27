// Drawing a decoded frame. The pipeline state that varies per polygon (depth compare, depth
// write, culling, blending) comes from the hardware's own ISP and TSP words, so the pipelines are
// cached on exactly the bits that matter.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dream/render/display_list.h"
#include "dream/render/fog.h"
#include "dream/render/vk/context.h"
#include "dream/render/vk/resources.h"
#include "dream/render/vk/texture_cache.h"

#include <vulkan/vulkan.h>

namespace dream::render::vk {

// What the guest's registers say the frame is, so the projection matches the game's own idea of
// the screen rather than the window.
struct FrameGeometry {
    // The region of the guest's screen to show: the whole 640x480 framebuffer.
    float left = 0, top = 0;
    float width = 640, height = 480;
    // The punch-through threshold, from the hardware's PT_ALPHA_REF register (PVR + 0x11C).
    float alpha_ref = 1.0f / 255.0f;
    // How much of a shadowed surface's light is left inside a modifier volume: FPU_SHAD_SCALE
    // (PVR + 0x074) bits 0-7 over 256.
    float shadow_scale = 0.5f;
    // Translucent auto-sort (the hardware sorts translucent polygons itself): from the region
    // array's PreSort bit or ISP_FEED_CFG bit 0. The translucent list then tests
    // depth with GREATER_EQUAL whatever its ISP word says, as Flycast does, so a layer drawn
    // exactly on an opaque surface is not rejected.
    bool autosort = true;
    // Fog registers: colours, density and the 128-entry table.
    FogSettings fog;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool create(Context& ctx, VkRenderPass render_pass);
    // Points the renderer at the guest's video and palette memory so textures can be decoded.
    void set_memory(const std::uint8_t* vram, std::size_t vram_size,
                    const std::uint32_t* palette_ram, PaletteFormat palette_format);
    TextureCache& textures() noexcept { return textures_; }
    // The render pass new pipelines are built against. Pipelines already made stay
    // valid for any compatible pass, but the handle passed to create() must not outlive its
    // pass: when the target is rebuilt, hand the renderer the new one, or the next pipeline is
    // created against a destroyed pass (a crash inside the driver).
    void set_render_pass(VkRenderPass pass) noexcept { render_pass_ = pass; }
    // Persistent pipelines: loads a Vulkan pipeline cache and the list of pipeline
    // variants earlier runs used from `path`, and creates those variants now, so none is compiled
    // mid-game (a 10-30 ms hitch each on some drivers). destroy() writes both back. Call after
    // create(). A missing, foreign or damaged file only means starting cold.
    void use_pipeline_cache(const std::string& path);
    // Pipeline variants a port knows its game uses (keys as use_pipeline_cache reads
    // them), created now as well, so even a first run with no cache compiles none mid-game. Call
    // after use_pipeline_cache. Returns how many were new.
    std::uint32_t prewarm(const std::vector<std::uint32_t>& keys);
    std::uint32_t prewarmed = 0;
    void destroy();

    // Records the draw commands for one decoded frame. The caller has already begun the render
    // pass and set the viewport and scissor.
    // `upload` is a command buffer outside a render pass, where textures are uploaded; it may be
    // the same buffer as `cmd` provided draw() is called before the render pass begins.
    void draw(VkCommandBuffer cmd, const Frame& frame, const FrameGeometry& geometry,
              VkExtent2D target);
    // The part of the target draw() may write, from its top-left corner: a render to
    // texture's own region, all a PowerVR had tiles for. Zero: the whole target.
    VkExtent2D area{};
    // Decodes and uploads every texture the frame needs. Call outside the render pass.
    void prepare(VkCommandBuffer upload, const Frame& frame);

    const std::string& error() const noexcept { return error_; }

private:
    // The pipeline state a polygon can vary. Packed so it can key the cache.
    struct PipelineKey {
        std::uint32_t depth_mode : 3;  // ISP DepthMode
        std::uint32_t depth_write : 1;
        std::uint32_t cull_mode : 2;  // ISP CullMode
        std::uint32_t src_blend : 3;  // TSP SrcInstr
        std::uint32_t dst_blend : 3;  // TSP DstInstr
        std::uint32_t blend : 1;      // blending on at all (the translucent list)
        std::uint32_t shadow : 1;     // PCW Shadow: modifier volumes darken it (opaque lists)
        // The fragment shader that can discard: for the alpha test and user clip mode
        // 3 only, as any discard in a shader costs a tile-based GPU its hidden-surface removal.
        std::uint32_t discard : 1;
        // The fragment shader's mode: the push constants' mode bits 0-9, which the
        // pipeline's shader is specialized for (kMode in geometry.frag). In a key's bits from
        // bit 16 (bit 15 names a line variant in key lists).
        std::uint32_t shader : 10;
        std::uint32_t padding : 7;
        std::uint32_t bits() const noexcept {
            return depth_mode | (depth_write << 3) | (cull_mode << 4) | (src_blend << 6) |
                   (dst_blend << 9) | (blend << 12) | (shadow << 13) | (discard << 14) | (shader << 16);
        }
        bool operator==(const PipelineKey& o) const noexcept { return bits() == o.bits(); }
    };
    struct KeyHash {
        std::size_t operator()(const PipelineKey& k) const noexcept { return k.bits(); }
    };

    VkPipeline pipeline_for(const PipelineKey& key);
    // Line batches' pipelines: a key's state with lines.vert, an instance a line on
    // binding 1. In a list of pipeline keys (prewarm), bit 15 names a key's line variant.
    VkPipeline line_pipeline_for(const PipelineKey& key);
    VkPipeline make_pipeline(const PipelineKey& key, bool lines);
    static PipelineKey key_from_bits(std::uint32_t bits);
    static constexpr std::uint32_t kLineKeyBit = 1u << 15;
    std::unordered_map<std::uint32_t, VkPipeline> line_pipelines_;
    VkShaderModule lines_vs_ = VK_NULL_HANDLE;
    // Modifier volumes, as Flycast's Vulkan renderer draws them: per volume, the
    // triangles XOR (closed) or OR (open) a stencil bit where they pass the depth test; the last
    // triangle's instruction folds it into "inside" (inclusion) or "outside" (exclusion); then
    // one quad darkens every pixel that is inside and belongs to a shadow-receiving polygon.
    enum class ModVol : std::uint32_t { Xor, Or, Inclusion, Exclusion, Final };
    VkPipeline modvol_pipeline(ModVol mode, std::uint32_t cull);
    void draw_modifier_volumes(VkCommandBuffer cmd, const Frame& frame,
                               const FrameGeometry& geometry, VkBuffer geometry_buffer);
    std::unordered_map<std::uint32_t, VkPipeline> modvol_pipelines_;
    VkShaderModule mv_vs_ = VK_NULL_HANDLE, mv_fs_ = VK_NULL_HANDLE;
    bool stencil_ = false;
    static constexpr unsigned kModvolRing = 4;
    HostBuffer modvol_ring_[kModvolRing];
    unsigned modvol_next_ = 0;
    std::vector<float> modvol_staging_;
    std::vector<ModifierTriangle> modvol_tris_;
    void save_pipeline_cache();
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    std::string pipeline_cache_path_;

    Context* ctx_ = nullptr;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkShaderModule fs_nd_ = VK_NULL_HANDLE;  // fs_ without its discards (PipelineKey::discard)
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::unordered_map<PipelineKey, VkPipeline, KeyHash> pipelines_;
    // The same by key, direct-mapped: a load or two instead of a hash lookup.
    static constexpr std::uint32_t kTableBits = 10;
    struct TableSlot {
        std::uint32_t bits = ~0u;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    std::array<TableSlot, 1u << kTableBits> pipeline_table_{};
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    TextureCache textures_;
    std::unordered_map<std::uint64_t, VkDescriptorSet> frame_sets_;
    VkDescriptorSet fallback_set_ = VK_NULL_HANDLE;
    static constexpr unsigned kVertexRing = 4;
    HostBuffer vertex_ring_[kVertexRing];
    // Batched draws: every item's triangles in draw order, and consecutive items with
    // the same state drawn as one indexed list. A ring like the vertices'.
    HostBuffer index_ring_[kVertexRing];
    HostBuffer line_ring_[kVertexRing];  // line batches' instances
    unsigned ring_next_ = 0;
    std::vector<float> staging_;  // vertex data in the shader's layout
    std::string error_;
};

}  // namespace dream::render::vk
