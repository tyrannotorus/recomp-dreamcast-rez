// Wide lines: a line batch (display_list.h, LineBatch) drawn as one instanced draw, an instance a
// line. Each line becomes the sprite quad Rez's line renderer (0x8C0ACF7E) builds: offset across
// the line by the half width, on the side where the direction's y is not negative, with corners in
// the decoder's sprite order (D, C, A, B) as a triangle strip. The rest is as geometry.vert.
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

layout(location = 0) in vec4 in_a;  // the first end: x, y in pixels, 1/w; its table fog factor
layout(location = 1) in vec4 in_b;  // the second end, the same
layout(location = 2) in vec4 in_base;    // the ARGB word as R8G8B8A8_UNORM (geometry.vert)
layout(location = 3) in vec4 in_offset;
layout(location = 4) in float in_half;   // the half width

layout(location = 0) out mediump vec4 v_base;
layout(location = 1) out mediump vec4 v_offset;
layout(location = 2) out vec2 v_uv;
layout(location = 3) noperspective out mediump float v_fog;

// geometry.vert's.
vec4 clip_position(vec3 p) {
    const float inv_w = clamp(p.z, 1e-20, 131072.0);
    const float w = 1.0 / inv_w;
    return vec4((p.xy * push.scale + push.offset) * w, 1.0 / 131072.0, w);
}

void main() {
    // As 0x8C0ACF7E: f6 = dx, f5 = dy, scaled by the half width over the length (FSRRA), both
    // negated when f5 comes out negative.
    float f6 = in_b.x - in_a.x;
    float f5 = in_b.y - in_a.y;
    const float s = inversesqrt(f6 * f6 + f5 * f5) * in_half;
    f5 *= s;
    f6 *= s;
    if (0.0 > f5) {
        f5 = -f5;
        f6 = -f6;
    }
    vec3 pos;
    float fog;
    switch (gl_VertexIndex) {
        case 0:  // D
            pos = vec3(in_b.x - f5, in_b.y + f6, in_b.z);
            fog = in_b.w;
            break;
        case 1:  // C
            pos = vec3(in_b.x + f5, in_b.y - f6, in_b.z);
            fog = in_b.w;
            break;
        case 2:  // A
            pos = vec3(in_a.x - f5, in_a.y + f6, in_a.z);
            fog = in_a.w;
            break;
        default:  // B
            pos = vec3(in_a.x + f5, in_a.y - f6, in_a.z);
            fog = in_a.w;
            break;
    }
    v_base = in_base.zyxw;
    v_offset = in_offset.zyxw;
    v_uv = vec2(0.0);
    v_fog = fog;
    gl_Position = clip_position(pos);
}
