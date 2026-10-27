#include "dream/render/interpolate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace dream::render {

namespace {

// A line batch (Polygon::line_batch) pairs with nothing: it is drawn from the newer frame as it is.
bool same(const Polygon& a, const Polygon& b) {
    return !a.line_batch && !b.line_batch && a.count == b.count && a.pcw == b.pcw && a.isp == b.isp &&
           a.tsp == b.tsp && a.tcw == b.tcw && a.tsp1 == b.tsp1 && a.tcw1 == b.tcw1;
}

// A textured strip's identity beyond its parameter words: its vertex count and the texture
// coordinates of its first two vertices, which world geometry keeps from frame to frame.
std::uint64_t uv_key(const Frame& f, const Polygon& p) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](std::uint32_t w) { h = (h ^ w) * 0x100000001b3ull; };
    mix(p.pcw); mix(p.isp); mix(p.tsp); mix(p.tcw); mix(p.count);
    for (std::uint32_t k = 0; k < std::min<std::uint32_t>(2, p.count); ++k) {
        const Vertex& v = f.vertices[p.first + k];
        mix(static_cast<std::uint32_t>(std::lround(v.u * 1024.0f)));
        mix(static_cast<std::uint32_t>(std::lround(v.v * 1024.0f)));
    }
    return h;
}

}  // namespace

MatchStats match_frames(const Frame& older, const Frame& newer, unsigned window,
                        std::vector<StripPair>& out) {
    out.clear();
    MatchStats st;
    for (std::uint32_t l = 0; l < newer.lists.size(); ++l) {
        const auto& A = older.lists[l];
        const auto& B = newer.lists[l];
        st.strips_a += static_cast<std::uint32_t>(A.size());
        st.strips_b += static_cast<std::uint32_t>(B.size());
        for (const auto& p : B) st.vertices_b += p.count;
        std::size_t i = 0, j = 0;
        while (i < A.size() && j < B.size()) {
            if (same(A[i], B[j])) {
                out.push_back({l, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j)});
                ++st.matched;
                st.matched_vertices += B[j].count;
                ++i;
                ++j;
                continue;
            }
            // The nearest resynchronisation: older has extra strips, or newer has new ones.
            std::size_t skip_a = 0, skip_b = 0;
            for (unsigned d = 1; d <= window; ++d) {
                if (i + d < A.size() && same(A[i + d], B[j])) {
                    skip_a = d;
                    break;
                }
                if (j + d < B.size() && same(A[i], B[j + d])) {
                    skip_b = d;
                    break;
                }
            }
            if (skip_a)
                i += skip_a;
            else if (skip_b)
                j += skip_b;
            else {
                ++i;
                ++j;
            }
        }
    }
    return st;
}

