#version 110

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_present.fs. Single
// output, so this one uses gl_FragColor directly (no MRT needed, no gl_FragData involved) —
// texture2D() replaces texture() and gl_FragDepth stays a builtin in every GLSL version.
// Presents the cached/just-computed composite into whichever framebuffer is bound (the
// default one, when called from render_toolpaths_realcolor()). Writes gl_FragDepth from peel
// pass 0 (the nearest toolpath surface) so the normal GL depth test integrates correctly with
// shells already drawn by render_shells(). Discards where peel pass 0 never wrote anything.
// s166 (item 3): also applies a cheap screen-space AO pass — see compute_ao() below.

uniform sampler2D u_accum_color;   // linear RGB, see realcolor_accum.fs
uniform sampler2D u_peel_meta0;    // pass-0 meta: r=tool_id, g=thickness, b=eye_z (linear mm), a=written
uniform sampler2D u_peel_depth0;   // pass-0 depth
uniform sampler2D u_peel_normal0;  // NEOTKO_REALCOLOR_TAG s166 (item 3): pass-0 packed view-space normal
uniform vec2 u_texel_size;         // 1 / supersampled peel/accum texture resolution
uniform float u_ao_radius;         // px, in the same supersampled texel space as u_texel_size
uniform float u_ao_strength;       // 0 = AO off, 1 = full strength

// NEOTKO_REALCOLOR_TAG s214 (PBR item 2, docs/WIP/REALCOLOR_VIEW/08_PBR_IBL_SSS_PLAN.md):
// single-pass screen-space subsurface scattering — same material TD array realcolor_accum.fs
// already indexes dynamically by tool_id (proven to work on this hardware since s163), reused
// here to scale the blur radius per-pixel by how translucent the surface under it is.
uniform float u_material_td[4];
uniform float u_sss_strength;      // 0 = off (bit-identical to pre-s214), 1 = full blur
uniform float u_sss_radius_px;     // px, same supersampled texel space as u_ao_radius
uniform float u_sss_reference_td;  // mm — a material at exactly this TD blurs at u_sss_radius_px

// NEOTKO_REALCOLOR_TAG s243 (F5 = A7): gradeo final — compresión de rango hacia el gris medio y
// desaturación. Los dos a 1.0 = imagen idéntica a pre-s243. Ver 140/realcolor_present.fs.
uniform float u_grade_contrast;
uniform float u_grade_saturation;

// NEOTKO_REALCOLOR_TAG s243 (F6, "silueta opaca"): 0 = apagado (≡ pre-F6), 1 = sella los huecos
// INTERIORES respetando el contorno. Ver compute_interior(). Detalle en 140/realcolor_present.fs.
uniform float u_fill_interior;

varying vec2 uv;

// NEOTKO_REALCOLOR_TAG s243 (F5): pivote fotográfico del 18% y pesos de luminancia Rec.709.
// Válidos en espacio LINEAR, que es donde trabaja todo esto hasta el gamma final.
const float REALCOLOR_MID_GRAY = 0.18;
const vec3 REALCOLOR_LUMA_709 = vec3(0.2126, 0.7152, 0.0722);

float linear_to_srgb(float c) { c = clamp(c, 0.0, 1.0); return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// NEOTKO_REALCOLOR_TAG s166 (item 3): see 140/realcolor_present.fs for the full rationale
// (classic depth-only SSAO, Kajalin 2007/Crysis, adapted with a normal-agreement weight since
// peel_normal_tex is now available — no view-space reconstruction, screen-space 2D disc only).
// 8 samples unrolled by hand — GLSL 1.10 has no array-constructor syntax to build a kernel array.
const float REALCOLOR_AO_MAX_DELTA_MM = 1.5;

float ao_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir)
{
    vec2 s_uv = center_uv + dir * u_texel_size * u_ao_radius;
    vec4 s_meta = texture2D(u_peel_meta0, s_uv);
    if (s_meta.a < 0.5)
        return 0.0; // background — doesn't occlude
    float delta = center_z - s_meta.b; // positive: neighbor is CLOSER to camera than center
    if (delta <= 0.0)
        return 0.0; // neighbor is farther away or coplanar — not an occluder of this pixel
    float range_falloff = clamp(1.0 - delta / REALCOLOR_AO_MAX_DELTA_MM, 0.0, 1.0);
    vec3 s_n = normalize(texture2D(u_peel_normal0, s_uv).rgb * 2.0 - 1.0);
    float normal_agreement = max(dot(center_n, s_n), 0.0);
    return range_falloff * normal_agreement;
}

float compute_ao(vec2 center_uv)
{
    vec4 center_meta = texture2D(u_peel_meta0, center_uv);
    if (center_meta.a < 0.5) // center tap missed geometry (can happen right at a thin edge,
        return 1.0;          // even though the box filter's 4 taps found coverage) — skip, no darkening
    float center_z = center_meta.b;
    vec3 center_n = normalize(texture2D(u_peel_normal0, center_uv).rgb * 2.0 - 1.0);

    float occlusion = 0.0;
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 1.0,  0.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2(-1.0,  0.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.0,  1.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.0, -1.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.7,  0.7));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2(-0.7,  0.7));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.7, -0.7));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2(-0.7, -0.7));
    occlusion /= 8.0;
    return 1.0 - clamp(occlusion, 0.0, 1.0);
}

