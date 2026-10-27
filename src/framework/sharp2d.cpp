// Point sampling for 1:1 screen-aligned quads (include/dreamcomp/sharp2d.h).
#include "dreamcomp/sharp2d.h"

#include <cmath>
#include <cstdint>

#include "dream/render/display_list.h"

namespace dreamcomp {
namespace {

constexpr float kEps = 1.0f / 64.0f;

bool whole(float a) { return std::fabs(a - std::round(a)) < kEps; }
bool same(float a, float b) { return std::fabs(a - b) < kEps; }

// The quad's two x and two y values when its four vertices sit on an axis-aligned rectangle.
bool rectangle(const dream::render::Vertex* v, float& x0, float& x1, float& y0, float& y1) {
    x0 = x1 = v[0].x;
    y0 = y1 = v[0].y;
    for (int i = 1; i < 4; ++i) {
        x0 = std::fmin(x0, v[i].x);
        x1 = std::fmax(x1, v[i].x);
        y0 = std::fmin(y0, v[i].y);
        y1 = std::fmax(y1, v[i].y);
    }
    for (int i = 0; i < 4; ++i)
        if (!(same(v[i].x, x0) || same(v[i].x, x1)) || !(same(v[i].y, y0) || same(v[i].y, y1)))
            return false;
    return x1 - x0 >= 1.0f && y1 - y0 >= 1.0f;
}

bool one_to_one(const dream::render::Polygon& p, const dream::render::Vertex* v) {
    const float z = v[0].z;
    for (int i = 1; i < 4; ++i)
        if (v[i].z != z)
            return false;  // perspective: texels are not pixels
    float x0, x1, y0, y1;
    if (!rectangle(v, x0, x1, y0, y1) || !whole(x0) || !whole(x1) || !whole(y0) || !whole(y1))
        return false;
    const float w = static_cast<float>(8u << ((p.tsp >> 3) & 7u));
    const float h = static_cast<float>(8u << (p.tsp & 7u));
    // u must follow x alone and v follow y alone, one texel per pixel, starting on a texel edge.
    float u_at_x0 = 0, u_at_x1 = 0, v_at_y0 = 0, v_at_y1 = 0;
    bool have_u0 = false, have_u1 = false, have_v0 = false, have_v1 = false;
    for (int i = 0; i < 4; ++i) {
        float& u_slot = same(v[i].x, x0) ? u_at_x0 : u_at_x1;
        bool& u_have = same(v[i].x, x0) ? have_u0 : have_u1;
        if (u_have && !same(u_slot * w, v[i].u * w))
            return false;
        u_slot = v[i].u;
        u_have = true;
        float& v_slot = same(v[i].y, y0) ? v_at_y0 : v_at_y1;
        bool& v_have = same(v[i].y, y0) ? have_v0 : have_v1;
        if (v_have && !same(v_slot * h, v[i].v * h))
            return false;
        v_slot = v[i].v;
        v_have = true;
    }
    return same(std::fabs(u_at_x1 - u_at_x0) * w, x1 - x0) &&
           same(std::fabs(v_at_y1 - v_at_y0) * h, y1 - y0) && whole(u_at_x0 * w) && whole(v_at_y0 * h);
}

}  // namespace

std::size_t sharpen_2d(dream::render::Frame& frame) {
    std::size_t n = 0;
    for (auto& list : frame.lists) {
        for (auto& p : list) {
            const bool textured = (p.pcw & 0x8u) != 0;
            const bool bilinear = ((p.tsp >> 13) & 3u) == 1u;
            const bool stride = (p.tcw & (1u << 25)) != 0;   // width from a register: skip
            const bool mipmapped = (p.tcw & (1u << 31)) != 0;
            const bool two_volume = p.tsp1 != 0xFFFFFFFFu;
            if (!textured || !bilinear || stride || mipmapped || two_volume || p.count != 4 ||
                p.first + 4 > frame.vertices.size())
                continue;
            if (!one_to_one(p, &frame.vertices[p.first]))
                continue;
            p.tsp &= ~(3u << 13);  // point sampling
            ++n;
        }
    }
    return n;
}

}  // namespace dreamcomp
