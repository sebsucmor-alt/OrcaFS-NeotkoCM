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
uniform float u_material_td[16];      // scalar TD per tool, broadcast per RGB channel
uniform bool  u_beer_lambert;        // false = M3 flat-alpha debug mode (peel-order validation)
uniform float u_flat_alpha;          // only used when u_beer_lambert == false
// NEOTKO_REALCOLOR_TAG s20oct (docs/WIP/REALCOLOR_LESSONS_FROM_REALVIEW.md §7.3, el modelo de RealView):
// u_material_rgb = el color del filamento (sRGB, el mismo array que realcolor_peel.fs), para el TD
// CON COLOR; u_diffusion = 1 la luz que se queda en el cordón según el TD (quita fantasmas), 0 = sin;
// u_last_pass = esta es la última pasada del presupuesto: lo que aún pasa se lo lleva este cordón.
uniform vec3  u_material_rgb[16];
uniform float u_diffusion;
uniform bool  u_last_pass;
// 7-nov-2026 (docs/WIP/REALCOLOR_MODEL_REVIEW_7NOV.md §1): medio de dos flujos (Kubelka-Munk) por CANAL. Por filamento:
// u_km_ks = K/S por canal, u_km_s = S (1/mm), u_km_rinf = lo que devuelve un bloque grueso. Lo calcula el C++.
uniform bool  u_km_on;
uniform vec3  u_km_ks[16];
uniform float u_km_s[16];
uniform vec3  u_km_rinf[16];

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
        // s253: acotado a mano — clamp() de enteros no existe en GLSL 1.10. Ver realcolor_peel.fs.
        int td_idx = tool;
        if (td_idx < 0)  td_idx = 0;
        if (td_idx > 15) td_idx = 15;
        float td = u_material_td[td_idx];
        vec3 layer_lin = vec3(srgb_to_linear(layer_srgb.r), srgb_to_linear(layer_srgb.g), srgb_to_linear(layer_srgb.b));
        // §7.3: TD con color. Cada canal se apaga según lo que el filamento NO refleja (un amarillo
        // se come el azul), y la media de los tres sigue cumpliendo el TD. Con td 0 = opaco.
        vec3 alb = u_material_rgb[td_idx];
        alb = vec3(srgb_to_linear(alb.r), srgb_to_linear(alb.g), srgb_to_linear(alb.b));
        vec3 k = -log(max(alb, vec3(0.02))) + vec3(0.05);
        vec3 w = vec3(0.15) + 0.85 * k / ((k.r + k.g + k.b) / 3.0);
        vec3 t = (td < 1e-6) ? vec3(0.0) : pow(vec3(0.1), w * (thickness / max(td, 1e-6)));
        // §7.3: difusión según el TD (entera hasta ~1,5 mm, mínimo 0,12): parte de la luz que iba a
        // pasar se queda en el cordón y sale con su color.
        float D = u_diffusion * max(clamp((4.5 - td) / 3.0, 0.0, 1.0), 0.12);
        float esparce = 1.0 - exp(-thickness * D * 5.0);
        vec3 aporta = esparce + (1.0 - esparce) * (1.0 - t);
        vec3 deja = (1.0 - esparce) * t;
        // 7-nov-2026 — medio KM por canal: reflexión R y transmisión T de una capa de grosor `thickness`.
        // `aporta` = R normalizado al bloque grueso (el color iluminado ya viene en layer_lin); la luz que
        // sigue baja Y vuelve a subir por esta capa: T².
        if (u_km_on && td > 1e-6) {
            vec3 ks = u_km_ks[td_idx];
            vec3 ri = u_km_rinf[td_idx];
            float u = u_km_s[td_idx] * thickness;
            vec3 a = vec3(1.0) + ks;
            vec3 b = sqrt(max(a * a - vec3(1.0), vec3(0.0)));
            vec3 bu = min(b * u, vec3(20.0));
            vec3 ep = exp(bu), em = exp(-bu);
            vec3 sh = 0.5 * (ep - em), ch = 0.5 * (ep + em);
            vec3 den = a * sh + b * ch;
            vec3 R = sh / max(den, vec3(1e-6));
            vec3 Tt = b / max(den, vec3(1e-6));
            // canal casi blanco (b ≈ 0): límite R = u/(1+u), T = 1/(1+u)
            if (b.r < 1e-4) { R.r = u / (1.0 + u); Tt.r = 1.0 / (1.0 + u); }
            if (b.g < 1e-4) { R.g = u / (1.0 + u); Tt.g = 1.0 / (1.0 + u); }
            if (b.b < 1e-4) { R.b = u / (1.0 + u); Tt.b = 1.0 / (1.0 + u); }
            aporta = clamp(R / max(ri, vec3(1e-3)), 0.0, 1.0);
            deja = Tt * Tt;
        }
        // §7.4.2: en la última pasada del presupuesto, si aún tocamos plástico, lo que sigue pasando
        // no puede ver el fondo: se lo lleva este cordón (sin esto la pieza gruesa se transparenta).
        if (u_last_pass) { aporta += deja; deja = vec3(0.0); }
        out_accum_color = vec4(prevC + T * aporta * layer_lin, 1.0);
        out_accum_transmit = vec4(T * deja, 1.0);
    } else {
        // M3 debug mode: flat non-physical alpha blend to validate peel order/geometry only.
        float a = clamp(u_flat_alpha, 0.0, 1.0);
        out_accum_color = vec4(prevC + T * a * layer_srgb, 1.0);
        out_accum_transmit = vec4(T * (1.0 - a), 1.0);
    }
}