// NEOTKO_REALCOLOR_TAG s214 (PBR item 2): single-pass screen-space subsurface scattering —
// same 8-tap disc pattern as compute_ao() above, adapted to accumulate blurred NEIGHBOR COLOR
// (from u_accum_color) instead of occlusion, with the same bilateral edge-stopping (depth delta
// + normal agreement) so the blur never bleeds color across a real silhouette edge. Deliberately
// NOT a separable 2-pass Gaussian (the "textbook" SSS technique) — see 08_PBR_IBL_SSS_PLAN.md for
// why a single disc pass was chosen (reuses these exact bound textures, zero new FBOs/passes).
const float REALCOLOR_SSS_MAX_DELTA_MM = 1.5; // same silhouette-edge cutoff as REALCOLOR_AO_MAX_DELTA_MM

void sss_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir, float radius_px,
                 inout vec3 accum_color, inout float accum_weight)
{
    vec2 s_uv = center_uv + dir * u_texel_size * radius_px;
    vec4 s_meta = texture2D(u_peel_meta0, s_uv);
    if (s_meta.a < 0.5)
        return; // no geometry at this tap — doesn't contribute
    float delta = abs(center_z - s_meta.b);
    float range_falloff = clamp(1.0 - delta / REALCOLOR_SSS_MAX_DELTA_MM, 0.0, 1.0);
    vec3 s_n = normalize(texture2D(u_peel_normal0, s_uv).rgb * 2.0 - 1.0);
    float weight = range_falloff * max(dot(center_n, s_n), 0.0);
    accum_color += texture2D(u_accum_color, s_uv).rgb * weight;
    accum_weight += weight;
}

// NEOTKO_REALCOLOR_TAG s214 (PBR item 2): `sharp_lin` is the already box-filtered color at this
// output pixel — seeding the accumulator with it (weight 1.0) means a neighborhood fully
// rejected by edge-stopping degrades gracefully back to the sharp color instead of going black.
vec3 compute_sss(vec2 center_uv, vec3 sharp_lin)
{
    vec4 center_meta = texture2D(u_peel_meta0, center_uv);
    if (center_meta.a < 0.5 || u_sss_strength <= 0.0)
        return sharp_lin;

    int tool = int(center_meta.r + 0.5);
    float radius_px = u_sss_radius_px * clamp(u_material_td[tool] / max(u_sss_reference_td, 1e-4), 0.15, 4.0);
    if (radius_px < 0.5)
        return sharp_lin;

    float center_z = center_meta.b;
    vec3 center_n = normalize(texture2D(u_peel_normal0, center_uv).rgb * 2.0 - 1.0);

    vec3 accum_color = sharp_lin;
    float accum_weight = 1.0;

    sss_sample(center_uv, center_z, center_n, vec2( 1.0,  0.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2(-1.0,  0.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.0,  1.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.0, -1.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.7,  0.7), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2(-0.7,  0.7), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.7, -0.7), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2(-0.7, -0.7), radius_px, accum_color, accum_weight);

    return mix(sharp_lin, accum_color / accum_weight, u_sss_strength);
}

// NEOTKO_REALCOLOR_TAG: 2x2 box-filter downsample from the REALCOLOR_SUPERSAMPLE-resolution
// peel/accum textures to the real canvas pixel — fixes thin ColorStitch/PathBlend geometry
// vanishing at single-sample resolution (root cause: the default framebuffer gets 4x MSAA via
// WX_GL_SAMPLE_BUFFERS/SAMPLES, these offscreen FBOs don't, so sub-pixel-wide toolpath lines
// can miss every sample point entirely). Coverage-weighted: taps with no geometry (meta.a==0)
// don't dilute the averaged color/depth of taps that DO have geometry — coverage itself becomes
// this pixel's alpha, blended against whatever render_shells()/background is already in the
// framebuffer (see glEnable(GL_BLEND) in render_toolpaths_realcolor's present-pass setup) so
// thin/faint features fade in smoothly instead of disappearing outright.
// NEOTKO_REALCOLOR_TAG s243 (F6): fraccion de los 8 vecinos con geometria. Un hueco rodeado de
// pieza esta DENTRO del contorno y se puede sellar; un pixel con pieza a un lado y aire al otro
// ES el contorno y su transparencia parcial es el antialiasing de la silueta. Mismo disco de 8
// taps que compute_ao()/compute_sss(), radio ~1 pixel de salida. Ver 140/realcolor_present.fs.
//
// ⚠️ AMBITO GLOBAL, obligatorio: GLSL no admite funciones anidadas. Este fichero coloca el
// comentario de s166 DENTRO de main(), al contrario que su gemelo 140/ — insertar por ese ancla
// mete la definicion dentro de main() y el shader entero deja de compilar (realcolor_present=0 en
// GLShadersManager::init, dialogo "Unable to load shaders" al arrancar).
float interior_tap(vec2 center_uv, vec2 dir)
{
    return step(0.5, texture2D(u_peel_meta0, center_uv + dir * u_texel_size * 2.0).a);
}

