// Upscaled 2D without bleeding.
//
// Fonts and HUD art are packed tight in their textures and drawn as screen-aligned quads that map
// one texel to one console pixel, bilinear filtered. At the console's 640x480 every pixel centre
// lands on a texel centre, so the filter returns the texel itself: the picture is point sampled.
// Rendered at 2x or more, the edge pixels sample a quarter texel outside the quad and pull in the
// neighbouring glyph or a transparent border: thin frames around every character. Point sampling those quads -- and only those -- gives at any scale exactly
// what the console showed at 1x.
#pragma once

#include <cstddef>

namespace dream::render {
struct Frame;
}

namespace dreamcomp {

// Switches bilinear to point sampling on textured quads that are axis-aligned, flat, on whole
// console pixels and map exactly one texel per pixel. Returns how many were switched.
std::size_t sharpen_2d(dream::render::Frame& frame);

}  // namespace dreamcomp
