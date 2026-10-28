// See renderer.h.
#include <cmath>
#include "dream/render/vk/renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "dream/render/tsp.h"

#include "geometry_frag.h"
#include "geometry_frag_nd.h"
#include "geometry_vert.h"
#include "lines_vert.h"
#include "modvol_frag.h"
#include "modvol_vert.h"

namespace dream::render::vk {

namespace {

// Vertex layout handed to the shaders: position (x, y, 1/w), texture coordinates, the base and
// offset colours as the hardware packs them (ARGB, 8 bits each: the vertex format makes floats of
// them), and the table fog factor. The decoder's Vertex is the first 28 bytes of it.
constexpr std::size_t kVertexBytes = sizeof(Vertex) + sizeof(float);

// A line of a line batch as lines.vert reads it, an instance: its ends (x, y, 1/w,
// and the table fog factor of that 1/w), the batch's colours (ARGB, as a vertex's) and half width.
struct LineInstance {
    float a[4], b[4];
    std::uint32_t base, offset;
    float half_width;
};
static_assert(sizeof(Vertex) == 28 && offsetof(Vertex, base) == 20 && offsetof(Vertex, offset) == 24,
              "the vertex buffer's layout starts with the decoder's Vertex");

// ISP/TSP instruction word (Flycast's ISP_TSP union): the fields that decide pipeline state.
constexpr std::uint32_t isp_depth_mode(std::uint32_t isp) {
    return (isp >> 29) & 7u;
}
constexpr std::uint32_t isp_cull_mode(std::uint32_t isp) {
    return (isp >> 27) & 3u;
}
constexpr std::uint32_t isp_z_write_disable(std::uint32_t isp) {
    return (isp >> 26) & 1u;
}

// The hardware's eight depth comparisons, in its own order. The depth buffer holds 1/w, so
// "nearer" is a larger value and the senses are the reverse of the usual convention.
VkCompareOp depth_compare(std::uint32_t mode) {
    switch (mode) {
        case 0:
            return VK_COMPARE_OP_NEVER;
        case 1:
            return VK_COMPARE_OP_LESS;
        case 2:
            return VK_COMPARE_OP_EQUAL;
        case 3:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 4:
            return VK_COMPARE_OP_GREATER;
        case 5:
            return VK_COMPARE_OP_NOT_EQUAL;
        case 6:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default:
            return VK_COMPARE_OP_ALWAYS;
    }
}

// Culling is by signed area on the hardware, with two thresholds we cannot express directly. Modes
// 2 and 3 are the ordinary back-face and front-face cases; 0 and 1 disable it.
VkCullModeFlags cull_flags(std::uint32_t mode) {
    switch (mode) {
        case 2:
            return VK_CULL_MODE_FRONT_BIT;
        case 3:
            return VK_CULL_MODE_BACK_BIT;
        default:
            return VK_CULL_MODE_NONE;
    }
}

VkShaderModule make_module(VkDevice device, const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

struct PushConstants {
    float scale[2];
    float offset[2];
    std::int32_t mode;
    float alpha_ref;
    float pad[2];       // the shader's vec4 members start on a 16-byte boundary
    float fog_vert[4];  // FOG_COL_VERT (per-vertex fog), RGBA
    float fog_ram[4];   // FOG_COL_RAM (table fog), RGBA
    float clip[4];      // user clip mode 3: discard inside this rectangle (target pixels)
};
static_assert(offsetof(PushConstants, fog_vert) == 32, "push constants must match the shaders");

// What identifies a texture within one frame: the control word, plus the TSP bits that carry the
// size. Everything the sampler reads is decided per texture in prepare(), so it does not belong
// here; two polygons that share these bits share the decoded image.
constexpr std::uint64_t texture_key(const Polygon& p) {
    // Size bits, plus the sampler bits (TSP 8-18), which choose the descriptor set too.
    return (static_cast<std::uint64_t>(p.tcw) << 32) | (p.tsp & 0x3Fu) |
           (static_cast<std::uint64_t>((p.tsp >> 8) & 0x7FFu) << 7);
}

// Parameter control word and TSP fields the fragment shader needs.
constexpr std::uint32_t pcw_texture(std::uint32_t p) {
    return (p >> 3) & 1u;
}
constexpr std::uint32_t pcw_offset(std::uint32_t p) {
    return (p >> 2) & 1u;
}
constexpr std::uint32_t pcw_shadow(std::uint32_t p) {
    return (p >> 7) & 1u;
}
// The hardware's eight blend factors. Index 2 and 3 mean "the other colour", which is the
// destination colour for a source factor and the source colour for a destination factor; the rest
// are the same on both sides.
VkBlendFactor src_factor(std::uint32_t instr) {
    switch (instr) {
        case 0:
            return VK_BLEND_FACTOR_ZERO;
        case 1:
            return VK_BLEND_FACTOR_ONE;
        case 2:
            return VK_BLEND_FACTOR_DST_COLOR;
        case 3:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 4:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:
            return VK_BLEND_FACTOR_DST_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}
VkBlendFactor dst_factor(std::uint32_t instr) {
    switch (instr) {
        case 0:
            return VK_BLEND_FACTOR_ZERO;
        case 1:
            return VK_BLEND_FACTOR_ONE;
        case 2:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case 3:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 4:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case 5:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 6:
            return VK_BLEND_FACTOR_DST_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}


// A depth as an unsigned key whose order is the floats' order, -0 with +0. NaN comes after
// everything, which keeps the order strict; a NaN depth is a degenerate triangle with no finite
// vertex, which draws nothing, so drawing it last changes no pixel.
inline std::uint32_t depth_key(float f) noexcept {
    if (std::isnan(f))
        return 0xFFFFFFFFu;
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    if (b == 0x80000000u)
        b = 0;  // -0 sorts with +0
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

// The translucent items back to front, stable: `pairs` holds each item's depth_key in
// the high half and its index in the low, in the items' order, and is sorted in place; ordered as
// numbers, pairs with the same key keep their items' order. A radix sort of the keys' bits, four
// stable passes of a byte each.
void sort_back_to_front(std::vector<std::uint64_t>& pairs) {
    const std::size_t n = pairs.size();
    thread_local std::vector<std::uint64_t> spare;
    if (n < 256) {
        std::sort(pairs.begin(), pairs.end());
    } else {
        spare.resize(n);
        for (unsigned shift = 32; shift < 64; shift += 8) {
            std::size_t count[257] = {};
            for (std::size_t i = 0; i < n; ++i) ++count[((pairs[i] >> shift) & 0xFFu) + 1];
            if (count[1 + ((pairs[0] >> shift) & 0xFFu)] == n)
                continue;  // every key has the same byte here
            for (unsigned b = 1; b < 257; ++b) count[b] += count[b - 1];
            for (std::size_t i = 0; i < n; ++i) spare[count[(pairs[i] >> shift) & 0xFFu]++] = pairs[i];
            pairs.swap(spare);
        }
    }
}

// What one batched draw shares: an item's pipeline, texture set, push constants and scissor.
struct DrawState {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    PushConstants push{};
    VkRect2D scissor{};
    bool ok = false;      // a pipeline draws it
    bool additive = false;  // its blend only adds to the target
    std::uint32_t key = 0;  // its PipelineKey's bits: a line batch's pipeline is the line variant
};
// A frame's distinct states and the order its items are drawn in: the render thread's own (an
// interpolated frame's draw comes after the real one's, on the same thread). Items with the same
// state (same_state) carry the same index, so the regrouping and the batching compare indices.
thread_local std::vector<DrawState> t_states;
constexpr std::uint32_t kNoState = ~0u;  // an item's state: no pipeline draws it

// An item: a polygon, or one triangle of a strip, with its state's index; or a line batch (count
// kLineBatch, first its index in Frame::lines).
constexpr std::uint32_t kLineBatch = ~0u;
struct Item {
    float depth;                 // 1/w: larger is nearer
    std::uint32_t first, count;  // vertices drawn: the whole strip, or one triangle of it
    std::uint32_t state;         // in t_states; kNoState: no pipeline draws it
};
// A translucent item with its vertices' bounds on the screen, for the regrouping, in one record so
// the sorted order reads it once. Bounds that are not all finite are stored as the whole plane.
struct Translucent {
    Item item;
    float x0, y0, x1, y1;
};
// The frame's items in draw order: the render thread's own, as t_states.
thread_local std::vector<Item> t_drawn;

// The translucent items, `sorted` back to front, appended to `out` regrouped by state.
// They write neither depth nor stencil, so two that share no pixel draw the same in either order,
// as do two whose blends only add to the target: each item joins the latest group with its state
// unless a later group overlaps its screen bounds (plus a pixel) and they do not both add, and
// otherwise starts a group. Non-finite bounds overlap everything; items no pipeline draws are left
// out.
void regroup(const std::vector<Translucent>& sorted, std::vector<Item>& out) {
    // How many groups back an item may move; the search stops at the first overlap anyway.
    constexpr std::size_t kReach = 32;
    struct Group {
        std::uint32_t head, tail;  // positions in sorted
        float x0, y0, x1, y1;
        std::uint32_t state;
    };
    thread_local std::vector<Group> groups;
    thread_local std::vector<std::uint32_t> next;
    groups.clear();
    next.assign(sorted.size(), ~0u);
    for (std::uint32_t at = 0; at < sorted.size(); ++at) {
        const Translucent& t = sorted[at];
        const std::uint32_t state = t.item.state;
        if (state == kNoState)
            continue;
        const float b[4] = {t.x0 - 1.0f, t.y0 - 1.0f, t.x1 + 1.0f, t.y1 + 1.0f};
        const bool additive = t_states[state].additive;
        std::size_t found = groups.size();
        for (std::size_t k = groups.size(), steps = 0; k-- > 0 && steps < kReach; ++steps) {
            const Group& g = groups[k];
            if (g.state == state) {
                found = k;
                break;
            }
            if (b[0] < g.x1 && g.x0 < b[2] && b[1] < g.y1 && g.y0 < b[3] &&
                !(additive && t_states[g.state].additive))
                break;  // it would move before something it overlaps
        }
        if (found == groups.size()) {
            groups.push_back({at, at, b[0], b[1], b[2], b[3], state});
            continue;
        }
        Group& g = groups[found];
        next[g.tail] = at;
        g.tail = at;
        g.x0 = std::min(g.x0, b[0]);
        g.y0 = std::min(g.y0, b[1]);
        g.x1 = std::max(g.x1, b[2]);
        g.y1 = std::max(g.y1, b[3]);
    }
    for (const Group& g : groups)
        for (std::uint32_t at = g.head; at != ~0u; at = next[at]) out.push_back(sorted[at].item);
}

}  // namespace

Renderer::~Renderer() {
    destroy();
}

bool Renderer::create(Context& ctx, VkRenderPass render_pass) {
    ctx_ = &ctx;
    render_pass_ = render_pass;
    // The depth is the vertex shader's 1/w; the fragment shaders are specialized per mode
    // (PipelineKey::shader).
    vs_ = make_module(ctx.device(), geometry_vert, sizeof geometry_vert);
    lines_vs_ = make_module(ctx.device(), lines_vert, sizeof lines_vert);
    fs_ = make_module(ctx.device(), geometry_frag, sizeof geometry_frag);
    fs_nd_ = make_module(ctx.device(), geometry_frag_nd, sizeof geometry_frag_nd);
    mv_vs_ = make_module(ctx.device(), modvol_vert, sizeof modvol_vert);
    mv_fs_ = make_module(ctx.device(), modvol_frag, sizeof modvol_frag);
    // The offscreen target picks the same depth format; with a stencil, modifier volumes draw.
    stencil_ = format_has_stencil(pick_depth_format(ctx.physical_device())) && mv_vs_ && mv_fs_;
    if (!vs_ || !fs_) {
        error_ = "failed to create the geometry shader modules";
        return false;
    }

    // One combined image sampler: the polygon's texture.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(ctx.device(), &dslci, nullptr, &set_layout_) != VK_SUCCESS) {
        error_ = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    if (!textures_.create(ctx, set_layout_)) {
        error_ = textures_.error();
        return false;
    }

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.device(), &plci, nullptr, &layout_) != VK_SUCCESS) {
        error_ = "vkCreatePipelineLayout failed";
        return false;
    }
    return true;
}

VkPipeline Renderer::pipeline_for(const PipelineKey& key) {
    const std::uint32_t bits = key.bits();
    TableSlot& slot = pipeline_table_[(bits * 0x9E3779B1u) >> (32 - kTableBits)];
    if (slot.bits == bits)
        return slot.pipeline;
    if (auto it = pipelines_.find(key); it != pipelines_.end()) {
        slot = {bits, it->second};
        return it->second;
    }
    const VkPipeline pipeline = make_pipeline(key, false);
    if (!pipeline)
        return VK_NULL_HANDLE;
    pipelines_.emplace(key, pipeline);
    slot = {bits, pipeline};
    return pipeline;
}

// A state's pipeline for line batches: its own with lines.vert, an instance a line. Looked up once
// a frame per state, so a map will do.
VkPipeline Renderer::line_pipeline_for(const PipelineKey& key) {
    if (auto it = line_pipelines_.find(key.bits()); it != line_pipelines_.end())
        return it->second;
    const VkPipeline pipeline = make_pipeline(key, true);
    if (!pipeline)
        return VK_NULL_HANDLE;
    line_pipelines_.emplace(key.bits(), pipeline);
    return pipeline;
}

VkPipeline Renderer::make_pipeline(const PipelineKey& key, bool lines) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = lines ? lines_vs_ : vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = key.discard || !fs_nd_ ? fs_ : fs_nd_;
    stages[1].pName = "main";
    if (!stages[0].module)
        return VK_NULL_HANDLE;
    // The fragment shader compiled for the key's mode (geometry.frag's kMode).
    const std::int32_t mode = static_cast<std::int32_t>(key.shader);
    const VkSpecializationMapEntry mode_entry{0, 0, sizeof mode};
    VkSpecializationInfo spec{};
    spec.mapEntryCount = 1;
    spec.pMapEntries = &mode_entry;
    spec.dataSize = sizeof mode;
    spec.pData = &mode;
    stages[1].pSpecializationInfo = &spec;

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = static_cast<std::uint32_t>(kVertexBytes);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    // The colours as R8G8B8A8_UNORM (mandatory for vertex buffers): bytes B, G, R, A of the ARGB
    // word, which the vertex shader puts back in order.
    std::array<VkVertexInputAttributeDescription, 5> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, base)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, offset)},
        {4, 0, VK_FORMAT_R32_SFLOAT, sizeof(Vertex)},
    }};
    if (lines) {
        // An instance a line (LineInstance), on binding 1.
        binding.binding = 1;
        binding.stride = static_cast<std::uint32_t>(sizeof(LineInstance));
        binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        attributes = {{
            {0, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LineInstance, a)},
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LineInstance, b)},
            {2, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineInstance, base)},
            {3, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(LineInstance, offset)},
            {4, 1, VK_FORMAT_R32_SFLOAT, offsetof(LineInstance, half_width)},
        }};
    }
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    // Batched draws are indexed triangle lists. A line's quad is a strip of four.
    ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull_flags(key.cull_mode);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = depth_compare(key.depth_mode);
    if (stencil_ && !key.blend) {
        // Opaque and punch-through polygons record whether modifier volumes may shade them:
        // stencil bit 7 = the PCW Shadow bit of the nearest surface (Flycast pipeline.cpp).
        ds.stencilTestEnable = VK_TRUE;
        VkStencilOpState so{};
        so.failOp = VK_STENCIL_OP_KEEP;
        so.passOp = VK_STENCIL_OP_REPLACE;
        so.depthFailOp = VK_STENCIL_OP_KEEP;
        so.compareOp = VK_COMPARE_OP_ALWAYS;
        so.compareMask = 0;
        so.writeMask = 0x80;
        so.reference = key.shadow ? 0x80u : 0u;
        ds.front = so;
        ds.back = so;
    }

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (key.blend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = src_factor(key.src_blend);
        blend.dstColorBlendFactor = dst_factor(key.dst_blend);
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
        blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dy;
    gpci.layout = layout_;
    gpci.renderPass = render_pass_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx_->device(), pipeline_cache_, 1, &gpci, nullptr, &pipeline) !=
        VK_SUCCESS) {
        // prewarm() makes pipelines on several threads.
        static std::mutex error_mutex;
        const std::lock_guard<std::mutex> lock(error_mutex);
        error_ = "vkCreateGraphicsPipelines failed";
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

void Renderer::set_memory(const std::uint8_t* vram, std::size_t vram_size,
                          const std::uint32_t* palette_ram, PaletteFormat palette_format) {
    textures_.set_memory(vram, vram_size, palette_ram, palette_format);
    frame_sets_.clear();
}

// Textures are decoded and uploaded before the render pass begins, because a copy cannot be
// recorded inside one. The descriptor sets are remembered for the draw that follows.
void Renderer::prepare(VkCommandBuffer upload, const Frame& frame) {
    frame_sets_.clear();
    fallback_set_ = textures_.fallback(upload);
    // Two passes, because whether a texture is addressed with repeat or clamp is a property of
    // every polygon that samples it, not of the first one seen. One strip that genuinely wraps is
    // enough to need repeat for all of them; the union is the safe way round, since clamping
    // something that does tile would lose the repetition, while repeating something that does not
    // only costs the seam this is here to remove.
    struct Use {
        std::uint32_t tcw = 0, tsp = 0;
        bool tiled = false;
    };
    std::unordered_map<std::uint64_t, Use> used;
    for (const auto& list : frame.lists)
        for (const Polygon& p : list) {
            if (!pcw_texture(p.pcw))
                continue;
            Use& u = used[texture_key(p)];
            u.tcw = p.tcw;
            u.tsp = p.tsp;
            u.tiled = u.tiled || polygon_tiles(frame, p);
        }
    for (const auto& [key, u] : used)
        frame_sets_[key] = textures_.get(upload, u.tcw, u.tsp, u.tiled);
}

void Renderer::draw(VkCommandBuffer cmd, const Frame& frame, const FrameGeometry& geometry,
                    VkExtent2D target) {
    if (frame.vertices.empty() && frame.line_edges.empty())
        return;

    // Pack the vertices into the shader's layout.
    // Table fog (FogCtrl 0 and 3) is evaluated per vertex from the hardware's table, only when
    // some polygon uses it; the hardware does it per pixel, which this approximates.
    bool table_fog = false;
    for (const auto& list : frame.lists)
        for (const Polygon& p : list) {
            const std::uint32_t f = tsp_fog_control(p.tsp);
            if (f == 0 || f == 3)
                table_fog = true;
        }
    const VkDeviceSize bytes = frame.vertices.size() * kVertexBytes;
    // A ring of vertex buffers: with renders pipelined, and with an interpolated
    // frame drawn beside each real one, the buffer a previous draw is still reading must not be
    // rewritten. Each target waits for its own previous frame, so four in rotation suffice.
    const unsigned ring_slot = ring_next_;
    HostBuffer& vertices_ = vertex_ring_[ring_next_];
    ring_next_ = (ring_next_ + 1) % kVertexRing;
    // Straight into the mapped buffer, in order. A frame of line batches alone has no vertices.
    if (bytes != 0) {
        if (!vertices_.ensure(*ctx_, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true) || !vertices_.mapped())
            return;
        auto* out = static_cast<std::uint8_t*>(vertices_.mapped());
        for (const Vertex& v : frame.vertices) {
            std::memcpy(out, &v, sizeof v);
            const float fog = table_fog ? fog_table_value(geometry.fog, v.z) : 0.0f;
            std::memcpy(out + sizeof v, &fog, sizeof fog);
            out += kVertexBytes;
        }
        vertices_.flush(0, bytes);
    }

    // The guest draws to its own framebuffer size; the window may be any size, and the viewport
    // the caller set already stretches one to the other.
    PushConstants push{};
    push.scale[0] = 2.0f / geometry.width;
    push.scale[1] = 2.0f / geometry.height;
    push.offset[0] = -1.0f - 2.0f * geometry.left / geometry.width;
    push.offset[1] = -1.0f - 2.0f * geometry.top / geometry.height;
    // Fog colours: RGBA8888 with red in the low byte (fog.h).
    for (int c = 0; c < 4; ++c) {
        push.fog_vert[c] =
            static_cast<float>((geometry.fog.vertex_colour >> (8 * c)) & 0xFFu) / 255.0f;
        push.fog_ram[c] = static_cast<float>((geometry.fog.table_colour >> (8 * c)) & 0xFFu) / 255.0f;
    }

    const VkDeviceSize zero = 0;
    VkBuffer buffer = bytes != 0 ? vertices_.handle() : VK_NULL_HANDLE;
    if (buffer)
        vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &zero);

    // The hardware draws the lists in a fixed order and the game relies on it: opaque geometry
    // first, then punch-through (opaque with holes, so it can also write depth), then translucent
    // blended over the result.
    //
    // Translucent polygons are sorted back to front per strip. That is the fallback the hardware
    // does not need: a real PowerVR sorts per pixel, so two translucent surfaces that intersect
    // come out right and a per-strip sort cannot. A per-strip sort is what most Dreamcast
    // rendering used for years and is right for all but intersecting transparency.

    // User tile clip (as Flycast's SetTileClip): mode 2 scissors to the rectangle,
    // mode 3 discards inside it in the shader. Guest pixels to the target's.
    const float sx = static_cast<float>(target.width) / geometry.width;
    const float sy = static_cast<float>(target.height) / geometry.height;
    // No scissor reaches past `area` (a render to texture's region), so nothing outside it is
    // shaded.
    const VkExtent2D limit{area.width ? std::min(area.width, target.width) : target.width,
                           area.height ? std::min(area.height, target.height) : target.height};
    const VkRect2D full_scissor{{0, 0}, limit};
    VkRect2D scissor_now = full_scissor;
    auto clip_rect = [&](const Polygon& p) {
        const float x0 = std::max(0.0f, (static_cast<float>(p.clip_x0) - geometry.left) * sx);
        const float y0 = std::max(0.0f, (static_cast<float>(p.clip_y0) - geometry.top) * sy);
        const float x1 = std::min(static_cast<float>(limit.width),
                                  (static_cast<float>(p.clip_x1) - geometry.left) * sx);
        const float y1 = std::min(static_cast<float>(limit.height),
                                  (static_cast<float>(p.clip_y1) - geometry.top) * sy);
        VkRect2D r{};
        r.offset = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)};
        r.extent = {static_cast<std::uint32_t>(std::max(0.0f, x1 - x0)),
                    static_cast<std::uint32_t>(std::max(0.0f, y1 - y0))};
        return r;
    };
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    bool volumes_drawn = false;
    auto same_rect = [](const VkRect2D& a, const VkRect2D& b) {
        return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
               a.extent.height == b.extent.height;
    };

    // What a polygon of a list (blended, alpha_test) draws with, or an odd triangle of it drawn on
    // its own: its pipeline, its texture's descriptor set (or none), the push constants and the
    // scissor. False when no pipeline can draw it.
    std::uint32_t last_key = ~0u;  // state_of: the last pipeline key looked up, and its pipeline
    VkPipeline last_pipeline = VK_NULL_HANDLE;
    const auto state_of = [&](const Polygon& p, bool blended, bool alpha_test, bool odd, DrawState& st) {
        PipelineKey key{};
        key.depth_mode = isp_depth_mode(p.isp) & 7u;
        // As Flycast (pipeline.cpp): punch-through always GREATER_EQUAL with depth writes;
        // translucent GREATER_EQUAL when the hardware sorts it.
        if (alpha_test || (blended && geometry.autosort))
            key.depth_mode = 6u;
        // A translucent surface tests depth but does not write it: writing would hide surfaces
        // behind it that still have to be blended in.
        key.depth_write = alpha_test ? 1u : (blended || isp_z_write_disable(p.isp)) ? 0u : 1u;
        key.cull_mode = isp_cull_mode(p.isp) & 3u;
        // An odd triangle of a strip, drawn on its own, has the opposite winding: swap which
        // side is culled (2 = cull if negative, 3 = cull if positive).
        if (odd && key.cull_mode >= 2u)
            key.cull_mode ^= 1u;
        key.blend = blended ? 1u : 0u;
        key.src_blend = tsp_src_instr(p.tsp) & 7u;
        key.dst_blend = tsp_dst_instr(p.tsp) & 7u;
        // Destination factor one, and a source factor not taken from the target.
        st.additive = blended && key.dst_blend == 1u &&
                      (key.src_blend == 0u || key.src_blend == 1u || key.src_blend == 4u || key.src_blend == 5u);
        key.shadow = (stencil_ && !blended) ? (pcw_shadow(p.pcw) & 1u) : 0u;
        const std::uint32_t mode = p.tile_clip & 3u;
        key.discard = (alpha_test || mode == 3) ? 1u : 0u;  // the shader's two discards
        key.padding = 0;
        // A polygon whose texture could not be decoded draws untextured rather than not at all.
        st.set = VK_NULL_HANDLE;
        if (pcw_texture(p.pcw)) {
            const auto it = frame_sets_.find(texture_key(p));
            if (it != frame_sets_.end())
                st.set = it->second;
        }
        st.push = push;
        st.push.mode = 0;
        if (st.set) {
            st.push.mode = static_cast<std::int32_t>(tsp_shading_instruction(p.tsp)) | 4;
            if (tsp_ignore_texture_alpha(p.tsp))
                st.push.mode |= 8;
        }
        // The offset colour exists only for textured polygons (Flycast reads it inside its texture
        // block; an untextured header's offset word is unused).
        if (pcw_offset(p.pcw) && pcw_texture(p.pcw))
            st.push.mode |= 16;
        if (alpha_test)
            st.push.mode |= 32;
        if (tsp_use_alpha(p.tsp))
            st.push.mode |= 64;
        st.scissor = mode == 2 ? clip_rect(p) : full_scissor;
        if (mode == 3) {
            const VkRect2D r = clip_rect(p);
            st.push.mode |= 512;
            st.push.clip[0] = static_cast<float>(r.offset.x);
            st.push.clip[1] = static_cast<float>(r.offset.y);
            st.push.clip[2] = static_cast<float>(r.offset.x) + static_cast<float>(r.extent.width);
            st.push.clip[3] = static_cast<float>(r.offset.y) + static_cast<float>(r.extent.height);
        } else {
            // Unused without mode 3; cleared so that it does not keep runs apart.
            st.push.clip[0] = st.push.clip[1] = st.push.clip[2] = st.push.clip[3] = 0.0f;
        }
        // Fog control in bits 7-8 (2 = none).
        st.push.mode |= static_cast<std::int32_t>((tsp_fog_control(p.tsp) & 3u) << 7);
        st.push.alpha_ref = geometry.alpha_ref;
        if (!st.set)
            st.set = fallback_set_;
        // The shader's mode in the key, with the bits that change no pixel cleared so
        // they make no pipelines of their own: per-vertex fog without the offset colour it is
        // taken from is no fog.
        std::uint32_t shader = static_cast<std::uint32_t>(st.push.mode) & 0x3FFu;
        if (((shader >> 7) & 3u) == 1u && !(shader & 16u))
            shader = (shader & ~(3u << 7)) | (2u << 7);
        key.shader = shader & 0x3FFu;
        // The item before's pipeline when the key is its (most are).
        if (key.bits() != last_key) {
            last_key = key.bits();
            last_pipeline = pipeline_for(key);
        }
        st.pipeline = last_pipeline;
        st.key = key.bits();
        st.ok = st.pipeline != VK_NULL_HANDLE;
        return st.ok;
    };
    // The same state for a run: everything but the base push constants, which one frame shares.
    const auto same_state = [&](const DrawState& a, const DrawState& b) {
        return a.pipeline == b.pipeline && a.set == b.set && a.push.mode == b.push.mode &&
               a.push.clip[0] == b.push.clip[0] && a.push.clip[1] == b.push.clip[1] &&
               a.push.clip[2] == b.push.clip[2] && a.push.clip[3] == b.push.clip[3] &&
               same_rect(a.scissor, b.scissor);
    };
    // A hash of what same_state compares: states it calls the same hash the same (a zero clip
    // edge as +0, since -0 == +0).
    const auto state_key = [](const DrawState& s) {
        std::uint64_t h = 0;
        const auto mix = [&h](std::uint64_t v) {
            h = (h ^ v) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        };
        const auto handle = [](const auto& x) {
            std::uint64_t v = 0;
            std::memcpy(&v, &x, sizeof x < sizeof v ? sizeof x : sizeof v);
            return v;
        };
        const auto edge = [](float f) {
            std::uint32_t u = 0;
            if (f != 0.0f)
                std::memcpy(&u, &f, sizeof u);
            return std::uint64_t{u};
        };
        mix(handle(s.pipeline));
        mix(handle(s.set));
        mix(static_cast<std::uint32_t>(s.push.mode));
        mix(edge(s.push.clip[0]) | edge(s.push.clip[1]) << 32);
        mix(edge(s.push.clip[2]) | edge(s.push.clip[3]) << 32);
        mix(static_cast<std::uint32_t>(s.scissor.offset.x) |
            std::uint64_t{static_cast<std::uint32_t>(s.scissor.offset.y)} << 32);
        mix(s.scissor.extent.width | std::uint64_t{s.scissor.extent.height} << 32);
        return h;
    };

    // Each polygon's state, kept once per distinct state. The state before is tried
    // first (lists repeat states), then those with the same key, compared in full. The keys' table:
    // open addressing, 2^12 slots, an index per slot (kNoState: free), emptied by a frame stamp
    // rather than cleared; a frame with more states than half of it takes no more (they each get
    // their own index, compared with no others: fewer merges, same picture).
    constexpr std::uint32_t kSlots = 1u << 12;
    struct Slot {
        std::uint64_t key;
        std::uint32_t id, stamp;
    };
    thread_local std::vector<Slot> slots(kSlots, Slot{0, kNoState, 0});
    thread_local std::uint32_t stamp = 0;
    if (++stamp == 0) {
        std::fill(slots.begin(), slots.end(), Slot{0, kNoState, 0});
        stamp = 1;
    }
    thread_local std::vector<std::uint32_t> next_of;  // index: the next with the same key
    t_states.clear();
    next_of.clear();
    std::uint32_t prev = kNoState;
    const auto intern = [&](const DrawState& cur) {
        if (prev != kNoState && same_state(cur, t_states[prev]))
            return prev;
        const std::uint64_t key = state_key(cur);
        Slot* slot = nullptr;
        if (t_states.size() < kSlots / 2) {
            for (std::uint32_t s = static_cast<std::uint32_t>(key) & (kSlots - 1);; s = (s + 1) & (kSlots - 1))
                if (slots[s].stamp != stamp || slots[s].key == key) {
                    slot = &slots[s];
                    break;
                }
        }
        const bool fresh = !slot || slot->stamp != stamp;
        std::uint32_t id = kNoState;
        if (!fresh)
            for (std::uint32_t j = slot->id; j != kNoState && id == kNoState; j = next_of[j])
                if (same_state(cur, t_states[j]))
                    id = j;
        if (id == kNoState) {
            id = static_cast<std::uint32_t>(t_states.size());
            t_states.push_back(cur);
            next_of.push_back(kNoState);
            if (!fresh) {
                next_of[id] = next_of[slot->id];
                next_of[slot->id] = id;
            } else if (slot) {
                *slot = Slot{key, id, stamp};
            }
        }
        prev = id;
        return id;
    };

    // The items (Item): the opaque and punch-through lists' straight into the draw order, the
    // translucent list's (Translucent: with their bounds, taken while their vertices are read for
    // the item anyway) aside for the sort. Kept from frame to frame. `indices`: the batched
    // draws'.
    std::vector<Item>& items = t_drawn;
    thread_local std::vector<Translucent> translucent;
    thread_local std::vector<std::uint64_t> pairs;  // their sort keys (sort_back_to_front)
    items.clear();
    translucent.clear();
    pairs.clear();
    std::size_t indices = 0;
    // A translucent item's bounds, grown vertex by vertex.
    struct Box {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        bool finite = true;
        void grow(float x, float y) {
            finite = finite && std::isfinite(x) && std::isfinite(y);
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
        void grow(const Vertex& v) { grow(v.x, v.y); }
    };
    // The guest region shown, a pixel wider on each side: lines wholly outside it draw nothing.
    // NaN compares false, so a line with one is kept.
    const float region_x0 = geometry.left - 1.0f, region_y0 = geometry.top - 1.0f;
    const float region_x1 = geometry.left + geometry.width + 1.0f;
    const float region_y1 = geometry.top + geometry.height + 1.0f;
    const auto box_off_region = [&](const Box& bb) {
        return bb.finite && (bb.x1 < region_x0 || bb.x0 > region_x1 || bb.y1 < region_y0 || bb.y0 > region_y1);
    };
    // A line's quad lies within a half width of its ends (lines.vert).
    const auto line_off_region = [&](const LineEdge& le, float hw) {
        return (le.xa + hw < region_x0 && le.xb + hw < region_x0) ||
               (le.xa - hw > region_x1 && le.xb - hw > region_x1) ||
               (le.ya + hw < region_y0 && le.yb + hw < region_y0) ||
               (le.ya - hw > region_y1 && le.yb - hw > region_y1);
    };
    const auto add_translucent = [&](const Item& item, const Box& bb) {
        constexpr float kInf = std::numeric_limits<float>::infinity();
        pairs.push_back(std::uint64_t{depth_key(item.depth)} << 32 | translucent.size());
        if (bb.finite)
            translucent.push_back({item, bb.x0, bb.y0, bb.x1, bb.y1});
        else
            translucent.push_back({item, -kInf, -kInf, kInf, kInf});
    };
    // Translucent geometry with the hardware's autosort is drawn per triangle. The PowerVR sorts
    // every pixel; one key for a whole strip puts a large strip on the wrong side of what it
    // crosses.
    const bool per_triangle = geometry.autosort;
    // What state_of reads of a polygon: the polygons with the same draw the same.
    const auto same_header = [](const Polygon& a, const Polygon& b) {
        return a.pcw == b.pcw && a.isp == b.isp && a.tsp == b.tsp && a.tcw == b.tcw && a.tile_clip == b.tile_clip &&
               a.clip_x0 == b.clip_x0 && a.clip_y0 == b.clip_y0 && a.clip_x1 == b.clip_x1 && a.clip_y1 == b.clip_y1;
    };
    const auto add_list = [&](ListType list, bool blended, bool alpha_test) {
        // Room in the index buffer for an item of `vertices` vertices that a pipeline draws.
        const auto count = [&](std::uint32_t id, std::uint32_t vertices) {
            if (id != kNoState && vertices >= 3)
                indices += std::size_t{vertices - 2} * 3;
        };
        // The polygon's state, and its odd triangles' when they cull the other side: the last
        // polygon's while the words they come from repeat (a model's strips mostly share them).
        const Polygon* last = nullptr;
        std::uint32_t even = kNoState, odd = kNoState;
        bool odd_known = false;
        for (const Polygon& p : frame.lists[static_cast<unsigned>(list)]) {
            if (!last || !same_header(*last, p)) {
                DrawState st;
                even = state_of(p, blended, alpha_test, false, st) ? intern(st) : kNoState;
                odd_known = false;
                last = &p;
            }
            if (p.line_batch) {
                // A line batch: one item, sorted by its ends' mean 1/w; its bounds its ends', a
                // half width around. One wholly outside the region is left out.
                const LineBatch& lb = frame.lines[p.first];
                float sum = 0.0f;
                Box bb;
                for (std::uint32_t e = lb.first; e < lb.first + lb.count; ++e) {
                    const LineEdge& le = frame.line_edges[e];
                    sum += le.za + le.zb;
                    bb.grow(le.xa, le.ya);
                    bb.grow(le.xb, le.yb);
                }
                const float hw = std::abs(lb.half_width);
                bb.x0 -= hw;
                bb.y0 -= hw;
                bb.x1 += hw;
                bb.y1 += hw;
                if (box_off_region(bb))
                    continue;
                const Item item{lb.count ? sum / static_cast<float>(2 * lb.count) : 0.0f, p.first, kLineBatch, even};
                if (blended)
                    add_translucent(item, bb);
                else
                    items.push_back(item);
                continue;
            }
            if (blended && per_triangle && p.count > 3) {
                if (!odd_known) {
                    odd = even;
                    DrawState st;
                    if ((isp_cull_mode(p.isp) & 3u) >= 2u)
                        odd = state_of(p, blended, alpha_test, true, st) ? intern(st) : kNoState;
                    odd_known = true;
                }
                for (std::uint32_t i = 0; i + 2 < p.count; ++i) {
                    const Vertex& a = frame.vertices[p.first + i];
                    const Vertex& b = frame.vertices[p.first + i + 1];
                    const Vertex& c = frame.vertices[p.first + i + 2];
                    // Strip joins repeat a vertex: those triangles have no area.
                    if ((a.x == b.x && a.y == b.y) || (b.x == c.x && b.y == c.y) ||
                        (a.x == c.x && a.y == c.y))
                        continue;
                    const std::uint32_t id = (i & 1u) ? odd : even;
                    count(id, 3u);
                    Box bb;
                    bb.grow(a);
                    bb.grow(b);
                    bb.grow(c);
                    add_translucent({(a.z + b.z + c.z) * (1.0f / 3.0f), p.first + i, 3u, id}, bb);
                }
                continue;
            }
            // Sort key: the farthest point, as Flycast's sorter uses (minZ): a large
            // strip reaching toward the camera must not be drawn over what lies in front of
            // most of it.
            float nearest = 1e30f;
            for (std::uint32_t i = 0; i < p.count; ++i)
                nearest = std::min(nearest, frame.vertices[p.first + i].z);
            // A piece clip_far_polygons cut sorts as the triangle (or strip) it came from.
            const float key = per_triangle ? p.sort_triangle : p.sort_strip;
            if (blended && !std::isnan(key))
                nearest = key;
            const Item item{nearest, p.first, p.count, even};
            count(even, p.count);
            if (!blended) {
                items.push_back(item);
                continue;
            }
            Box bb;
            for (std::uint32_t q = 0; q < p.count; ++q) bb.grow(frame.vertices[p.first + q]);
            add_translucent(item, bb);
        }
    };
    add_list(ListType::Opaque, false, false);
    add_list(ListType::PunchThrough, false, true);
    const std::size_t translucent_start = items.size();
    add_list(ListType::Translucent, true, false);

    // The draw order: the opaque and punch-through items as they came, then the translucent ones
    // back to front (the farthest surface has the smallest 1/w and must be drawn first), batched:
    // regrouped by state. The records are copied into the sorted order, prefetched a few records
    // ahead.
    thread_local std::vector<Translucent> in_order;
    const std::size_t n_translucent = translucent.size();
    sort_back_to_front(pairs);
    in_order.resize(n_translucent);
    constexpr std::size_t kAhead = 32;
    for (std::size_t k = 0; k < n_translucent; ++k) {
#if defined(__GNUC__) || defined(__clang__)
        if (k + kAhead < n_translucent)
            __builtin_prefetch(&translucent[static_cast<std::uint32_t>(pairs[k + kAhead])]);
#endif
        in_order[k] = translucent[static_cast<std::uint32_t>(pairs[k])];
    }
    regroup(in_order, items);

    // Batched draws: each item's triangles, in draw order, as a list, written straight
    // into the frame's index buffer as the draws are recorded, and only for items that are drawn.
    // A strip's triangle j is (j, j+1, j+2) when j is even and (j, j+2, j+1) when odd: Vulkan's own
    // order for strips, so the winding and the first (provoking) vertex are those of the strip
    // draw. A single triangle of a strip (per-triangle autosort) is its three vertices in order;
    // its pipeline already culls the other side when it was odd.
    std::uint32_t* idx = nullptr;
    HostBuffer* ib = nullptr;
    if (indices != 0) {
        ib = &index_ring_[ring_slot];
        if (!ib->ensure(*ctx_, indices * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true) ||
            !ib->mapped())
            return;
        idx = static_cast<std::uint32_t*>(ib->mapped());
        vkCmdBindIndexBuffer(cmd, ib->handle(), 0, VK_INDEX_TYPE_UINT32);
    }
    std::uint32_t at = 0;  // the next index written
    // Batched draws: a run of consecutive items with the same pipeline, texture, push
    // constants and scissor, whose triangles follow each other in the index buffer, drawn at once.
    struct Run {
        const DrawState* state = nullptr;
        std::uint32_t id = kNoState;  // the state's index in t_states
        std::uint32_t first = 0, count = 0;
    } run;
    // The push constants last pushed: they stay across pipelines of one layout, so a
    // draw whose are the same pushes none. Not valid after the modifier volumes,
    // which push their own.
    PushConstants pushed{};
    bool pushed_valid = false;
    // A state's pipeline (`pipeline`: its own, or its line variant), texture, scissor and push
    // constants, as far as they differ from what is bound.
    const auto bind_state = [&](const DrawState& st, VkPipeline pipeline) {
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        if (st.set && st.set != bound_set) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &st.set, 0,
                                    nullptr);
            bound_set = st.set;
        }
        if (!same_rect(st.scissor, scissor_now)) {
            vkCmdSetScissor(cmd, 0, 1, &st.scissor);
            scissor_now = st.scissor;
        }
        if (!pushed_valid || std::memcmp(&pushed, &st.push, sizeof pushed) != 0) {
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof st.push, &st.push);
            pushed = st.push;
            pushed_valid = true;
        }
    };
    auto flush = [&] {
        if (run.count == 0)
            return;
        bind_state(*run.state, run.state->pipeline);
        vkCmdDrawIndexed(cmd, run.count, 1, run.first, 0, 0);
        run.count = 0;
    };
    // The line batches' lines: an instance each, with their ends' table fog, the batch's colours
    // and half width; on binding 1, which only the line pipelines read. Written in draw order as
    // the items are recorded, so consecutive line items with one state are one draw of four
    // vertices an instance (lines.vert). Lines wholly outside the region are left out.
    LineInstance* li = nullptr;
    HostBuffer* lb = nullptr;
    std::uint32_t line_at = 0;  // the next instance written
    thread_local std::vector<VkPipeline> line_pipes;  // each state's line pipeline, once a frame
    thread_local std::vector<std::uint8_t> line_piped;
    if (!frame.line_edges.empty()) {
        lb = &line_ring_[ring_slot];
        const VkDeviceSize lbytes = frame.line_edges.size() * sizeof(LineInstance);
        if (!lb->ensure(*ctx_, lbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true) || !lb->mapped())
            return;
        li = static_cast<LineInstance*>(lb->mapped());
        const VkDeviceSize zero_offset = 0;
        const VkBuffer lbuf = lb->handle();
        vkCmdBindVertexBuffers(cmd, 1, 1, &lbuf, &zero_offset);
        line_pipes.assign(t_states.size(), VK_NULL_HANDLE);
        line_piped.assign(t_states.size(), 0);
    }
    // Instances [first, first + count), drawn with one state's line pipeline.
    struct LineRun {
        std::uint32_t id = kNoState;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::uint32_t first = 0, count = 0;
    } line_run;
    const auto flush_lines = [&] {
        if (line_run.count == 0)
            return;
        bind_state(t_states[line_run.id], line_run.pipeline);
        vkCmdDraw(cmd, 4, line_run.count, 0, line_run.first);
        line_run.count = 0;
    };
    // Appends an item's lines to the run, drawing the run first when the state changes. An item no
    // pipeline draws keeps the run open.
    const auto add_lines = [&](const Item& item) {
        if (!li || item.state == kNoState)
            return;
        const LineBatch& b = frame.lines[item.first];
        if (b.count == 0)
            return;
        if (!line_piped[item.state]) {
            line_piped[item.state] = 1;
            line_pipes[item.state] = line_pipeline_for(key_from_bits(t_states[item.state].key));
        }
        const VkPipeline lp = line_pipes[item.state];
        if (!lp)
            return;
        if (line_run.count != 0 && line_run.id != item.state)
            flush_lines();
        if (line_run.count == 0) {
            line_run.id = item.state;
            line_run.pipeline = lp;
            line_run.first = line_at;
        }
        const float hw = std::abs(b.half_width);
        for (std::uint32_t e = b.first; e < b.first + b.count; ++e) {
            const LineEdge& le = frame.line_edges[e];
            if (line_off_region(le, hw))
                continue;
            LineInstance& o = li[line_at++];
            o.a[0] = le.xa;
            o.a[1] = le.ya;
            o.a[2] = le.za;
            o.a[3] = table_fog ? fog_table_value(geometry.fog, le.za) : 0.0f;
            o.b[0] = le.xb;
            o.b[1] = le.yb;
            o.b[2] = le.zb;
            o.b[3] = table_fog ? fog_table_value(geometry.fog, le.zb) : 0.0f;
            o.base = b.base;
            o.offset = b.offset;
            o.half_width = b.half_width;
            ++line_run.count;
        }
    };
    const std::size_t draws = items.size();
    for (std::size_t index = 0; index < draws; ++index) {
        const Item& item = items[index];
        if (!volumes_drawn && index == translucent_start) {
            flush();
            flush_lines();
            // After the opaque and punch-through lists, before the translucent one (Flycast).
            if (!same_rect(scissor_now, full_scissor)) {
                vkCmdSetScissor(cmd, 0, 1, &full_scissor);
                scissor_now = full_scissor;
            }
            draw_modifier_volumes(cmd, frame, geometry, buffer);
            volumes_drawn = true;
            bound = VK_NULL_HANDLE;
            bound_set = VK_NULL_HANDLE;
            pushed_valid = false;
        }
        if (item.count == kLineBatch) {
            flush();
            add_lines(item);
            continue;
        }
        const std::uint32_t id = item.state;
        if (id == kNoState || item.count < 3)
            continue;  // no pipeline, or fewer than three vertices: nothing drawn
        flush_lines();
        const std::uint32_t first_index = at;
        for (std::uint32_t j = 0; j + 2 < item.count; ++j) {
            const std::uint32_t v = item.first + j;
            idx[at] = v;
            idx[at + 1] = (j & 1u) ? v + 2 : v + 1;
            idx[at + 2] = (j & 1u) ? v + 1 : v + 2;
            at += 3;
        }
        // Each item's indices follow the last drawn item's, so a run grows while the state holds.
        if (run.count != 0 && id == run.id) {
            run.count += at - first_index;
        } else {
            flush();
            run.state = &t_states[id];
            run.id = id;
            run.first = first_index;
            run.count = at - first_index;
        }
    }
    flush();
    flush_lines();
    if (ib)
        ib->flush(0, std::size_t{at} * sizeof(std::uint32_t));
    if (lb)
        lb->flush(0, std::size_t{line_at} * sizeof(LineInstance));
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);
    if (!volumes_drawn)
        draw_modifier_volumes(cmd, frame, geometry, buffer);
}

