// See display_list.h.
#include <vector>
#include <cmath>
#include "dream/render/display_list.h"

#include <algorithm>
#include <cfenv>
#include <cstring>

namespace dream::render {

namespace {

using u32 = std::uint32_t;

// Parameter control word fields (Flycast's PCW union, ta_structs.h).
constexpr u32 pcw_para_type(u32 p) {
    return (p >> 29) & 7u;
}
constexpr u32 pcw_end_of_strip(u32 p) {
    return (p >> 28) & 1u;
}
constexpr u32 pcw_list_type(u32 p) {
    return (p >> 24) & 7u;
}
constexpr u32 pcw_user_clip(u32 p) {
    return (p >> 16) & 3u;
}
constexpr u32 pcw_volume(u32 p) {
    return (p >> 6) & 1u;
}
constexpr u32 pcw_col_type(u32 p) {
    return (p >> 4) & 3u;
}
constexpr u32 pcw_texture(u32 p) {
    return (p >> 3) & 1u;
}
constexpr u32 pcw_offset(u32 p) {
    return (p >> 2) & 1u;
}
constexpr u32 pcw_uv_16bit(u32 p) {
    return (p >> 0) & 1u;
}

enum ParaType : u32 {
    kEndOfList = 0,
    kUserTileClip = 1,
    kObjectListSet = 2,
    kPolygonOrModVol = 4,
    kSprite = 5,
    kExternal = 6,  // reserved by the PowerVR; the line batches (LineBatch)
    kVertex = 7
};

enum ColType : u32 { kPacked = 0, kFloating = 1, kIntensity = 2, kIntensityPrev = 3 };

float as_float(u32 w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

u32 clamp_byte(float v) {
    const int i = static_cast<int>(v * 255.0f + 0.5f);
    return static_cast<u32>(std::clamp(i, 0, 255));
}

// Four floats in A, R, G, B order to ARGB8888.
u32 float_colour(const u32* w) {
    return (clamp_byte(as_float(w[0])) << 24) | (clamp_byte(as_float(w[1])) << 16) |
           (clamp_byte(as_float(w[2])) << 8) | clamp_byte(as_float(w[3]));
}

// A byte's c / 255, tabulated once for each rounding mode the guest's code runs in, so colours
// decode without a divide. A frame is decoded on one thread, in one mode; reset() takes that
// mode's table.
struct ByteQuotients {
    float nearest[256], toward_zero[256];
    ByteQuotients() {
        const int mode = std::fegetround();
        std::fesetround(FE_TONEAREST);
        for (unsigned c = 0; c < 256; ++c) nearest[c] = static_cast<float>(c) / 255.0f;
        std::fesetround(FE_TOWARDZERO);
        for (unsigned c = 0; c < 256; ++c) toward_zero[c] = static_cast<float>(c) / 255.0f;
        std::fesetround(mode);
    }
};
const float* byte_quotients() {
    static const ByteQuotients q;
    const int mode = std::fegetround();
    return mode == FE_TONEAREST ? q.nearest : mode == FE_TOWARDZERO ? q.toward_zero : nullptr;
}

// Scales a face colour's RGB by an intensity, keeping its alpha, as the intensity colour modes do.
// `quotients`: byte_quotients(), or null to divide.
u32 intensity_colour(const float* quotients, u32 face, float intensity) {
    const u32 a = face >> 24;
    const auto scale = [&](u32 shift) {
        const u32 b = (face >> shift) & 0xFFu;
        const float c = quotients ? quotients[b] : static_cast<float>(b) / 255.0f;
        return clamp_byte(c * intensity);
    };
    return (a << 24) | (scale(16) << 16) | (scale(8) << 8) | scale(0);
}

// Outside clip_far_polygons' default bounds (320 and 240, plus or minus kFarLimit), as its own
// test: NaN is never outside.
bool outside_far(const Vertex& v) {
    return v.x < 320.0f - kFarLimit || v.x > 320.0f + kFarLimit || v.y < 240.0f - kFarLimit ||
           v.y > 240.0f + kFarLimit;
}

// 16-bit texture coordinates are the top half of the float.
float uv16(u32 half) {
    return as_float(half << 16);
}

// A polygon header is 64 bytes when it carries a face colour (the intensity colour modes with a
// texture or an offset colour) or when it describes two volumes. Flycast's TaTypeLut encodes the
// same rule.
// Only intensity mode 1 makes a header grow, and only when the face colours will not fit in the
// first 32 bytes. Mode 2 reuses the previous polygon's face colour and so never carries one,
// whatever else is set: reading its header as 64 bytes swallows the first vertex of the strip and
// every triangle after it is built from the wrong corners. Sizes follow the reference,
// TaTypeLut::poly_header_type_size in Flycast's core/hw/pvr/ta.cpp (GPL-2.0).
bool header_is_64(u32 pcw) {
    if (pcw_col_type(pcw) != kIntensity)
        return false;
    if (pcw_volume(pcw))
        return true;  // type 4: a face colour for each volume
    // Type 2 when there is an offset colour to carry as well, type 1 when there is not.
    return pcw_texture(pcw) != 0 && pcw_offset(pcw) != 0;
}

// A vertex is 64 bytes when its colours do not fit in 32: floating colours, or two volumes.
bool vertex_is_64(u32 pcw) {
    const u32 col = pcw_col_type(pcw);
    if (pcw_volume(pcw))
        return pcw_texture(pcw);  // two volumes with a texture need two sets of coordinates
    if (!pcw_texture(pcw))
        return false;
    return col == kFloating;
}

}  // namespace

void Frame::clear() {
    vertices.clear();
    for (auto& l : lists) l.clear();
    modifiers.clear();
    lines.clear();
    line_edges.clear();
}

void DisplayList::reset() {
    quotients_ = byte_quotients();
    frame_.clear();
    list_ = kListCount;
    current_ = Polygon{};
    header_active_ = false;
    pending_ = Pending::None;
    face_base_ = 0xFFFFFFFFu;
    face_offset_ = 0;
    sprite_base_ = 0xFFFFFFFFu;
    sprite_offset_ = 0;
    in_sprite_ = false;
    line_words_ = 0;
    line_keep_ = false;
    line_fill_ = 0;
    // Flycast's reset rectangle: tiles 0..39 by 0..14.
    clip_x0_ = 0;
    clip_y0_ = 0;
    clip_x1_ = 40 * 32;
    clip_y1_ = 15 * 32;
}

void DisplayList::feed_stream(const std::uint32_t* words, std::size_t count) {
    for (std::size_t i = 0; i + 8 <= count; i += 8) {
        feed(words + i);
        // A line batch's ends follow its parameter in whole records, laid out as LineEdge: taken
        // at once when all of them are in this stream, otherwise record by record by
        // take_line_words.
        if (line_words_ != 0 && line_fill_ == 0) {
            const std::size_t records = (std::size_t{line_words_} + 7) / 8;
            if (i + 8 + records * 8 <= count) {
                if (line_keep_)
                    take_lines(words + i + 8, line_words_ / 6);
                line_words_ = 0;
                i += records * 8;
            }
        }
    }
}

static_assert(sizeof(LineEdge) == 6 * sizeof(float), "a line's ends are six words of the stream");
void DisplayList::take_lines(const std::uint32_t* words, std::uint32_t lines) {
    const std::size_t at = frame_.line_edges.size();
    frame_.line_edges.resize(at + lines);
    LineEdge* e = frame_.line_edges.data() + at;
    std::memcpy(static_cast<void*>(e), words, std::size_t{lines} * sizeof(LineEdge));
}

void DisplayList::feed(const std::uint32_t w[8]) {
    // A line batch's ends (handle_line_batch).
    if (line_words_ != 0) {
        take_line_words(w);
        return;
    }
    // Second half of a 64-byte parameter.
    if (pending_ != Pending::None) {
        const Pending what = pending_;
        pending_ = Pending::None;
        switch (what) {
            case Pending::PolyHeader:
                // The face colours live in the second half.
                face_base_ = float_colour(&w[0]);
                face_offset_ = float_colour(&w[4]);
                if (pcw_volume(current_.pcw)) {
                    // Two volumes: the second half holds the second volume's face colour only.
                    face_offset_ = 0;
                }
                return;
            case Pending::Vertex:
                handle_vertex(held_, true, w);
                return;
            case Pending::SpriteVertex:
                handle_sprite_vertex(held_, w);
                return;
            case Pending::ModifierVertex:
                handle_modifier_vertex(held_, w);
                return;
            case Pending::None:
                break;
        }
    }

    const u32 pcw = w[0];
    switch (pcw_para_type(pcw)) {
        case kEndOfList:
            close_list();
            return;
        case kUserTileClip:
            // Tiles of 32 pixels, inclusive (Flycast ta_vtx.cpp setClipRect).
            clip_x0_ = static_cast<std::uint16_t>((w[4] & 63u) * 32u);
            clip_y0_ = static_cast<std::uint16_t>((w[5] & 31u) * 32u);
            clip_x1_ = static_cast<std::uint16_t>(((w[6] & 63u) + 1u) * 32u);
            clip_y1_ = static_cast<std::uint16_t>(((w[7] & 31u) + 1u) * 32u);
            return;
        case kObjectListSet:
            return;  // object-list placement does not affect what is drawn here
        case kPolygonOrModVol:
        case kSprite:
            handle_header(w);
            return;
        case kVertex:
            if (!header_active_ && !in_sprite_)
                return;  // a vertex with no header: the stream is malformed
            if (list_ == static_cast<unsigned>(ListType::OpaqueModifier) ||
                list_ == static_cast<unsigned>(ListType::TranslucentModifier)) {
                // Modifier volume vertices are always 64 bytes: three positions.
                std::memcpy(held_, w, sizeof held_);
                pending_ = Pending::ModifierVertex;
                return;
            }
            if (in_sprite_) {
                std::memcpy(held_, w, sizeof held_);
                pending_ = Pending::SpriteVertex;
                return;
            }
            if (vertex_is_64(current_.pcw)) {
                std::memcpy(held_, w, sizeof held_);
                pending_ = Pending::Vertex;
                return;
            }
            handle_vertex(w, false, nullptr);
            return;
        case kExternal:
            handle_line_batch(w);
            return;
        default:
            return;
    }
}

// A line batch (LineBatch): word 0 the parameter (type 6, kind 1 in its low byte), word 1 the
// count, word 2 the half width; then each line's ends, (xa, ya, za, xb, yb, zb), in the records
// after it, the last padded to a whole record. Under the sprite header in force it is one polygon
// of the list, as its sprites would have been; with none, its words are skipped.
void DisplayList::handle_line_batch(const std::uint32_t w[8]) {
    if ((w[0] & 0xFFu) != 1u)
        return;
    const u32 count = w[1];
    line_words_ = 6u * count;
    line_fill_ = 0;
    line_keep_ = in_sprite_ && list_ < kListCount;
    if (!line_keep_)
        return;
    LineBatch b;
    b.first = static_cast<u32>(frame_.line_edges.size());
    b.count = count;
    b.half_width = as_float(w[2]);
    b.base = sprite_base_;
    b.offset = pcw_texture(current_.pcw) ? sprite_offset_ : 0;
    Polygon p = current_;
    p.first = static_cast<u32>(frame_.lines.size());
    p.count = 0;
    p.line_batch = true;
    p.may_be_far = false;
    frame_.lines.push_back(b);
    frame_.lists[list_].push_back(p);
    if (line_words_ == 0)
        return;
    // The words run on in whole records: the rest of the last one is padding.
    frame_.line_edges.reserve(frame_.line_edges.size() + count);
}

void DisplayList::take_line_words(const std::uint32_t w[8]) {
    for (unsigned i = 0; i < 8; ++i) {
        if (line_words_ == 0) {
            line_fill_ = 0;  // padding to the record's end
            continue;
        }
        --line_words_;
        line_[line_fill_++] = as_float(w[i]);
        if (line_fill_ == 6) {
            line_fill_ = 0;
            if (line_keep_)
                frame_.line_edges.push_back({line_[0], line_[1], line_[2], line_[3], line_[4], line_[5]});
        }
    }
}

void DisplayList::handle_header(const std::uint32_t w[8]) {
    const u32 pcw = w[0];
    end_strip();  // a new header ends whatever strip was open
    header_active_ = false;

    // The list type is latched by the first header after an end of list and ignored on every
    // header after it until the list is closed, as the hardware and Flycast's ta_vtx.cpp
    // (startList, GPL-2.0) do: translucent sprites may carry list-type bits of 0, and read per
    // header they would land in the opaque list.
    if (list_ >= kListCount) {
        const unsigned list = pcw_list_type(pcw);
        if (list >= kListCount)
            return;
        list_ = list;
    }

    current_ = Polygon{};
    strip_far_ = false;
    current_.pcw = pcw;
    current_.isp = w[1];
    current_.tsp = w[2];
    current_.tcw = w[3];
    current_.tile_clip = pcw_user_clip(pcw);
    current_.clip_x0 = clip_x0_;
    current_.clip_y0 = clip_y0_;
    current_.clip_x1 = clip_x1_;
    current_.clip_y1 = clip_y1_;
    current_.first = static_cast<u32>(frame_.vertices.size());

    if (pcw_para_type(pcw) == kSprite) {
        // A sprite header carries the colours for all four corners and is always 32 bytes.
        // Its corners come in the opposite winding, so the cull mode's low bit flips (Flycast
        // ta_vtx.cpp: isp.CullMode ^= 1).
        current_.isp ^= 1u << 27;
        sprite_base_ = w[4];
        sprite_offset_ = w[5];
        in_sprite_ = true;
        header_active_ = false;
        return;
    }
    in_sprite_ = false;

    if (list_ == static_cast<unsigned>(ListType::OpaqueModifier) ||
        list_ == static_cast<unsigned>(ListType::TranslucentModifier)) {
        // A modifier volume header only sets the ISP word; its vertices follow.
        header_active_ = true;
        ++modifier_headers_;
        modifier_last_ = pcw_volume(pcw) != 0;
        modifier_translucent_ = list_ == static_cast<unsigned>(ListType::TranslucentModifier);
        return;
    }

    if (pcw_volume(pcw)) {
        current_.tsp1 = w[4];
        current_.tcw1 = w[5];
    }
    // Colour type 3 reuses the previous polygon's face colour, so only the other intensity mode
    // sets one. Where it lives depends on the header's size: a 64-byte header carries it in the
    // second half, and a 32-byte one in words 4 to 7, which are the words a header with no face
    // colour leaves unused. Treating the 32-byte case as "no face colour" and substituting white
    // washed out every polygon that used it.
    if (header_is_64(pcw)) {
        pending_ = Pending::PolyHeader;
    } else if (pcw_col_type(pcw) == kIntensity) {
        face_base_ = float_colour(&w[4]);
        face_offset_ = 0;
    }
    header_active_ = true;
}

void DisplayList::handle_vertex(const std::uint32_t w[8], bool have_second_half,
                                const std::uint32_t second[8]) {
    const u32 pcw = current_.pcw;
    Vertex v;
    v.x = as_float(w[1]);
    v.y = as_float(w[2]);
    v.z = as_float(w[3]);

    const u32 col = pcw_col_type(pcw);
    const bool textured = pcw_texture(pcw) != 0;
    const bool offset = pcw_offset(pcw) != 0;
    const bool uv_short = pcw_uv_16bit(pcw) != 0;

    // Words 4 and 5 are the texture coordinates when textured; otherwise they are ignored, and the
    // colour (packed or intensity) is word 6 either way. The exception is an untextured
    // two-volume vertex, whose two colours are words 4 and 5 (Flycast's TA_Vertex2, 9 and 10 in
    // ta_structs.h). Reading word 4 for an ordinary untextured intensity vertex took an ignored
    // word as its intensity.
    const bool two_volume_flat = !textured && pcw_volume(pcw) != 0;
    unsigned next = two_volume_flat ? 4 : 6;
    if (textured) {
        if (uv_short) {
            // 16-bit coordinates share one word, v in the low half and u in the high half.
            v.v = uv16(w[4] & 0xFFFFu);
            v.u = uv16(w[4] >> 16);
            next = 6;  // the following word is ignored
        } else {
            v.u = as_float(w[4]);
            v.v = as_float(w[5]);
            next = 6;
        }
    }

    switch (col) {
        case kPacked:
            v.base = w[next];
            v.offset = offset && !two_volume_flat ? w[7] : 0;
            break;
        case kFloating:
            if (have_second_half) {
                v.base = float_colour(&second[0]);
                v.offset = offset ? float_colour(&second[4]) : 0;
            } else {
                // Untextured floating colour fits in the first chunk, at words 4..7.
                v.base = float_colour(&w[4]);
                v.offset = 0;
            }
            break;
        case kIntensity:
        case kIntensityPrev: {
            const float base_i = as_float(w[next]);
            v.base = intensity_colour(quotients_, face_base_, base_i);
            v.offset = offset && !two_volume_flat
                           ? intensity_colour(quotients_, face_offset_, as_float(w[next + 1]))
                           : 0;
            break;
        }
        default:
            break;
    }

    strip_far_ = strip_far_ || outside_far(v);
    frame_.vertices.push_back(v);
    ++current_.count;
    if (pcw_end_of_strip(w[0]))
        end_strip();
}

// A sprite is a quad given as three corners plus the x and y of the fourth; its z and texture
// coordinates are found by solving the plane the other three define (Flycast's
// CaclulateSpritePlane). The result is emitted as a four-vertex strip in the order the hardware
// rasterises: corner 3, corner 2, corner 0, corner 1.
void DisplayList::handle_sprite_vertex(const std::uint32_t a[8], const std::uint32_t b[8]) {
    const bool textured = pcw_texture(current_.pcw) != 0;

    Vertex q[4];
    for (Vertex& v : q) {
        v.base = sprite_base_;
        v.offset = textured ? sprite_offset_ : 0;
    }
    // Words: x0 y0 z0 x1 y1 z1 x2 | y2 z2 x3 y3 (u0v0) (u1v1) (u2v2)
    q[2].x = as_float(a[1]);
    q[2].y = as_float(a[2]);
    q[2].z = as_float(a[3]);
    q[3].x = as_float(a[4]);
    q[3].y = as_float(a[5]);
    q[3].z = as_float(a[6]);
    q[1].x = as_float(a[7]);
    q[1].y = as_float(b[0]);
    q[1].z = as_float(b[1]);
    q[0].x = as_float(b[2]);
    q[0].y = as_float(b[3]);

    if (textured) {
        // Packed 16-bit coordinates, v in the low half and u in the high half, for corners 0..2.
        q[2].u = uv16(b[5] >> 16);
        q[2].v = uv16(b[5] & 0xFFFFu);
        q[3].u = uv16(b[6] >> 16);
        q[3].v = uv16(b[6] & 0xFFFFu);
        q[1].u = uv16(b[7] >> 16);
        q[1].v = uv16(b[7] & 0xFFFFu);
    }

    // Solve for the fourth corner on the plane of the other three.
    const Vertex& A = q[2];
    const Vertex& B = q[3];
    const Vertex& C = q[1];
    Vertex& P = q[0];
    const float ab_x = B.x - A.x, ab_y = B.y - A.y, ab_z = B.z - A.z;
    const float ac_x = C.x - A.x, ac_y = C.y - A.y, ac_z = C.z - A.z;
    const float ap_x = P.x - A.x, ap_y = P.y - A.y;
    const float k3 = ac_x * ab_y - ac_y * ab_x;
    if (k3 != 0.0f) {
        const float k2 = (ap_x * ab_y - ap_y * ab_x) / k3;
        const float k1 = ab_x != 0.0f ? (P.x - A.x - k2 * ac_x) / ab_x
                                      : (ab_y != 0.0f ? (P.y - A.y - k2 * ac_y) / ab_y : 0.0f);
        P.z = A.z + k1 * ab_z + k2 * ac_z;
        P.u = A.u + k1 * (B.u - A.u) + k2 * (C.u - A.u);
        P.v = A.v + k1 * (B.v - A.v) + k2 * (C.v - A.v);
    } else {
        P.z = A.z;
    }

    current_.first = static_cast<u32>(frame_.vertices.size());
    current_.count = 4;
    current_.may_be_far = false;
    for (const Vertex& v : q) {
        current_.may_be_far = current_.may_be_far || outside_far(v);
        frame_.vertices.push_back(v);
    }
    if (list_ < kListCount)
        frame_.lists[list_].push_back(current_);
    // Each sprite is its own quad; the header stays active for the next one.
    current_.count = 0;
}

// A modifier-volume vertex is one whole triangle and always 64 bytes: nine floats, of which the
// last two fall in the second half.
void DisplayList::handle_modifier_vertex(const std::uint32_t a[8], const std::uint32_t b[8]) {
    ModifierTriangle t;
    t.isp = current_.isp;
    t.header = modifier_headers_;
    t.volume_last = modifier_last_;
    t.translucent = modifier_translucent_;
    t.x[0] = as_float(a[1]);
    t.y[0] = as_float(a[2]);
    t.z[0] = as_float(a[3]);
    t.x[1] = as_float(a[4]);
    t.y[1] = as_float(a[5]);
    t.z[1] = as_float(a[6]);
    t.x[2] = as_float(a[7]);
    t.y[2] = as_float(b[0]);
    t.z[2] = as_float(b[1]);
    frame_.modifiers.push_back(t);
}

// Emits the strip just finished. The header stays in force: the hardware lets one header be
// followed by several strips, each ended by the end-of-strip flag on its last vertex.
void DisplayList::end_strip() {
    if (current_.count >= 3 && list_ < kListCount) {
        current_.may_be_far = strip_far_;
        frame_.lists[list_].push_back(current_);
    }
    strip_far_ = false;
    current_.count = 0;
    current_.first = static_cast<u32>(frame_.vertices.size());
}

void DisplayList::close_list() {
    end_strip();
    in_sprite_ = false;
    header_active_ = false;
    list_ = kListCount;
}

// A coordinate is "outside" only past a whole texel, not past the unit square: a quad whose UVs
// run exactly 0..1 is the ordinary case and must read as not tiled, and floating-point rounding in
// the guest's own transform puts those endpoints a hair either side of the mark. Anything that
// genuinely wraps overshoots by whole multiples, so the tolerance can be loose without ever
// mistaking a tiled polygon for a clamped one.
bool polygon_tiles(const Frame& frame, const Polygon& polygon) {
    constexpr float kSlack = 1.0f / 64.0f;
    const std::size_t first = polygon.first;
    const std::size_t last = std::min(frame.vertices.size(), first + polygon.count);
    for (std::size_t i = first; i < last; ++i) {
        const Vertex& v = frame.vertices[i];
        if (v.u < -kSlack || v.u > 1.0f + kSlack)
            return true;
        if (v.v < -kSlack || v.v > 1.0f + kSlack)
            return true;
    }
    return false;
}


namespace {

// A vertex in the form the shaders interpolate: everything multiplied by 1/w (z) but z itself.
struct ClipVertex {
    double x, y, z, uz, vz;
    double base[4], offset[4];
};

ClipVertex to_clip(const Vertex& v) {
    ClipVertex c{v.x, v.y, v.z, v.u * double(v.z), v.v * double(v.z), {}, {}};
    for (int k = 0; k < 4; ++k) {
        c.base[k] = double((v.base >> (8 * k)) & 0xFFu) * v.z;
        c.offset[k] = double((v.offset >> (8 * k)) & 0xFFu) * v.z;
    }
    return c;
}

Vertex from_clip(const ClipVertex& c) {
    Vertex v;
    v.x = static_cast<float>(c.x);
    v.y = static_cast<float>(c.y);
    v.z = static_cast<float>(c.z);
    const double iz = c.z != 0.0 ? 1.0 / c.z : 0.0;
    v.u = static_cast<float>(c.uz * iz);
    v.v = static_cast<float>(c.vz * iz);
    for (int k = 0; k < 4; ++k) {
        const auto ch = [&](double q) {
            return static_cast<std::uint32_t>(std::clamp(std::lround(q * iz), 0l, 255l));
        };
        v.base |= ch(c.base[k]) << (8 * k);
        v.offset |= ch(c.offset[k]) << (8 * k);
    }
    return v;
}

ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, double t) {
    ClipVertex r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.uz = a.uz + (b.uz - a.uz) * t;
    r.vz = a.vz + (b.vz - a.vz) * t;
    for (int k = 0; k < 4; ++k) {
        r.base[k] = a.base[k] + (b.base[k] - a.base[k]) * t;
        r.offset[k] = a.offset[k] + (b.offset[k] - a.offset[k]) * t;
    }
    return r;
}

// A triangle's corners as the edges cut it, in place. An edge adds at most one corner to a convex
// polygon and in any case no more than half the corners it had, so a triangle cut by four edges
// has at most 13.
struct ClipPoly {
    std::array<ClipVertex, 16> v;
    std::size_t n = 0;
};

// Sutherland-Hodgman against one edge: keep where side(v) >= 0, side linear in x and y. The
// result goes to `out`.
template <class Side>
void clip_edge(const ClipPoly& poly, ClipPoly& out, Side side) {
    out.n = 0;
    for (std::size_t i = 0; i < poly.n; ++i) {
        const ClipVertex& a = poly.v[i];
        const ClipVertex& b = poly.v[(i + 1) % poly.n];
        const double sa = side(a), sb = side(b);
        if (sa >= 0.0)
            out.v[out.n++] = a;
        if ((sa >= 0.0) != (sb >= 0.0))
            out.v[out.n++] = lerp(a, b, sa / (sa - sb));
    }
}

}  // namespace

std::size_t clip_far_polygons(Frame& frame, float limit) {
    constexpr double cx = 320.0, cy = 240.0;
    const double x0 = cx - limit, x1 = cx + limit, y0 = cy - limit, y1 = cy + limit;
    // The bounds in float when they are exactly floats (the default limit's are), so the test is
    // float compares with the same answers; outside first, since a NaN is never outside and an
    // infinity is not far.
    const float fx0 = static_cast<float>(x0), fx1 = static_cast<float>(x1),
                fy0 = static_cast<float>(y0), fy1 = static_cast<float>(y1);
    const bool exact = fx0 == x0 && fx1 == x1 && fy0 == y0 && fy1 == y1;
    auto far = [&](const Vertex& v) {
        if (exact) {
            if (!(v.x < fx0 || v.x > fx1 || v.y < fy0 || v.y > fy1))
                return false;
        } else if (!(v.x < x0 || v.x > x1 || v.y < y0 || v.y > y1)) {
            return false;
        }
        return std::isfinite(v.x) && std::isfinite(v.y);
    };
    // The decoder's mark holds for the default bounds: a strip it cleared has no far vertex.
    const bool marked = limit == kFarLimit;
    std::size_t clipped = 0;
    for (auto& list : frame.lists) {
        bool any = false;
        for (const auto& p : list) {
            if (marked && !p.may_be_far)
                continue;
            for (std::uint32_t k = 0; k < p.count && !any; ++k)
                any = far(frame.vertices[p.first + k]);
            if (any)
                break;
        }
        if (!any)
            continue;
        // The list from its first cut polygon on is rebuilt aside and copied back after the
        // polygons before the cut. When those are fewer than the ones after it, they are copied
        // aside too and the rebuilt list takes the list's place instead: the smaller copy.
        thread_local std::vector<Polygon> out;
        out.clear();
        std::size_t first_cut = list.size();
        bool whole = false;  // `out` holds the whole list, the polygons before the cut included
        for (std::size_t n = 0; n < list.size(); ++n) {
            const Polygon& p = list[n];
            bool cut = false;
            if (!marked || p.may_be_far) {
                bool hit = false, finite = true;
                for (std::uint32_t k = 0; k < p.count; ++k) {
                    const Vertex& v = frame.vertices[p.first + k];
                    hit = hit || far(v);
                    finite = finite && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
                }
                cut = hit && finite && p.count >= 3;
            }
            if (!cut) {
                if (first_cut != list.size())
                    out.push_back(p);
                continue;
            }
            if (first_cut == list.size()) {
                first_cut = n;
                whole = n < list.size() - n;
                if (whole)
                    out.insert(out.end(), list.begin(), list.begin() + static_cast<std::ptrdiff_t>(n));
            }
            ++clipped;
            float strip_min = 1e30f;
            for (std::uint32_t k = 0; k < p.count; ++k)
                strip_min = std::min(strip_min, frame.vertices[p.first + k].z);
            for (std::uint32_t k = 0; k + 2 < p.count; ++k) {
                // Strip triangle k, wound as the strip draws it (odd ones are reversed).
                std::uint32_t ia = p.first + k, ib = p.first + k + 1;
                const std::uint32_t ic = p.first + k + 2;
                if (k & 1u)
                    std::swap(ia, ib);
                ClipPoly ping, pong;
                ping.v[0] = to_clip(frame.vertices[ia]);
                ping.v[1] = to_clip(frame.vertices[ib]);
                ping.v[2] = to_clip(frame.vertices[ic]);
                ping.n = 3;
                clip_edge(ping, pong, [&](const ClipVertex& v) { return v.x - x0; });
                clip_edge(pong, ping, [&](const ClipVertex& v) { return x1 - v.x; });
                clip_edge(ping, pong, [&](const ClipVertex& v) { return v.y - y0; });
                clip_edge(pong, ping, [&](const ClipVertex& v) { return y1 - v.y; });
                const ClipPoly& poly = ping;
                const float tri_mean = (frame.vertices[ia].z + frame.vertices[ib].z +
                                        frame.vertices[ic].z) * (1.0f / 3.0f);
                for (std::size_t i = 1; i + 1 < poly.n; ++i) {
                    Polygon t = p;
                    t.sort_triangle = std::isnan(p.sort_triangle) ? tri_mean : p.sort_triangle;
                    t.sort_strip = std::isnan(p.sort_strip) ? strip_min : p.sort_strip;
                    t.first = static_cast<std::uint32_t>(frame.vertices.size());
                    t.count = 3;
                    frame.vertices.push_back(from_clip(poly.v[0]));
                    frame.vertices.push_back(from_clip(poly.v[i]));
                    frame.vertices.push_back(from_clip(poly.v[i + 1]));
                    out.push_back(t);
                }
            }
        }
        if (whole) {
            list.swap(out);
        } else if (first_cut != list.size()) {
            list.resize(first_cut);
            list.insert(list.end(), out.begin(), out.end());
        }
    }
    // Modifier-volume triangles too: shadow volumes reach down and, with the camera
    // tilted up, behind it (x = -1e8). Each is a parity flip in the stencil, so a coverage error
    // from the clipper is a line of wrongly shaded pixels. The pieces fan a convex polygon, so each pixel is covered by one of them
    // as it was by the triangle; outside the rectangle there is nothing to shade.
    bool any_mod = false;
    for (const auto& m : frame.modifiers)
        for (int k = 0; k < 3 && !any_mod; ++k)
            any_mod = far(Vertex{m.x[k], m.y[k], m.z[k]});
    if (any_mod) {
        std::vector<ModifierTriangle> out;
        out.reserve(frame.modifiers.size() + 64);
        for (const auto& m : frame.modifiers) {
            bool hit = false, finite = true;
            for (int k = 0; k < 3; ++k) {
                hit = hit || far(Vertex{m.x[k], m.y[k], m.z[k]});
                finite = finite && std::isfinite(m.x[k]) && std::isfinite(m.y[k]) &&
                         std::isfinite(m.z[k]);
            }
            if (!hit || !finite) {
                out.push_back(m);
                continue;
            }
            ++clipped;
            ClipPoly ping, pong;
            for (int k = 0; k < 3; ++k)
                ping.v[ping.n++] = ClipVertex{m.x[k], m.y[k], m.z[k], 0, 0, {}, {}};
            clip_edge(ping, pong, [&](const ClipVertex& v) { return v.x - x0; });
            clip_edge(pong, ping, [&](const ClipVertex& v) { return x1 - v.x; });
            clip_edge(ping, pong, [&](const ClipVertex& v) { return v.y - y0; });
            clip_edge(pong, ping, [&](const ClipVertex& v) { return y1 - v.y; });
            const ClipPoly& poly = ping;
            for (std::size_t i = 1; i + 1 < poly.n; ++i) {
                ModifierTriangle t = m;
                const ClipVertex* src[3] = {&poly.v[0], &poly.v[i], &poly.v[i + 1]};
                for (int k = 0; k < 3; ++k) {
                    t.x[k] = static_cast<float>(src[k]->x);
                    t.y[k] = static_cast<float>(src[k]->y);
                    t.z[k] = static_cast<float>(src[k]->z);
                }
                out.push_back(t);
            }
        }
        frame.modifiers.swap(out);
    }
    return clipped;
}

}  // namespace dream::render
