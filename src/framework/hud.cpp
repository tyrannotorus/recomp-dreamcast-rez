// Widescreen HUD correction with per-element overrides: see include/dreamcomp/hud.h.
#include "dreamcomp/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "dream/render/display_list.h"

namespace dreamcomp::hud {

namespace {
// A word saved before untextured pieces were told apart: a TCW whose pixel format field is 7
// (reserved) is never a texture -- it is float data such as 0x3F800000 (1.0) -- so it is read as 0.
std::uint32_t clean_word(std::uint32_t w) { return ((w >> 27) & 7u) == 7u ? 0u : w; }
}  // namespace

const char* name(Anchor a) noexcept {
    switch (a) {
        case Anchor::Left: return "left";
        case Anchor::Center: return "center";
        case Anchor::Right: return "right";
        case Anchor::Stretch: return "stretch";
        default: return "auto";
    }
}

bool parse(const std::string& s, Anchor& out) noexcept {
    for (Anchor a : {Anchor::Auto, Anchor::Left, Anchor::Center, Anchor::Right, Anchor::Stretch})
        if (s == name(a)) {
            out = a;
            return true;
        }
    return false;
}

bool Override::covers(float cx, float cy, std::uint32_t tcw) const noexcept {
    if (cx < x0 || cx > x1 || cy < y0 || cy > y1)
        return false;
    return tcws.empty() || std::find(tcws.begin(), tcws.end(), tcw) != tcws.end();
}

int Overrides::match(float cx, float cy, std::uint32_t tcw, bool wide) const noexcept {
    for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i) {
        const Override& o = items[static_cast<std::size_t>(i)];
        if ((!wide || o.full) && o.covers(cx, cy, tcw))
            return i;
    }
    return -1;
}

bool Overrides::load(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file);
    if (!in)
        return true;  // no file: nothing to add
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (const auto hash = line.find('#'); hash != std::string::npos)
            line.erase(hash);
        std::istringstream words(line);
        std::string w;
        Override o;
        bool have_rect = false, have_anchor = false, bad = false;
        while (words >> w) {
            const auto eq = w.find('=');
            if (eq == std::string::npos) {
                bad = true;
                break;
            }
            const std::string key = w.substr(0, eq), value = w.substr(eq + 1);
            if (key == "rect") {
                have_rect = std::sscanf(value.c_str(), "%f,%f,%f,%f", &o.x0, &o.y0, &o.x1, &o.y1) == 4;
                bad |= !have_rect;
            } else if (key == "anchor") {
                have_anchor = parse(value, o.anchor);
                bad |= !have_anchor;
            } else if (key == "tcw") {
                std::istringstream list(value);
                std::string t;
                while (std::getline(list, t, ','))
                    if (!t.empty()) {
                        const std::uint32_t word =
                            clean_word(static_cast<std::uint32_t>(std::strtoul(t.c_str(), nullptr, 0)));
                        if (std::find(o.tcws.begin(), o.tcws.end(), word) == o.tcws.end())
                            o.tcws.push_back(word);
                    }
            } else if (key == "full") {
                o.full = value == "1" || value == "true";
            } else {
                bad = true;
            }
        }
        if (!have_rect && !have_anchor && !bad)
            continue;  // blank or comment
        if (bad || !have_rect || !have_anchor) {
            if (error)
                *error = file.string() + ":" + std::to_string(n) +
                         ": expected rect=x0,y0,x1,y1 anchor=... [tcw=...] [full=1]";
            return false;
        }
        items.push_back(std::move(o));
    }
    return true;
}

