// Fog.
//
// The hardware fogs a pixel by its depth, and it does it with a 128-entry table rather than a
// formula, so a title can shape the curve however it likes. Two modes are in real use and they are
// not variations of one idea:
//
//   * **Table fog** looks the depth up in that table and blends towards one colour. The index is
//     built from the floating-point exponent and mantissa of a scaled 1/w, which spreads the 128
//     entries logarithmically over the depth range instead of linearly, so near geometry gets the
//     detail.
//   * **Per-vertex fog** ignores the table and uses the alpha of the polygon's offset colour,
//     interpolated across the triangle, against a different colour register.
//
// Which one a polygon uses is two bits of its TSP word, and a frame mixes them freely. Reference:
// Flycast's `fog_mode2` in core/rend/vulkan/shaders.cpp and its FOG_DENSITY_type (GPL-2.0).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dream::render {

// What the TSP word's two fog bits select.
enum class FogMode : unsigned {
    Table = 0,       // look the depth up in the fog table
    PerVertex = 1,   // use the offset colour's alpha
    None = 2,        // no fog on this polygon
    TableAlpha = 3,  // table lookup, but only the alpha channel is fogged
};

struct FogSettings {
    // FOG_DENSITY is a sign-magnitude float in two bytes: an 8-bit mantissa over a signed 8-bit
    // exponent. Scaling depth by it is the first step of the table lookup.
    float density = 1.0f;
    // The colour table fog blends towards, and the one per-vertex fog uses. They are separate
    // registers and a title can set them differently.
    std::uint32_t table_colour = 0;   // FOG_COL_RAM, as RGBA8888
    std::uint32_t vertex_colour = 0;  // FOG_COL_VERT, as RGBA8888
    // 128 entries of the fog table, each a blend factor and the next step's delta, as the hardware
    // stores them. Kept in the hardware's own form so the shader's index arithmetic is the
    // hardware's too.
    std::array<std::uint8_t, 128> factor{};
    std::array<std::uint8_t, 128> delta{};
    bool clamping = false;  // FOG_CLAMP_MIN/MAX constrain the result
    std::uint32_t clamp_min = 0, clamp_max = 0xFFFFFFFFu;
};

// Reads the fog registers out of the PVR register block (0x2000 bytes from 0x005F8000), including
// the 128-entry table at offset 0x200.
void describe_fog(const std::uint32_t* pvr_regs, FogSettings& out);

// The fog table index the hardware would use for a given 1/w, as a position in 0..128. Exposed so
// the shader's arithmetic can be checked against it on the host, where a wrong curve is a number
// rather than a picture that looks slightly wrong.
float fog_table_index(float density, float inv_w);

// The table fog factor (0..1) for a depth, as Flycast's fog_mode2 samples its 128x2 fog texture
//: the entry the index lands on, blended from the high byte toward the low byte by
// the mantissa's fraction. Inline: it is called for every vertex of a frame.
inline float fog_table_value(const FogSettings& fog, float inv_w) {
    const float z = std::clamp(fog.density * inv_w, 1.0f, 255.9999f);
    // The exponent and mantissa straight from the float's bits, as the hardware indexes. z is in
    // [1, 256), so the exponent is 0 to 7, and the mantissa with a zero exponent field is
    // z / 2^exponent exactly, in [1, 2).
    std::uint32_t bits;
    std::memcpy(&bits, &z, sizeof bits);
    const float exponent = static_cast<float>(static_cast<int>((bits >> 23) & 0xFFu) - 127);
    const std::uint32_t mbits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float mantissa;
    std::memcpy(&mantissa, &mbits, sizeof mantissa);
    const float m = mantissa * 16.0f - 16.0f;
    const int idx = std::clamp(static_cast<int>(std::floor(m) + exponent * 16.0f), 0, 127);
    const float frac = m - std::floor(m);
    // Flycast's texture: row 0 the low byte (`delta` here), row 1 the high byte (`factor`); the
    // lookup's v = 0.75 - frac / 2 runs from row 1 at frac 0 to row 0 at frac 1.
    const float hi = static_cast<float>(fog.factor[static_cast<std::size_t>(idx)]);
    const float lo = static_cast<float>(fog.delta[static_cast<std::size_t>(idx)]);
    return (hi + (lo - hi) * frac) / 255.0f;
}

}  // namespace dream::render