VkPipeline Renderer::modvol_pipeline(ModVol mode, std::uint32_t cull) {
    const std::uint32_t key = static_cast<std::uint32_t>(mode) * 4u + (cull & 3u);
    if (auto it = modvol_pipelines_.find(key); it != modvol_pipelines_.end())
        return it->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mv_vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mv_fs_;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull_flags(cull);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Stencil states copied from Flycast's Vulkan renderer (pipeline.cpp CreateModVolPipeline):
    // (fail, pass, depth fail, compare, compare mask, write mask, reference).
    auto op = [](VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp,
                 std::uint32_t cmask, std::uint32_t wmask, std::uint32_t ref) {
        VkStencilOpState s{};
        s.failOp = fail;
        s.passOp = pass;
        s.depthFailOp = dfail;
        s.compareOp = cmp;
        s.compareMask = cmask;
        s.writeMask = wmask;
        s.reference = ref;
        return s;
    };
    VkStencilOpState so{};
    switch (mode) {
        case ModVol::Xor:
            so = op(VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_KEEP,
                    VK_COMPARE_OP_ALWAYS, 0, 2, 2);
            break;
        case ModVol::Or:
            so = op(VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
                    VK_COMPARE_OP_ALWAYS, 2, 2, 2);
            break;
        case ModVol::Inclusion:
            so = op(VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_ZERO,
                    VK_COMPARE_OP_LESS_OR_EQUAL, 3, 3, 1);
            break;
        case ModVol::Exclusion:
            so = op(VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO,
                    VK_COMPARE_OP_EQUAL, 3, 3, 1);
            break;
        case ModVol::Final:
            so = op(VK_STENCIL_OP_ZERO, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_ZERO,
                    VK_COMPARE_OP_EQUAL, 0x81, 3, 0x81);
            break;
    }
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (mode == ModVol::Xor || mode == ModVol::Or) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER;  // nearer is larger, as for the geometry
    ds.stencilTestEnable = VK_TRUE;
    ds.front = so;
    ds.back = so;
    VkPipelineColorBlendAttachmentState blend{};
    if (mode == ModVol::Final) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dy;
    gpci.layout = layout_;
    gpci.renderPass = render_pass_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx_->device(), pipeline_cache_, 1, &gpci, nullptr, &pipeline) !=
        VK_SUCCESS) {
        error_ = "vkCreateGraphicsPipelines failed (modifier volume)";
        return VK_NULL_HANDLE;
    }
    modvol_pipelines_.emplace(key, pipeline);
    return pipeline;
}