bool Overrides::save(const std::filesystem::path& file, std::string* error) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            if (error)
                *error = "cannot write " + tmp;
            return false;
        }
        out << "# Rez Recompiled HUD overrides: rect in the console's 640x480 coordinates\n"
               "# before correction, anchor left|center|right|stretch|auto, tcw = texture words of\n"
               "# the element's pieces (none: any). Later lines win. Edited with F1 in game.\n";
        char buf[64];
        for (const Override& o : items) {
            std::snprintf(buf, sizeof buf, "rect=%.0f,%.0f,%.0f,%.0f", o.x0, o.y0, o.x1, o.y1);
            out << buf << " anchor=" << name(o.anchor);
            if (!o.tcws.empty()) {
                out << " tcw=";
                for (std::size_t i = 0; i < o.tcws.size(); ++i) {
                    std::snprintf(buf, sizeof buf, "%s0x%08x", i ? "," : "", static_cast<unsigned>(o.tcws[i]));
                    out << buf;
                }
            }
            if (o.full)
                out << " full=1";
            out << "\n";
        }
        if (!out) {
            if (error)
                *error = "cannot write " + tmp;
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        if (error)
            *error = "cannot replace " + file.string() + ": " + ec.message();
        return false;
    }
    return true;
}

namespace {

// The texture word an override compares: the TCW of a textured polygon, 0 for an untextured one
// (whose TCW slot is leftover data).
std::uint32_t texture_word(const dream::render::Polygon& p) { return (p.pcw & 0x8u) ? p.tcw : 0u; }

struct Item {
    const dream::render::Polygon* poly;
    float x0, x1, y0, y1;
    Anchor forced;  // from an override (Auto: none, or an override that only adds)
    int override_index;
};

float anchor_x(Anchor a, float cx, bool edges) {
    if (!edges)
        return 320.0f;
    switch (a) {
        case Anchor::Left: return 0.0f;
        case Anchor::Right: return 640.0f;
        case Anchor::Center: return 320.0f;
        default:
            // About the screen edge of its third, not its own edge: an element keeps its 4:3
            // distance from the left or right edge (or from the centre), so a row such as
            // "STAGE 1  0'09\"07" stays together at any width instead of drifting apart.
            return cx < 640.0f / 3.0f ? 0.0f : cx > 1280.0f / 3.0f ? 640.0f : 320.0f;
    }
}

Anchor resolved(float ax) {
    return ax == 0.0f ? Anchor::Left : ax == 640.0f ? Anchor::Right : Anchor::Center;
}

void add_tcw(std::vector<std::uint32_t>& v, std::uint32_t t) {
    if (std::find(v.begin(), v.end(), t) == v.end())
        v.push_back(t);
}

}  // namespace

