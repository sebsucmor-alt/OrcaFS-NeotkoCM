#version 110

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_accum.fs. Same
// front-to-back Beer-Lambert transmittance accumulate math (see the 140 file for the full
// derivation/reference to ColorSci::blend_stacked and test_realcolor_blend.cpp) — only the
// MRT mechanism differs: gl_FragData[N] (core GLSL 1.10) instead of layout(location=N) out.
// Runs in LINEAR RGB the whole time (accum textures are GL_RGBA16F, NOT sRGB); gamma is
// applied once, at the very end, in realcolor_present.fs.

uniform sampler2D u_peel_color;
uniform sampler2D u_peel_meta;
uniform sampler2D u_prev_accum_color;
uniform sampler2D u_prev_accum_transmit;
uniform float u_material_td[16];      // scalar TD per tool, broadcast per RGB channel
uniform bool  u_beer_lambert;        // false = M3 flat-alpha debug mode (peel-order validation)
uniform float u_flat_alpha;          // only used when u_beer_lambert == false

varying vec2 uv;

float srgb_to_linear(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

void main()
{
    vec4 meta  = texture2D(u_peel_meta, uv);
    vec3 prevC = texture2D(u_prev_accum_color, uv).rgb;
    vec3 T     = texture2D(u_prev_accum_transmit, uv).rgb;

    // no geometry in this peel layer here, or transmittance already negligible (physical
    // early-exit, see 03_DEPTH_PEELING_RENDER.md): carry the accumulator through unchanged
    if (meta.a < 0.5 || max(T.r, max(T.g, T.b)) < 0.02) {
        gl_FragData[0] = vec4(prevC, 1.0);
        gl_FragData[1] = vec4(T, 1.0);
        return;
    }

    int tool = int(meta.r + 0.5);
    float thickness = meta.g;
    vec3 layer_srgb = texture2D(u_peel_color, uv).rgb;

    if (u_beer_lambert) {
        // ColorSci.cpp::blend_stacked: t = pow(0.1, thickness/td), linear RGB. TD is scalar
        // per tool (see refresh_realcolor_materials), broadcast to all 3 channels here.
        // s253: acotado a mano — clamp() de enteros no existe en GLSL 1.10. Ver realcolor_peel.fs.
        int td_idx = tool;
        if (td_idx < 0)  td_idx = 0;
        if (td_idx > 15) td_idx = 15;
        float td = u_material_td[td_idx];
        float tt = (td < 1e-6) ? 0.0 : pow(0.1, thickness / max(td, 1e-6));
        vec3 t = vec3(tt);
        vec3 layer_lin = vec3(srgb_to_linear(layer_srgb.r), srgb_to_linear(layer_srgb.g), srgb_to_linear(layer_srgb.b));
        gl_FragData[0] = vec4(prevC + T * (1.0 - t) * layer_lin, 1.0);
        gl_FragData[1] = vec4(T * t, 1.0);
    } else {
        // M3 debug mode: flat non-physical alpha blend to validate peel order/geometry only.
        float a = clamp(u_flat_alpha, 0.0, 1.0);
        gl_FragData[0] = vec4(prevC + T * a * layer_srgb, 1.0);
        gl_FragData[1] = vec4(T * (1.0 - a), 1.0);
    }
}
