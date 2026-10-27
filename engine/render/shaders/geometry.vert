// Opaque geometry. The Tile Accelerator's coordinates are already in screen space, so there is no
// model or view transform: x and y are pixels, and z is 1/w (larger is nearer).
//
// The position goes out in clip space with the vertex's own w (x and y times w), so the GPU
// interpolates the varyings perspective-correctly, as the PowerVR does, and the fragment shader
// samples the texture without a dependent read.
#version 450

layout(push_constant) uniform Push {
    vec2 scale;   // 2 / the region of the guest screen being shown
    vec2 offset;
    int mode;         // used by the fragment shader
    float alpha_ref;  // used by the fragment shader
    vec4 fog_vert;    // used by the fragment shader
    vec4 fog_ram;
    vec4 clip;
} push;

layout(location = 0) in vec3 in_pos;   // x, y in pixels; z is 1/w
layout(location = 1) in vec2 in_uv;
// The ARGB words as R8G8B8A8_UNORM: their bytes are B, G, R, A, put back in order below.
layout(location = 2) in vec4 in_base;
layout(location = 3) in vec4 in_offset;
layout(location = 4) in float in_fog;  // table fog factor, worked out per vertex

layout(location = 0) out mediump vec4 v_base;
layout(location = 1) out mediump vec4 v_offset;
layout(location = 2) out vec2 v_uv;
// The table fog factor is per vertex in screen space: not perspective-corrected.
layout(location = 3) noperspective out mediump float v_fog;

// The clip-space position of a vertex at x, y (pixels) with 1/w = z; lines.vert and modvol.vert
// have the same, so the three give the same depth for the same vertex.
vec4 clip_position(vec3 p) {
    // 1/w within (0, 2^17]: as far as the depth range reaches, and never 0 (w would be infinite).
    const float inv_w = clamp(p.z, 1e-20, 131072.0);
    const float w = 1.0 / inv_w;
    // The depth is 1/w itself, as the PowerVR keeps it, so no fragment shader writes depth (which
    // disables hidden-surface removal on a tile-based GPU): z = 2^-17 makes z / w = 1/w * 2^-17.
    return vec4((p.xy * push.scale + push.offset) * w, 1.0 / 131072.0, w);
}

void main() {
    v_base = in_base.zyxw;
    v_offset = in_offset.zyxw;
    v_uv = in_uv;
    v_fog = in_fog;
    gl_Position = clip_position(in_pos);
}