void correct(dream::render::Frame& frame, const PortInfo::HudRule& rule, float aspect, bool edges,
             bool apply, const Overrides& overrides, Snapshot* snapshot, Stats& stats) {
    const float k = apply ? (4.0f / 3.0f) / aspect : 1.0f;  // < 1: squeeze back
    auto type_of = [](const dream::render::Polygon& p) { return p.pcw >> 29; };  // 5 sprite, 4 polygon
    // A primitive drawn at one depth (every vertex the same 1/w): 2D.
    auto flat = [&](const dream::render::Polygon& p, float& z) {
        const std::uint32_t type = type_of(p);
        if (p.count == 0 || (type != 5u && type != 4u))
            return false;
        z = frame.vertices[p.first].z;
        for (std::uint32_t i = 1; i < p.count; ++i)
            if (frame.vertices[p.first + i].z != z)
                return false;
        return true;
    };
    auto eligible = [&](const dream::render::Polygon& p) {
        const std::uint32_t type = type_of(p);
        return type == 5u || (rule.polygons && type == 4u);
    };
    auto rect = [&](const dream::render::Polygon& p, float& x0, float& x1, float& y0, float& y1) {
        x0 = y0 = 1e30f;
        x1 = y1 = -1e30f;
        for (std::uint32_t i = 0; i < p.count; ++i) {
            const auto& v = frame.vertices[p.first + i];
            x0 = std::min(x0, v.x);
            x1 = std::max(x1, v.x);
            y0 = std::min(y0, v.y);
            y1 = std::max(y1, v.y);
        }
    };

    // Pass 1: how many eligible flat pieces share each depth, and how much of the screen they
    // cover together (a depth covering most of it is a backdrop, not HUD).
    std::unordered_map<float, unsigned> counts;
    std::unordered_map<float, float> cover;
    for (const auto& list : frame.lists)
        for (const auto& p : list) {
            float z;
            if (!flat(p, z) || !eligible(p))
                continue;
            ++counts[z];
            float x0, x1, y0, y1;
            rect(p, x0, x1, y0, y1);
            cover[z] += (x1 - x0) * (y1 - y0);
        }
    const float backdrop_area = rule.backdrop_cover * 640.0f * 480.0f;
    const bool any_overrides = !overrides.items.empty();

    // Pass 2: the HUD pieces -- the rule's, plus what an override adds, minus what it stretches.
    std::vector<Item> items;
    std::unordered_map<int, Box> stretched;  // per override, for the snapshot
    for (auto& list : frame.lists) {
        for (const auto& p : list) {
            float z;
            if (!flat(p, z))
                continue;
            float x0, x1, y0, y1;
            rect(p, x0, x1, y0, y1);
            const int m = any_overrides ? overrides.match(0.5f * (x0 + x1), 0.5f * (y0 + y1), texture_word(p),
                                                          x1 - x0 > rule.full_width)
                                        : -1;
            if (m >= 0) {
                const Anchor a = overrides.items[static_cast<std::size_t>(m)].anchor;
                if (a == Anchor::Stretch) {
                    if (snapshot) {
                        auto [it, fresh] = stretched.try_emplace(m);
                        Box& b = it->second;
                        if (fresh) {
                            b.x0 = b.ox0 = x0;
                            b.x1 = b.ox1 = x1;
                            b.y0 = b.oy0 = y0;
                            b.y1 = b.oy1 = y1;
                        }
                        b.x0 = b.ox0 = std::min(b.ox0, x0);
                        b.x1 = b.ox1 = std::max(b.ox1, x1);
                        b.y0 = b.oy0 = std::min(b.oy0, y0);
                        b.y1 = b.oy1 = std::max(b.oy1, y1);
                        b.anchor = Anchor::Stretch;
                        b.hud = true;
                        b.override_index = m;
                        add_tcw(b.tcws, texture_word(p));
                    }
                    continue;
                }
                items.push_back({&p, x0, x1, y0, y1, a, m});
                continue;
            }
            bool hud = eligible(p);
            if (hud && z < rule.overlay_z && counts[z] < rule.min_shared)
                hud = false;
            if (hud && cover[z] > backdrop_area)
                hud = false;
            if (hud && x1 - x0 > rule.full_width)
                hud = false;
            if (hud) {
                items.push_back({&p, x0, x1, y0, y1, Anchor::Auto, -1});
            } else if (snapshot && x1 - x0 >= 2.0f && y1 - y0 >= 2.0f && x1 - x0 <= rule.full_width &&
                       (x1 - x0) * (y1 - y0) < backdrop_area && !(eligible(p) && cover[z] > backdrop_area)) {
                // Offered for adding: flat pieces of a sensible size (not screen-wide strips or
                // backdrops, which only clutter the editor).
                Box b;
                b.x0 = b.ox0 = x0;
                b.x1 = b.ox1 = x1;
                b.y0 = b.oy0 = y0;
                b.y1 = b.oy1 = y1;
                b.anchor = Anchor::Stretch;
                b.hud = false;
                b.tcws.push_back(texture_word(p));
                snapshot->boxes.push_back(std::move(b));
            }
        }
    }

    // Layout `center`: the whole HUD scaled about the screen centre (its original 4:3 layout).
    // Layout `edges` (default): pieces that touch horizontally and overlap vertically form one
    // element (a health bar is a cap, a bar and a cap); each element is un-stretched about the
    // edge of the screen third its centre falls in, or about the edge its override names.
    const std::size_t n = items.size();
    std::vector<std::size_t> group(n);
    for (std::size_t i = 0; i < n; ++i) group[i] = i;
    auto root = [&](std::size_t i) {
        while (group[i] != i) i = group[i] = group[group[i]];
        return i;
    };
    if (edges) {
        // Touching pieces (gap <= 3 px, overlapping rows) are one element; so are pieces of one
        // text line -- same top and bottom within 4 px -- across word gaps up to 1.5x their
        // height, so "INSERT COIN" moves as one. A bar beside taller timer digits has a different
        // extent and stays separate.
        constexpr float kTouch = 3.0f, kSameRow = 4.0f;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = i + 1; j < n; ++j) {
                const Item& a = items[i];
                const Item& b = items[j];
                const float gap = std::max(a.x0, b.x0) - std::min(a.x1, b.x1);
                const bool rows_overlap = a.y0 <= b.y1 && b.y0 <= a.y1;
                const bool same_line = std::abs(a.y0 - b.y0) <= kSameRow && std::abs(a.y1 - b.y1) <= kSameRow;
                const float line_gap = 1.5f * std::max(a.y1 - a.y0, b.y1 - b.y0);
                // A piece with an override of its own (F2: one piece anchored on its own) is
                // never grouped with pieces that have another one, or none.
                if (a.override_index != b.override_index)
                    continue;
                if ((rows_overlap && gap <= kTouch) || (same_line && gap <= line_gap))
                    group[root(i)] = root(j);
            }
    }
    std::vector<float> gx0(n, 1e30f), gx1(n, -1e30f), gy0(n, 1e30f), gy1(n, -1e30f);
    std::vector<Anchor> gforced(n, Anchor::Auto);
    std::vector<int> goverride(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t r = root(i);
        gx0[r] = std::min(gx0[r], items[i].x0);
        gx1[r] = std::max(gx1[r], items[i].x1);
        gy0[r] = std::min(gy0[r], items[i].y0);
        gy1[r] = std::max(gy1[r], items[i].y1);
        if (items[i].override_index >= 0 && goverride[r] < 0) {
            goverride[r] = items[i].override_index;
            gforced[r] = items[i].forced;
        }
    }
    std::vector<int> box_of(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t r = root(i);
        const float anchor = anchor_x(gforced[r], 0.5f * (gx0[r] + gx1[r]), edges);
        const auto& p = *items[i].poly;
        if (apply) {
            for (std::uint32_t v = 0; v < p.count; ++v) {
                auto& vx = frame.vertices[p.first + v];
                vx.x = anchor + (vx.x - anchor) * k;
            }
            ++stats.corrected;
        }
        if (!snapshot)
            continue;
        if (box_of[r] < 0) {
            box_of[r] = static_cast<int>(snapshot->boxes.size());
            Box b;
            b.ox0 = gx0[r];
            b.ox1 = gx1[r];
            b.oy0 = gy0[r];
            b.oy1 = gy1[r];
            b.x0 = anchor + (gx0[r] - anchor) * k;
            b.x1 = anchor + (gx1[r] - anchor) * k;
            b.y0 = gy0[r];
            b.y1 = gy1[r];
            b.anchor = resolved(anchor);
            b.hud = true;
            b.override_index = goverride[r];
            snapshot->boxes.push_back(std::move(b));
        }
        add_tcw(snapshot->boxes[static_cast<std::size_t>(box_of[r])].tcws, texture_word(p));
        // The piece on its own, for the F2 list.
        Box pc;
        pc.ox0 = items[i].x0;
        pc.ox1 = items[i].x1;
        pc.oy0 = items[i].y0;
        pc.oy1 = items[i].y1;
        pc.x0 = anchor + (items[i].x0 - anchor) * k;
        pc.x1 = anchor + (items[i].x1 - anchor) * k;
        pc.y0 = items[i].y0;
        pc.y1 = items[i].y1;
        pc.anchor = resolved(anchor);
        pc.hud = true;
        pc.override_index = items[i].override_index;
        pc.tcws.push_back(texture_word(p));
        snapshot->pieces.push_back(std::move(pc));
    }
    if (snapshot) {
        for (auto& [m, b] : stretched) {
            (void)m;
            snapshot->boxes.push_back(std::move(b));
        }
        snapshot->image_aspect = aspect;
        snapshot->edges = edges;
    }
}

}  // namespace dreamcomp::hud