bool blend_frames(const Frame& older, const Frame& newer, const BlendOptions& opt,
                  std::vector<StripPair>& pairs, Frame& out, BlendStats& stats) {
    out = newer;
    stats = BlendStats{};
    stats.match = match_frames(older, newer, opt.window, pairs);
    // Camera cut test: the median movement of paired, on-screen first vertices.
    std::vector<float> motion;
    motion.reserve(pairs.size());
    for (const auto& p : pairs) {
        const Vertex& va = older.vertices[older.lists[p.list][p.a].first];
        const Vertex& vb = newer.vertices[newer.lists[p.list][p.b].first];
        if (vb.x >= 0.0f && vb.x <= 640.0f && vb.y >= 0.0f && vb.y <= 480.0f)
            motion.push_back(std::hypot(vb.x - va.x, vb.y - va.y));
    }
    if (!motion.empty()) {
        std::nth_element(motion.begin(), motion.begin() + motion.size() / 2, motion.end());
        stats.median_motion = motion[motion.size() / 2];
    }
    if (stats.median_motion > opt.cut_median) {
        stats.cut = true;
        return false;
    }
    const float t = opt.t;
    // Texture coordinates identify a strip only when no other strip of its list carries the same
    // ones. World geometry's are unique; repeated sprites (dozens of identical bubbles) share
    // theirs, and trusting them would pair a sprite with another one across the screen, so every
    // in-between frame would show the swarm rearranged. Only
    // unique strips get the long jump and the re-pairing below.
    std::vector<std::unordered_map<std::uint64_t, std::uint32_t>> key_count(out.lists.size());
    if (opt.match_uv)
        for (std::size_t l = 0; l < out.lists.size(); ++l) {
            for (const auto& p : newer.lists[l])
                if ((p.pcw >> 3) & 1u) ++key_count[l][uv_key(newer, p)];
            if (l < older.lists.size())
                for (const auto& p : older.lists[l])
                    if ((p.pcw >> 3) & 1u) key_count[l][uv_key(older, p)] += 0x10000u;
        }
    // Exactly once in each frame.
    auto unique_uv = [&](std::size_t l, const Polygon& p) {
        const auto it = key_count[l].find(uv_key(newer, p));
        return it != key_count[l].end() && it->second == 0x10001u;
    };
    auto duplicate_uv = [&](std::size_t l, const Polygon& p) {
        const auto it = key_count[l].find(uv_key(newer, p));
        return it != key_count[l].end() && ((it->second & 0xFFFFu) > 1 || (it->second >> 16) > 1);
    };
    std::vector<std::vector<std::uint8_t>> blended(out.lists.size());
    for (std::size_t l = 0; l < out.lists.size(); ++l) blended[l].assign(out.lists[l].size(), 0);
    for (const auto& p : pairs) {
        const Polygon& pa = older.lists[p.list][p.a];
        const Polygon& pb = out.lists[p.list][p.b];
        bool ok = true;
        const bool uv_checked = opt.match_uv && ((pb.pcw >> 3) & 1u);
        if (uv_checked) {
            for (std::uint32_t k = 0; k < pb.count && ok; ++k) {
                const Vertex& va = older.vertices[pa.first + k];
                const Vertex& vb = out.vertices[pb.first + k];
                ok = std::abs(vb.u - va.u) <= opt.uv_tolerance &&
                     std::abs(vb.v - va.v) <= opt.uv_tolerance;
            }
            if (!ok) {
                ++stats.uv_mismatched_strips;
                continue;
            }
        }
        // A sprite with twins is paired by position below, not by list order.
        if (uv_checked && duplicate_uv(p.list, pb))
            continue;
        const float jump = uv_checked && unique_uv(p.list, pb) ? opt.max_jump_uv : opt.max_jump;
        for (std::uint32_t k = 0; k < pb.count && ok; ++k) {
            const Vertex& va = older.vertices[pa.first + k];
            const Vertex& vb = out.vertices[pb.first + k];
            ok = std::isfinite(va.x) && std::isfinite(va.y) && std::isfinite(va.z) &&
                 std::isfinite(vb.x) && std::isfinite(vb.y) && std::isfinite(vb.z) &&
                 std::abs(vb.x - va.x) <= jump && std::abs(vb.y - va.y) <= jump;
        }
        if (!ok) {
            ++stats.jumped_strips;
            continue;
        }
        for (std::uint32_t k = 0; k < pb.count; ++k) {
            const Vertex& va = older.vertices[pa.first + k];
            Vertex& vb = out.vertices[pb.first + k];
            vb.x = va.x + (vb.x - va.x) * t;
            vb.y = va.y + (vb.y - va.y) * t;
            vb.z = va.z + (vb.z - va.z) * t;
        }
        blended[p.list][p.b] = 1;
        ++stats.blended_strips;
    }
    // Twins: strips that share their texture coordinates with others in the list,
    // such as a swarm of identical bubbles. List order says nothing about which
    // is which (the game adds, drops and reorders them), so each pairs with the twin nearest to it
    // in the older frame, and only when that one's nearest is it in return, within max_jump.
    // A twin left unpaired is drawn where the newer frame has it.
    if (opt.match_uv) {
        auto centre = [](const Frame& f, const Polygon& p) {
            float x = 0.0f, y = 0.0f;
            for (std::uint32_t k = 0; k < p.count; ++k) {
                x += f.vertices[p.first + k].x;
                y += f.vertices[p.first + k].y;
            }
            return std::pair<float, float>{x / static_cast<float>(p.count), y / static_cast<float>(p.count)};
        };
        for (std::size_t l = 0; l < out.lists.size() && l < older.lists.size(); ++l) {
            const auto& A = older.lists[l];
            const auto& B = out.lists[l];
            std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> twins_a, twins_b;
            for (std::uint32_t j = 0; j < B.size(); ++j)
                if (((B[j].pcw >> 3) & 1u) && duplicate_uv(l, B[j]))
                    twins_b[uv_key(newer, B[j])].push_back(j);
            if (twins_b.empty())
                continue;
            for (std::uint32_t i = 0; i < A.size(); ++i)
                if ((A[i].pcw >> 3) & 1u) {
                    const auto key = uv_key(older, A[i]);
                    if (twins_b.count(key))
                        twins_a[key].push_back(i);
                }
            for (const auto& [key, bs] : twins_b) {
                const auto found = twins_a.find(key);
                if (found == twins_a.end())
                    continue;
                const auto& as = found->second;
                std::vector<std::pair<float, float>> ca, cb;
                for (auto i : as) ca.push_back(centre(older, A[i]));
                for (auto j : bs) cb.push_back(centre(newer, B[j]));
                auto d2 = [&](std::size_t x, std::size_t y) {
                    const float dx = ca[x].first - cb[y].first, dy = ca[x].second - cb[y].second;
                    return dx * dx + dy * dy;
                };
                auto nearest_a = [&](std::size_t y) {
                    std::size_t best = 0;
                    for (std::size_t x = 1; x < ca.size(); ++x)
                        if (d2(x, y) < d2(best, y)) best = x;
                    return best;
                };
                auto nearest_b = [&](std::size_t x) {
                    std::size_t best = 0;
                    for (std::size_t y = 1; y < cb.size(); ++y)
                        if (d2(x, y) < d2(x, best)) best = y;
                    return best;
                };
                for (std::size_t y = 0; y < cb.size(); ++y) {
                    const std::size_t x = nearest_a(y);
                    if (nearest_b(x) != y || d2(x, y) > opt.max_jump * opt.max_jump)
                        continue;
                    const Polygon& pa = A[as[x]];
                    const Polygon& pb = B[bs[y]];
                    if (!same(pa, pb))
                        continue;
                    bool ok = true;
                    for (std::uint32_t k = 0; k < pb.count && ok; ++k) {
                        const Vertex& va = older.vertices[pa.first + k];
                        const Vertex& vb = out.vertices[pb.first + k];
                        ok = std::isfinite(va.x) && std::isfinite(va.y) && std::isfinite(va.z) &&
                             std::isfinite(vb.x) && std::isfinite(vb.y) && std::isfinite(vb.z);
                    }
                    if (!ok)
                        continue;
                    for (std::uint32_t k = 0; k < pb.count; ++k) {
                        const Vertex& va = older.vertices[pa.first + k];
                        Vertex& vb = out.vertices[pb.first + k];
                        vb.x = va.x + (vb.x - va.x) * t;
                        vb.y = va.y + (vb.y - va.y) * t;
                        vb.z = va.z + (vb.z - va.z) * t;
                    }
                    blended[l][bs[y]] = 1;
                    ++stats.blended_strips;
                }
            }
        }
    }
    // Second chance for textured strips the in-order pairing missed or got wrong: games that cull
    // the world per strip reorder it as the camera turns. Look the strip up in the older frame by
    // its texture coordinates instead of its position in the list.
    if (opt.match_uv) {
        for (std::size_t l = 0; l < out.lists.size() && l < older.lists.size(); ++l) {
            std::unordered_multimap<std::uint64_t, std::uint32_t> by_key;
            const auto& A = older.lists[l];
            for (std::uint32_t i = 0; i < A.size(); ++i)
                if ((A[i].pcw >> 3) & 1u)
                    by_key.emplace(uv_key(older, A[i]), i);
            if (by_key.empty())
                continue;
            for (std::uint32_t j = 0; j < out.lists[l].size(); ++j) {
                if (blended[l][j])
                    continue;
                const Polygon& pb = newer.lists[l][j];
                if (!((pb.pcw >> 3) & 1u) || !unique_uv(l, pb))
                    continue;
                auto [lo, hi] = by_key.equal_range(uv_key(newer, pb));
                for (auto it = lo; it != hi; ++it) {
                    const Polygon& pa = A[it->second];
                    if (!same(pa, pb))
                        continue;
                    bool ok = true;
                    for (std::uint32_t k = 0; k < pb.count && ok; ++k) {
                        const Vertex& va = older.vertices[pa.first + k];
                        const Vertex& vb = newer.vertices[pb.first + k];
                        ok = std::abs(vb.u - va.u) <= opt.uv_tolerance &&
                             std::abs(vb.v - va.v) <= opt.uv_tolerance && std::isfinite(va.x) &&
                             std::isfinite(va.y) && std::isfinite(va.z) && std::isfinite(vb.x) &&
                             std::isfinite(vb.y) && std::isfinite(vb.z) &&
                             std::abs(vb.x - va.x) <= opt.max_jump_uv &&
                             std::abs(vb.y - va.y) <= opt.max_jump_uv;
                    }
                    if (!ok)
                        continue;
                    for (std::uint32_t k = 0; k < pb.count; ++k) {
                        const Vertex& va = older.vertices[pa.first + k];
                        Vertex& vb = out.vertices[out.lists[l][j].first + k];
                        vb.x = va.x + (vb.x - va.x) * t;
                        vb.y = va.y + (vb.y - va.y) * t;
                        vb.z = va.z + (vb.z - va.z) * t;
                    }
                    blended[l][j] = 1;
                    ++stats.blended_strips;
                    ++stats.repaired_strips;
                    by_key.erase(it);
                    break;
                }
            }
        }
    }
    // Tear test: on-screen vertices of strips left as the newer frame has them. Only meaningful
    // when the scene moves; a still scene blends to itself either way.
    if (stats.median_motion > 0.5f) {
        // Measured by screen area, not vertex count: the strips that cannot pair are mostly ones
        // the game re-clips every frame at the near plane (new vertices, new texture coordinates),
        // which are few vertices but the largest pieces on screen (walls in a turn).
        double on = 0.0, left = 0.0;
        auto cx = [](float x) { return std::clamp(x, 0.0f, 640.0f); };
        auto cy = [](float y) { return std::clamp(y, 0.0f, 480.0f); };
        for (std::size_t l = 0; l < out.lists.size(); ++l)
            for (std::size_t i = 0; i < out.lists[l].size(); ++i) {
                const Polygon& pb = newer.lists[l][i];
                double area = 0.0;
                for (std::uint32_t k = 0; k + 2 < pb.count; ++k) {
                    const Vertex& a = newer.vertices[pb.first + k];
                    const Vertex& b = newer.vertices[pb.first + k + 1];
                    const Vertex& c = newer.vertices[pb.first + k + 2];
                    if (!std::isfinite(a.x) || !std::isfinite(b.x) || !std::isfinite(c.x))
                        continue;
                    const double ax = cx(a.x), ay = cy(a.y), bx = cx(b.x), by = cy(b.y),
                                 qx = cx(c.x), qy = cy(c.y);
                    area += std::abs((bx - ax) * (qy - ay) - (by - ay) * (qx - ax)) * 0.5;
                }
                on += area;
                if (!blended[l][i])
                    left += area;
            }
        stats.unblended_share = on > 0.0 ? static_cast<float>(left / on) : 0.0f;
        if (stats.unblended_share > opt.max_unblended) {
            stats.torn = true;
            out = newer;
            return false;
        }
    }
    return true;
}

}  // namespace dream::render
