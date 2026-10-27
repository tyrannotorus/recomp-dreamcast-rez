// Geometry interpolation between two consecutive display lists.
//
// Dreamcast games transform vertices on the CPU, so a display list holds screen-space geometry and
// no matrices. To draw a frame between two game frames, each strip of the newer frame is paired
// with the same strip of the older one and their vertex positions are blended. Strips carry no
// identity; pairing relies on games emitting their scene in a stable order. A strip pairs with
// one of the same signature
// -- the parameter words (PCW/ISP/TSP/TCW) and vertex count -- in submission order, skipping at most
// `window` unmatched strips on either side (objects appearing, disappearing or clipped differently).
#pragma once

#include <cstdint>
#include <vector>

#include "dream/render/display_list.h"

namespace dream::render {

struct StripPair {
    std::uint32_t list = 0;  // list index
    std::uint32_t a = 0;     // polygon index in the older frame's list
    std::uint32_t b = 0;     // polygon index in the newer frame's list
};

struct MatchStats {
    std::uint32_t strips_a = 0, strips_b = 0, matched = 0;
    std::uint32_t vertices_b = 0, matched_vertices = 0;
};

MatchStats match_frames(const Frame& older, const Frame& newer, unsigned window,
                        std::vector<StripPair>& out);

struct BlendOptions {
    float t = 0.5f;               // 0 = older, 1 = newer
    unsigned window = 8;          // match lookahead
    float max_jump = 64.0f;       // a paired vertex moving further (guest px) is not blended
    float cut_median = 40.0f;     // median movement above this: a camera cut, no blending at all
    // A textured strip pairs only if every vertex keeps its texture coordinates.
    // World geometry carries fixed UVs, so two strips that merely share parameter words and a
    // vertex count -- the many terrain strips of one texture when the game culls or adds some
    // while the camera turns -- do not pair with each other and get blended across the screen.
    bool match_uv = true;
    float uv_tolerance = 1.0f / 1024.0f;
    // With its texture coordinates confirmed, a strip is the same piece of the world, so it may
    // move further than max_jump: a fast camera turn moves near geometry hundreds of pixels per
    // game frame, and refusing those strips is what opened the seams.
    float max_jump_uv = 640.0f;
    // Strips that cannot be blended stay where the newer frame has them, and their blended
    // neighbours move: the seams between them open. When more than this share of the drawn
    // screen area would be left unblended, the in-between frame is dropped like a cut.
    float max_unblended = 0.01f;
};

struct BlendStats {
    MatchStats match;
    std::uint32_t blended_strips = 0, jumped_strips = 0;
    float median_motion = 0.0f;
    bool cut = false;
    std::uint32_t uv_mismatched_strips = 0;  // paired by words, refused by texture coordinates
    std::uint32_t repaired_strips = 0;       // paired by texture coordinates out of list order
    float unblended_share = 0.0f;            // of the drawn screen area, in a moving scene
    bool torn = false;                       // dropped: too much would be left unblended
};

// `out` becomes `newer` with every paired, plausibly moving strip's positions (x, y and 1/w)
// blended towards `older`. Texture coordinates and colours come from `newer`. Returns false (and
// leaves `out` equal to `newer`) on a camera cut, or when the blend would tear (BlendStats::torn). `pairs` is scratch storage, kept by the caller.
bool blend_frames(const Frame& older, const Frame& newer, const BlendOptions& opt,
                  std::vector<StripPair>& pairs, Frame& out, BlendStats& stats);

}  // namespace dream::render
