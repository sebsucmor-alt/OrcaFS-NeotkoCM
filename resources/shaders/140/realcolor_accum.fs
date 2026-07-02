#version 140
#extension GL_ARB_explicit_attrib_location : require

// NEOTKO_REALCOLOR_TAG: one accumulate step per peel pass. Reads this peel's (color, meta)
// and the running accumulator, writes the updated accumulator. Front-to-back transmittance
// accumulate: acc += T*(1-t)*layer; T *= t — mathematically equivalent to
// ColorSci::blend_stacked()'s bottom-to-top recurrence for the same stack (see plan doc
// math note; verified by tests/libslic3r/test_realcolor_blend.cpp). Runs in LINEAR RGB the
// whole time (accum textures are GL_RGBA16F, NOT sRGB) — gamma is applied once, at the very
// end, in realcolor_present.fs, to avoid repeated 8-bit quantization over up to N_max passes.

uniform sampler2D u_peel_color;
uniform sampler2D u_peel_meta;
uniform sampler2D u_prev_accum_color;
uniform sampler2D u_prev_accum_transmit;
uniform float u_material_td[4];      // scalar TD per tool, broadcast per RGB channel
uniform bool  u_beer_lambert;        // false = M3 flat-alpha debug mode (peel-order validation)
uniform float u_flat_alpha;          // only used when u_beer_lambert == false

in vec2 uv;
layout(location = 0) out vec4 out_accum_color;
layout(location = 1) out vec4 out_accum_transmit;

float srgb_to_linear(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

void main()
{
    vec4 meta  = texture(u_peel_meta, uv);
    vec3 prevC = texture(u_prev_accum_color, uv).rgb;
    vec3 T     = texture(u_prev_accum_transmit, uv).rgb;

    // no geometry in this peel layer here, or transmittance already negligible (physical
    // early-exit, see 03_DEPTH_PEELING_RENDER.md): carry the accumulator through unchanged
    if (meta.a < 0.5 || max(T.r, max(T.g, T.b)) < 0.02) {
        out_accum_color = vec4(prevC, 1.0);
        out_accum_transmit = vec4(T, 1.0);
        return;
    }

    int tool = int(meta.r + 0.5);
    float thickness = meta.g;
    vec3 layer_srgb = texture(u_peel_color, uv).rgb;

    if (u_beer_lambert) {
        // ColorSci.cpp::blend_stacked: t = pow(0.1, thickness/td), linear RGB. TD is scalar
        // per tool (see refresh_realcolor_materials), broadcast to all 3 channels here.
        float td = u_material_td[tool];
        float tt = (td < 1e-6) ? 0.0 : pow(0.1, thickness / max(td, 1e-6));
        vec3 t = vec3(tt);
        vec3 layer_lin = vec3(srgb_to_linear(layer_srgb.r), srgb_to_linear(layer_srgb.g), srgb_to_linear(layer_srgb.b));
        out_accum_color = vec4(prevC + T * (1.0 - t) * layer_lin, 1.0);
        out_accum_transmit = vec4(T * t, 1.0);
    } else {
        // M3 debug mode: flat non-physical alpha blend to validate peel order/geometry only.
        float a = clamp(u_flat_alpha, 0.0, 1.0);
        out_accum_color = vec4(prevC + T * a * layer_srgb, 1.0);
        out_accum_transmit = vec4(T * (1.0 - a), 1.0);
    }
}
