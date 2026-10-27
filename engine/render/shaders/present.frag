#version 450
// The guest's own framebuffer, decoded to RGBA and uploaded. Whatever put the pixels there, a
// render or a direct write by the title, they arrive here the same way.
//
// When the image is larger than the area it is drawn into -- which is every run with --scale above
// 1 -- each output pixel covers several source pixels, and this averages all of them. Picking one
// and discarding the rest is what a plain nearest-neighbour sampler does, and it throws away the
// entire point of drawing at a higher resolution: supersampling only becomes anti-aliasing at the
// moment the extra samples are averaged. Sparse sampling of a detailed image is also what makes
// fine detail crawl and sparkle while the camera moves.
//
// Magnification is left alone: `taps` falls to 1 and the sampler's own filter decides, so the
// guest's pixels are still shown as pixels unless Presenter::smooth asks otherwise.
layout(set = 0, binding = 0) uniform sampler2D image;

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    vec2 texel;  // one source texel in texture coordinates
    vec2 taps;   // source texels covered per output pixel, per axis; 1 when magnifying
    vec2 mode;   // x: 1 keeps the texture's (premultiplied) alpha -- an overlay layer
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 colour;

// Four per axis is sixteen samples, which covers --scale 4 into any window big enough to be worth
// looking at. Beyond that the extra taps buy less than they cost.
const int kMaxTaps = 4;

void main() {
    // The covering triangle reaches past the image once it is letterboxed or pillarboxed: nothing
    // outside the image is drawn, so the bars keep the clear colour rather than the image's last
    // row and column smeared across them.
    if (any(lessThan(v_uv, vec2(0.0))) || any(greaterThan(v_uv, vec2(1.0))))
        discard;
    // Rounded up, not truncated. A footprint of 2.67 texels wants three taps; taking two leaves a
    // third of it unsampled, which is the aliasing this exists to remove. The small bias keeps a
    // 1:1 image on the single-tap path, where any averaging would only blur it -- without it a
    // footprint of 1.33, which is what --scale 2 produces in a 960x720 window, would round down to
    // one tap and get nothing.
    const ivec2 n = ivec2(clamp(ceil(push.taps - 0.05), vec2(1.0), vec2(float(kMaxTaps))));
    if (n.x == 1 && n.y == 1) {
        const vec4 t = texture(image, v_uv);
        colour = push.mode.x > 0.5 ? t : vec4(t.rgb, 1.0);
        return;
    }
    // Taps are spread one source texel apart and centred on the output pixel, so the footprint is
    // the region that pixel actually covers rather than something offset to one side of it.
    const vec2 first = v_uv - 0.5 * (vec2(n) - 1.0) * push.texel;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < n.y; ++y)
        for (int x = 0; x < n.x; ++x)
            sum += texture(image, first + vec2(float(x), float(y)) * push.texel);
    sum /= float(n.x * n.y);
    colour = push.mode.x > 0.5 ? sum : vec4(sum.rgb, 1.0);
}