void Renderer::draw_modifier_volumes(VkCommandBuffer cmd, const Frame& frame,
                                     const FrameGeometry& geometry, VkBuffer geometry_buffer) {
    if (!stencil_ || frame.modifiers.empty())
        return;
    // Only the opaque modifier list shades (Flycast draws global_param_mvo only).
    modvol_tris_.clear();
    for (const ModifierTriangle& t : frame.modifiers)
        if (!t.translucent)
            modvol_tris_.push_back(t);
    if (modvol_tris_.empty())
        return;
    const std::vector<ModifierTriangle>& mods = modvol_tris_;
    // Triangles, then the final quad as two more, all in one buffer: x, y in pixels, z = 1/w.
    const std::size_t tris = mods.size();
    modvol_staging_.resize((tris + 2) * 9);
    float* out = modvol_staging_.data();
    for (const ModifierTriangle& t : mods)
        for (unsigned i = 0; i < 3; ++i) {
            *out++ = t.x[i];
            *out++ = t.y[i];
            *out++ = t.z[i];
        }
    const float l = geometry.left, t = geometry.top;
    const float r = geometry.left + geometry.width, b = geometry.top + geometry.height;
    const float quad[18] = {l, t, 1, r, t, 1, l, b, 1, r, t, 1, r, b, 1, l, b, 1};
    std::copy(std::begin(quad), std::end(quad), out);
    HostBuffer& buf = modvol_ring_[modvol_next_];
    modvol_next_ = (modvol_next_ + 1) % kModvolRing;
    const VkDeviceSize bytes = modvol_staging_.size() * sizeof(float);
    if (!buf.ensure(*ctx_, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true))
        return;
    buf.write(modvol_staging_.data(), static_cast<std::size_t>(bytes));
    buf.flush(0, bytes);
    const VkDeviceSize zero = 0;
    VkBuffer vb = buf.handle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);

    PushConstants push{};
    push.scale[0] = 2.0f / geometry.width;
    push.scale[1] = 2.0f / geometry.height;
    push.offset[0] = -1.0f - 2.0f * geometry.left / geometry.width;
    push.offset[1] = -1.0f - 2.0f * geometry.top / geometry.height;
    push.alpha_ref = 1.0f - geometry.shadow_scale;
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof push, &push);

    // One "parameter" per modifier header, as in Flycast's DrawModVols.
    std::size_t base = SIZE_MAX;  // first triangle of the volume being summed
    VkPipeline bound = VK_NULL_HANDLE;
    auto bind = [&](VkPipeline p) {
        if (p && p != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound = p;
        }
        return p != VK_NULL_HANDLE;
    };
    for (std::size_t first = 0; first < tris;) {
        std::size_t end = first + 1;
        while (end < tris && mods[end].header == mods[first].header)
            ++end;
        const std::uint32_t isp = mods[first].isp;
        const std::uint32_t mv_mode = isp >> 29;            // volume instruction
        const bool volume_last = mods[first].volume_last;  // header PCW Volume bit
        const std::uint32_t cull = (isp >> 27) & 3u;
        if (base == SIZE_MAX)
            base = first;
        if (bind(modvol_pipeline(!volume_last && mv_mode > 0 ? ModVol::Or : ModVol::Xor, cull)))
            vkCmdDraw(cmd, static_cast<std::uint32_t>((end - first) * 3), 1,
                      static_cast<std::uint32_t>(first * 3), 0);
        if (mv_mode == 1 || mv_mode == 2) {
            if (bind(modvol_pipeline(mv_mode == 1 ? ModVol::Inclusion : ModVol::Exclusion, cull)))
                vkCmdDraw(cmd, static_cast<std::uint32_t>((end - base) * 3), 1,
                          static_cast<std::uint32_t>(base * 3), 0);
            base = SIZE_MAX;
        }
        first = end;
    }
    if (bind(modvol_pipeline(ModVol::Final, 0)))
        vkCmdDraw(cmd, 6, 1, static_cast<std::uint32_t>(tris * 3), 0);
    if (geometry_buffer)  // none in a frame of line batches alone
        vkCmdBindVertexBuffers(cmd, 0, 1, &geometry_buffer, &zero);
}

