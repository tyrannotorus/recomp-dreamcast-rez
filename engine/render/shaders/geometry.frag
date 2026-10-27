// Geometry shading. The depth is the vertex shader's interpolated 1/w (geometry.vert).
//
// How the texture and the vertex colour combine is the TSP word's shading instruction. The four
// modes are the hardware's: "decal" means the texture replaces the colour, "modulate" means it
// multiplies it, and the two "alpha" variants also use the vertex colour's alpha.
//
// The varyings arrive perspective-correct, so the texture is read at the interpolated coordinate
// as is (no dependent read). Colours are mediump, as they end at 8 bits a channel; the texture
// coordinate and the alpha test stay full precision. The vertex shaders' outputs must match these
// precisions: some PowerVR drivers misread a mediump input fed by a highp output.
#version 450
// Full precision unless a declaration says mediump.
precision highp float;
precision highp int;

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    int mode;         // bit 0-1 shading instruction, 2 textured, 3 ignore texture alpha,
                      // 4 offset colour, 5 alpha test, 6 use alpha
    float alpha_ref;  // punch-through threshold, from the hardware's own register
    vec4 fog_vert;    // FOG_COL_VERT: per-vertex fog blends toward it by the offset alpha
    vec4 fog_ram;     // FOG_COL_RAM: table fog's colour
    vec4 clip;        // mode bit 9: discard inside this rectangle (user tile clip mode 3)
}
push;
// mode bits 7-8: the TSP fog control: 0 table, 1 per vertex, 2 none, 3 table alpha.

// The mode as a specialization constant, so each pipeline's shader is compiled for its own mode
// (the renderer always sets it; -1 would read push.mode instead).
layout(constant_id = 0) const int kMode = -1;

layout(set = 0, binding = 0) uniform mediump sampler2D tex;

layout(location = 0) in mediump vec4 v_base;
layout(location = 1) in mediump vec4 v_offset;
layout(location = 2) in vec2 v_uv;
layout(location = 3) noperspective in mediump float v_fog;

layout(location = 0) out mediump vec4 o_colour;

// Also built with NO_DISCARD for pipelines that need neither discard below (no alpha test, no
// user clip mode 3): on a tile-based GPU any discard in a shader disables hidden-surface removal.
void main() {
    const int mode = kMode >= 0 ? kMode : push.mode;
#ifndef NO_DISCARD
    if ((mode & 512) != 0 && gl_FragCoord.x >= push.clip.x && gl_FragCoord.x < push.clip.z &&
        gl_FragCoord.y >= push.clip.y && gl_FragCoord.y < push.clip.w)
        discard;
#endif
    mediump vec4 base = v_base;
    // "Use alpha" clear means the surface is opaque however its colours are encoded: the vertex
    // alpha is data for something else and must not reach the blender, even for a polygon in the
    // translucent list.
    if ((mode & 64) == 0)
        base.a = 1.0;
    mediump vec4 colour = base;
    const mediump vec3 fog_ram = push.fog_ram.rgb;
    const int fog = (mode >> 7) & 3;
    // As Flycast's shader: fog mode 3 replaces the colour with the fog colour and the table's
    // factor as alpha, before the texture is applied.
    if (fog == 3)
        colour = vec4(fog_ram, v_fog);

    if ((mode & 4) != 0) {
        mediump vec4 texel = texture(tex, v_uv);
        if ((mode & 8) != 0)
            texel.a = 1.0;
        const int shading = mode & 3;
        if (shading == 0) {  // decal
            colour = texel;
        } else if (shading == 1) {  // modulate
            colour = vec4(texel.rgb * base.rgb, texel.a);
        } else if (shading == 2) {  // decal, blended by the texture's alpha
            colour = vec4(mix(base.rgb, texel.rgb, texel.a), base.a);
        } else {  // modulate, alphas multiplied
            colour = texel * base;
        }
    }
    if ((mode & 16) != 0)
        colour.rgb += v_offset.rgb;

    // Fog after the colour is complete (Flycast shaders.cpp, after colorClamp).
    if (fog == 0) {
        colour.rgb = mix(colour.rgb, fog_ram, clamp(v_fog, 0.0, 1.0));
    } else if (fog == 1 && (mode & 16) != 0) {
        const mediump vec3 fog_vert = push.fog_vert.rgb;
        colour.rgb = mix(colour.rgb, fog_vert, clamp(v_offset.a, 0.0, 1.0));
    }

    // Punch-through polygons are opaque geometry with holes in it: the hardware keeps or discards
    // a pixel outright rather than blending, so the list can be drawn with depth writes on and in
    // any order. Anything at or below the threshold is not drawn at all.
#ifndef NO_DISCARD
    if ((mode & 32) != 0) {
        // As Flycast: the test sees the alpha at 8 bits, and what passes is opaque.
        const highp float a = round(float(colour.a) * 255.0) / 255.0;
        if (a < push.alpha_ref)
            discard;
        colour.a = 1.0;
    }
#endif

    o_colour = clamp(colour, 0.0, 1.0);
}
