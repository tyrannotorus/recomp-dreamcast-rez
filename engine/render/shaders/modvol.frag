// Modifier volumes: the depth the volume triangles are tested at must be computed
// exactly as the geometry's (geometry.vert), or the stencil marks the wrong side of every surface.
#version 450

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    int mode;
    float alpha;
} push;

layout(location = 0) noperspective in float v_inv_w;

layout(location = 0) out vec4 o_colour;

void main() {
    o_colour = vec4(0.0, 0.0, 0.0, push.alpha);
}