namespace {
constexpr std::uint32_t kCacheMagic = 0x43505244u;  // "DRPC"
constexpr std::uint32_t kCacheVersion = 3;  // keys with the discard bit (14) and shader (16-25)
}  // namespace

void Renderer::use_pipeline_cache(const std::string& path) {
    if (!ctx_ || !ctx_->device() || path.empty())
        return;
    pipeline_cache_path_ = path;
    std::vector<std::uint32_t> keys;
    std::vector<char> blob;
    {
        std::ifstream in(path, std::ios::binary);
        const std::vector<char> data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        auto word = [&](std::size_t i) {
            std::uint32_t w = 0;
            std::memcpy(&w, data.data() + i * 4, 4);
            return w;
        };
        if (data.size() >= 12 && word(0) == kCacheMagic && word(1) == kCacheVersion) {
            const std::uint32_t n = word(2);
            if (data.size() >= 16 + 4ull * n) {
                for (std::uint32_t i = 0; i < n; ++i) keys.push_back(word(3 + i));
                const std::uint32_t bytes = word(3 + n);
                if (data.size() >= 16 + 4ull * n + bytes)
                    blob.assign(data.begin() + 16 + 4 * n, data.begin() + 16 + 4 * n + bytes);
            }
        }
    }
    VkPipelineCacheCreateInfo pcci{};
    pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    // A cache from another driver or GPU is ignored by the driver itself (its header says whose).
    pcci.initialDataSize = blob.size();
    pcci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(ctx_->device(), &pcci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        pcci.initialDataSize = 0;
        pcci.pInitialData = nullptr;
        if (vkCreatePipelineCache(ctx_->device(), &pcci, nullptr, &pipeline_cache_) != VK_SUCCESS)
            pipeline_cache_ = VK_NULL_HANDLE;
    }
    prewarmed += prewarm(keys);
}

