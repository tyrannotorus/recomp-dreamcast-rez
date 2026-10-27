// Modifier volumes: triangles that only touch the stencil buffer, and the full-screen
// quad that darkens what they marked. Same screen-space convention as geometry.vert: x and y in
// pixels, z is 1/w (larger is nearer).
#version 450

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    int mode;
    float alpha;  // the final quad's darkening: 1 - FPU_SHAD_SCALE / 256
} push;

layout(location = 0) in vec3 in_pos;

layout(location = 0) noperspective out float v_inv_w;

// geometry.vert's: the volumes' depth must come out exactly as the geometry's.
vec4 clip_position(vec3 p) {
    const float inv_w = clamp(p.z, 1e-20, 131072.0);
    const float w = 1.0 / inv_w;
    return vec4((p.xy * push.scale + push.offset) * w, 1.0 / 131072.0, w);
}

void main() {
    v_inv_w = in_pos.z;
    gl_Position = clip_position(in_pos);
}