float compute_interior(vec2 center_uv)
{
    float n = 0.0;
    n += interior_tap(center_uv, vec2( 1.0,  0.0));
    n += interior_tap(center_uv, vec2(-1.0,  0.0));
    n += interior_tap(center_uv, vec2( 0.0,  1.0));
    n += interior_tap(center_uv, vec2( 0.0, -1.0));
    n += interior_tap(center_uv, vec2( 0.7,  0.7));
    n += interior_tap(center_uv, vec2(-0.7,  0.7));
    n += interior_tap(center_uv, vec2( 0.7, -0.7));
    n += interior_tap(center_uv, vec2(-0.7, -0.7));
    return n / 8.0;
}

void main()
{
    vec2 o1 = vec2(-0.5, -0.5) * u_texel_size;
    vec2 o2 = vec2( 0.5, -0.5) * u_texel_size;
    vec2 o3 = vec2(-0.5,  0.5) * u_texel_size;
    vec2 o4 = vec2( 0.5,  0.5) * u_texel_size;

    float a1 = step(0.5, texture2D(u_peel_meta0, uv + o1).a);
    float a2 = step(0.5, texture2D(u_peel_meta0, uv + o2).a);
    float a3 = step(0.5, texture2D(u_peel_meta0, uv + o3).a);
    float a4 = step(0.5, texture2D(u_peel_meta0, uv + o4).a);

    float coverage = a1 + a2 + a3 + a4;
    if (coverage < 0.5) // none of the 4 taps saw any toolpath geometry here
        discard;

    vec3 c1 = a1 * texture2D(u_accum_color, uv + o1).rgb;
    vec3 c2 = a2 * texture2D(u_accum_color, uv + o2).rgb;
    vec3 c3 = a3 * texture2D(u_accum_color, uv + o3).rgb;
    vec3 c4 = a4 * texture2D(u_accum_color, uv + o4).rgb;
    vec3 lin = (c1 + c2 + c3 + c4) / coverage;

    float d1 = mix(1.0, texture2D(u_peel_depth0, uv + o1).r, a1);
    float d2 = mix(1.0, texture2D(u_peel_depth0, uv + o2).r, a2);
    float d3 = mix(1.0, texture2D(u_peel_depth0, uv + o3).r, a3);
    float d4 = mix(1.0, texture2D(u_peel_depth0, uv + o4).r, a4);
    float min_depth = min(min(d1, d2), min(d3, d4));

    // NEOTKO_REALCOLOR_TAG s214 (PBR item 2): subsurface blur runs BEFORE AO — AO is a shading/
    // occlusion modulation that should apply to the already-diffused material color, not the
    // other way around. See compute_sss() above; realcolor_accum.fs is untouched, this never
    // feeds back into the Beer-Lambert composite either.
    lin = compute_sss(uv, lin);

    // NEOTKO_REALCOLOR_TAG s166 (item 3): AO is a post-process on the already box-filtered
    // color, computed once per output pixel (not once per box-filter tap) — see compute_ao()
    // above for the kernel itself; realcolor_accum.fs is untouched, this never feeds back
    // into the Beer-Lambert composite.
    float ao = compute_ao(uv);
    lin *= mix(1.0, ao, u_ao_strength);

    // NEOTKO_REALCOLOR_TAG s243 (F5 = A7): lo último antes del gamma, y deliberadamente fuera del
    // acumulador — es presentación, no material (razonamiento completo en 140/realcolor_present.fs).
    lin = REALCOLOR_MID_GRAY + (lin - REALCOLOR_MID_GRAY) * u_grade_contrast;
    lin = mix(vec3(dot(max(lin, vec3(0.0)), REALCOLOR_LUMA_709)), lin, u_grade_saturation);
    lin = max(lin, vec3(0.0));

    // NEOTKO_REALCOLOR_TAG s166: see 140/realcolor_present.fs for the full rationale — curved
    // coverage->alpha (pow 0.4) instead of linear, so partially-aliased-but-real geometry
    // (thin edges, sparse top-infill hatching) doesn't bleed 75% of the printbed (incl. its
    // printed text/logo) through at just 1/4 tap coverage.
    float out_alpha = pow(coverage / 4.0, 0.4);

    // NEOTKO_REALCOLOR_TAG s243 (F6): sella los huecos interiores, respeta el contorno. Solo sube
    // el ALFA sobre pixeles que YA tienen geometria — no inventa color ni profundidad, y por eso
    // no tapa agujeros reales del modelo. Ver 140/realcolor_present.fs.
    float interior = smoothstep(0.70, 0.95, compute_interior(uv));
    out_alpha = mix(out_alpha, 1.0, interior * u_fill_interior);
    gl_FragColor = vec4(linear_to_srgb(lin.r), linear_to_srgb(lin.g), linear_to_srgb(lin.b), out_alpha);
    gl_FragDepth = min_depth;
}