// The pipelines not made yet, made on up to four threads: Vulkan lets pipelines be
// created on any thread, the cache being synchronized internally.
std::uint32_t Renderer::prewarm(const std::vector<std::uint32_t>& keys) {
    struct Job {
        PipelineKey key;
        bool lines;  // bit 15: the key's line variant (line_pipeline_for)
        VkPipeline made;
    };
    std::vector<Job> jobs;
    for (std::uint32_t bits : keys) {
        const PipelineKey k = key_from_bits(bits);
        const bool lines = (bits & kLineKeyBit) != 0;
        if (lines ? line_pipelines_.count(k.bits()) != 0 : pipelines_.count(k) != 0)
            continue;
        if (std::none_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.key == k && j.lines == lines; }))
            jobs.push_back({k, lines, VK_NULL_HANDLE});
    }
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t i; (i = next.fetch_add(1)) < jobs.size();)
            jobs[i].made = make_pipeline(jobs[i].key, jobs[i].lines);
    };
    const unsigned helpers = std::min(3u, std::max(1u, std::thread::hardware_concurrency()) - 1u);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < helpers && t + 1 < jobs.size(); ++t) threads.emplace_back(work);
    work();
    for (std::thread& t : threads) t.join();
    std::uint32_t made = 0;
    for (const Job& j : jobs) {
        if (!j.made)
            continue;
        if (j.lines)
            line_pipelines_.emplace(j.key.bits(), j.made);
        else
            pipelines_.emplace(j.key, j.made);
        ++made;
    }
    return made;
}

Renderer::PipelineKey Renderer::key_from_bits(std::uint32_t bits) {
    PipelineKey k{};
    k.depth_mode = bits & 7u;
    k.depth_write = (bits >> 3) & 1u;
    k.cull_mode = (bits >> 4) & 3u;
    k.src_blend = (bits >> 6) & 7u;
    k.dst_blend = (bits >> 9) & 7u;
    k.blend = (bits >> 12) & 1u;
    k.shadow = (bits >> 13) & 1u;
    k.discard = (bits >> 14) & 1u;
    k.shader = (bits >> 16) & 0x3FFu;
    k.padding = 0;
    return k;
}

void Renderer::save_pipeline_cache() {
    if (pipeline_cache_path_.empty() || !pipeline_cache_)
        return;
    std::size_t size = 0;
    std::vector<char> blob;
    if (vkGetPipelineCacheData(ctx_->device(), pipeline_cache_, &size, nullptr) == VK_SUCCESS &&
        size > 0) {
        blob.resize(size);
        if (vkGetPipelineCacheData(ctx_->device(), pipeline_cache_, &size, blob.data()) !=
            VK_SUCCESS)
            blob.clear();
        blob.resize(std::min(blob.size(), size));
    }
    std::vector<std::uint32_t> words{kCacheMagic, kCacheVersion,
                                     static_cast<std::uint32_t>(pipelines_.size())};
    for (const auto& [key, pipeline] : pipelines_) words.push_back(key.bits());
    words.push_back(static_cast<std::uint32_t>(blob.size()));
    // Written to a temporary name and renamed, so a crash mid-write never leaves a torn cache.
    const std::string tmp = pipeline_cache_path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(words.data()),
                  static_cast<std::streamsize>(words.size() * 4));
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!out)
            return;
    }
    std::remove(pipeline_cache_path_.c_str());
    std::rename(tmp.c_str(), pipeline_cache_path_.c_str());
}

void Renderer::destroy() {
    if (!ctx_ || !ctx_->device())
        return;
    vkDeviceWaitIdle(ctx_->device());
    save_pipeline_cache();
    if (pipeline_cache_) {
        vkDestroyPipelineCache(ctx_->device(), pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
    }
    for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(ctx_->device(), pipeline, nullptr);
    pipelines_.clear();
    pipeline_table_.fill(TableSlot{});
    for (auto& [key, pipeline] : line_pipelines_) vkDestroyPipeline(ctx_->device(), pipeline, nullptr);
    line_pipelines_.clear();
    for (auto& b : line_ring_) b.destroy();
    if (lines_vs_) {
        vkDestroyShaderModule(ctx_->device(), lines_vs_, nullptr);
        lines_vs_ = VK_NULL_HANDLE;
    }
    for (auto& [key, pipeline] : modvol_pipelines_)
        vkDestroyPipeline(ctx_->device(), pipeline, nullptr);
    modvol_pipelines_.clear();
    for (auto& b : modvol_ring_) b.destroy();
    if (mv_vs_) {
        vkDestroyShaderModule(ctx_->device(), mv_vs_, nullptr);
        mv_vs_ = VK_NULL_HANDLE;
    }
    if (mv_fs_) {
        vkDestroyShaderModule(ctx_->device(), mv_fs_, nullptr);
        mv_fs_ = VK_NULL_HANDLE;
    }
    textures_.destroy();
    if (set_layout_) {
        vkDestroyDescriptorSetLayout(ctx_->device(), set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    for (auto& b : vertex_ring_) b.destroy();
    for (auto& b : index_ring_) b.destroy();
    if (layout_) {
        vkDestroyPipelineLayout(ctx_->device(), layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (vs_) {
        vkDestroyShaderModule(ctx_->device(), vs_, nullptr);
        vs_ = VK_NULL_HANDLE;
    }
    if (fs_) {
        vkDestroyShaderModule(ctx_->device(), fs_, nullptr);
        fs_ = VK_NULL_HANDLE;
    }
    if (fs_nd_) {
        vkDestroyShaderModule(ctx_->device(), fs_nd_, nullptr);
        fs_nd_ = VK_NULL_HANDLE;
    }
    ctx_ = nullptr;
}

}  // namespace dream::render::vk
