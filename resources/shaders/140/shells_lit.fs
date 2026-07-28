#version 140

// NEOTKO_REALCOLOR_TAG s166 (item 4): drop-in replacement for gouraud_light.fs when rendering
// shells in render_shells() (Preview only, gated by NeoDebug::REALCOLOR — see
// GCodeViewer.cpp). Same uniform_color/emission_factor contract as gouraud_light.fs
// (GLVolumeCollection::render sets these by name regardless of which shader is bound, see
// 3DScene.cpp), plus screen-space AO sampled from the shells_gbuffer pre-pass. Uses
// gl_FragColor (not an explicit `out` var) to match gouraud_light.fs's own convention under
// this same #version — this codebase already runs its "140" shaders under a profile where
// that's valid (gouraud_light.fs does the same), so this isn't a new pattern.
//
// NEOTKO_SHADOW_TAG s229: adds two shadow terms on top of that AO, both documented in
// docs/FUTURE/SHADOWS_REALISTIC_CAST_RESEARCH.md:
//   Fase 1 — SSCS: screen-space contact shadows, marched through the SAME G-buffer the AO
//            already samples. No new GL object. Short range; catches "part resting on part".
//   Fase 2 — real directional shadow map, sampled with PCF. Long-range, lands on other objects
//            and self-shadows, which the old flattened-to-bed shells_shadow.vs never could.
// The two are combined with min() — whichever says "more shadowed" wins — because they cover
// different distance ranges: SSCS resolves the first millimetres, where a 2048² map is too coarse.

uniform vec4 uniform_color;
uniform float emission_factor;
// NEOTKO_REALCOLOR_TAG s166: shells_gbuffer.fs output — rgb = packed view-space normal
// ([-1,1]->[0,1]), a = linear eye-space depth (mm); a<=0.0 marks "no shell geometry here"
// (see shells_gbuffer.fs / ensure_shells_ao_fbo's clear color).
uniform sampler2D u_gbuffer;
uniform vec2 u_viewport;    // canvas size in px — u_gbuffer is native-resolution, no supersampling here
uniform float u_ao_radius;  // px
uniform float u_ao_strength;

// NEOTKO_SHADOW_TAG s229 (Fase 1): projection_matrix is the very same uniform render() already
// sets for the vertex stage — re-declaring it here just makes it visible to the fragment stage
// (uniforms are per-program, not per-stage), no extra C++ plumbing.
uniform mat4  projection_matrix;
uniform float u_sscs_length_mm;     // how far to march toward the light, in world mm
uniform float u_sscs_thickness_mm;  // an occluder thicker than this is treated as a wall, not a contact
uniform float u_sscs_strength;      // 0 disables the whole march

// NEOTKO_SHADOW_TAG s229 (Fase 2): directional shadow map, depth-only, sampled manually (compare
// mode NONE) rather than through sampler2DShadow — GLSL 110 would need ARB_shadow for that and
// this fork does not bet on extensions in the legacy Mac profile. Same discipline as RealColor:
// the 110 and 140 variants stay mathematically identical.
uniform sampler2D u_shadow_map;
uniform bool  u_shadow_enabled;   // false => map unavailable/disabled, skip the lookup entirely
uniform vec2  u_shadow_texel;     // 1.0 / shadow map resolution
uniform float u_shadow_bias_min;
uniform float u_shadow_bias_max;
uniform float u_shadow_strength;  // 0 = no shadow map contribution, 1 = full

in vec2 intensity;
in vec3 v_view_normal;
in vec3 v_view_pos;
in float v_ambient;
in vec4 v_shadow_coord;
in float v_world_ndotl;

// NEOTKO_SHADOW_TAG s229 (Fase 1): the key light in CAMERA space — same constant, same space, as
// the Phong in shells_lit.vs. The SSCS march is therefore fully coherent with the shading (config
// C1 of the study); only the Fase 2 shadow map uses this vector as a world direction instead.
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);

// NEOTKO_REALCOLOR_TAG s166: same depth-diff + normal-agreement AO as
// realcolor_present.fs's compute_ao() (see that file for the full rationale / Kajalin 2007
// citation) — separate copy because this samples a different G-buffer texture (shells_gbuffer,
// single opaque pass) at native resolution, not RealColor's supersampled peel textures.
const float SHELLS_AO_MAX_DELTA_MM = 1.5;

float ao_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir)
{
    vec2 s_uv = center_uv + dir * (u_ao_radius / u_viewport);
    vec4 s = texture(u_gbuffer, s_uv);
    if (s.a <= 0.0)
        return 0.0; // background — doesn't occlude
    float delta = center_z - s.a; // positive: neighbor is CLOSER to camera than center
    if (delta <= 0.0)
        return 0.0; // neighbor is farther away or coplanar — not an occluder of this pixel
    float range_falloff = clamp(1.0 - delta / SHELLS_AO_MAX_DELTA_MM, 0.0, 1.0);
    vec3 s_n = normalize(s.rgb * 2.0 - 1.0);
    float normal_agreement = max(dot(center_n, s_n), 0.0);
    return range_falloff * normal_agreement;
}

float compute_ao(vec2 uv)
{
    vec4 center = texture(u_gbuffer, uv);
    if (center.a <= 0.0)
        return 1.0; // shouldn't happen (this fragment IS shell geometry) but stay safe
    vec3 center_n = normalize(center.rgb * 2.0 - 1.0);
    float center_z = center.a;

    float occlusion = 0.0;
    occlusion += ao_sample(uv, center_z, center_n, vec2( 1.0,  0.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2(-1.0,  0.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.0,  1.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.0, -1.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.7,  0.7));
    occlusion += ao_sample(uv, center_z, center_n, vec2(-0.7,  0.7));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.7, -0.7));
    occlusion += ao_sample(uv, center_z, center_n, vec2(-0.7, -0.7));
    occlusion /= 8.0;
    return 1.0 - clamp(occlusion, 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------
// NEOTKO_SHADOW_TAG s229 (Fase 1) — screen-space contact shadows.
//
// Walk from this fragment toward the light in small steps. At each step, project the ray point
// back to screen and ask the G-buffer how far the nearest surface is at that pixel. If that
// surface sits BETWEEN the ray point and the camera, something is standing between us and the
// light, so we're in shadow. This is the same trick the AO above uses, aimed along the light
// instead of in a ring.
//
// Structural limits (intentional, documented rather than worked around): the occluder has to be
// on screen and not hidden behind other geometry, and the range is short by construction. That's
// what the Fase 2 shadow map is for.
// ---------------------------------------------------------------------------------------------
#define SSCS_STEPS 12
const float SSCS_SELF_BIAS_MM = 0.08; // ignore hits this close — that's our own surface

float sscs_occlusion()
{
    if (u_sscs_strength <= 0.0 || u_sscs_length_mm <= 0.0)
        return 0.0;

    vec3 rd = normalize(LIGHT_TOP_DIR);
    float step_mm = u_sscs_length_mm / float(SSCS_STEPS);
    // Per-pixel jitter of the first step: a fixed stride bands visibly on smooth surfaces.
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float t = step_mm * (0.5 + 0.5 * jitter);

    for (int i = 0; i < SSCS_STEPS; ++i) {
        vec3 p = v_view_pos + rd * t;
        vec4 clip = projection_matrix * vec4(p, 1.0);
        if (clip.w <= 0.0)
            break; // behind the eye, nothing meaningful left to sample
        vec2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
            break; // marched off screen — no information out there
        float scene_z = texture(u_gbuffer, suv).a;
        if (scene_z > 0.0) {
            // eye_z convention matches shells_gbuffer.vs: positive, growing away from the camera.
            float delta = (-p.z) - scene_z;
            if (delta > SSCS_SELF_BIAS_MM && delta < u_sscs_thickness_mm)
                // Closer hits darken more — a contact shadow should be tightest at the contact.
                return 1.0 - t / u_sscs_length_mm;
        }
        t += step_mm;
    }
    return 0.0;
}

// ---------------------------------------------------------------------------------------------
// NEOTKO_SHADOW_TAG s229 (Fase 2) — directional shadow map lookup, 3x3 PCF.
//
// v_shadow_coord already carries the normal-offset bias applied in world space by shells_lit.vs.
// Here we only add the slope-scaled depth bias and average 9 taps to soften the edge. Taps are
// unrolled by hand to match this file's existing AO style and to keep the 110 variant free of any
// loop-around-a-sampler construct.
// ---------------------------------------------------------------------------------------------
float shadow_tap(vec2 uv, float r)
{
    // 1.0 = lit, 0.0 = occluded. Anything outside the map reads as lit (never invent shadow).
    return (r > texture(u_shadow_map, uv).r) ? 0.0 : 1.0;
}

float shadow_map_visibility()
{
    if (v_shadow_coord.w <= 0.0)
        return 1.0;
    vec3 proj = v_shadow_coord.xyz / v_shadow_coord.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0; // outside the light frustum's footprint — treat as fully lit
    float ref = proj.z * 0.5 + 0.5;
    if (ref <= 0.0 || ref >= 1.0)
        return 1.0; // nearer than the light's near plane, or past its far plane

    float bias = max(u_shadow_bias_min, u_shadow_bias_max * (1.0 - v_world_ndotl));
    float r = ref - bias;

    float s = 0.0;
    s += shadow_tap(uv + vec2(-1.0, -1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 0.0, -1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 1.0, -1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2(-1.0,  0.0) * u_shadow_texel, r);
    s += shadow_tap(uv,                                     r);
    s += shadow_tap(uv + vec2( 1.0,  0.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2(-1.0,  1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 0.0,  1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 1.0,  1.0) * u_shadow_texel, r);
    return s / 9.0;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / u_viewport;
    float ao = compute_ao(uv);

    // NEOTKO_SHADOW_TAG s229: visibility of the DIRECT light. 1 = fully lit, 0 = fully shadowed.
    float vis = 1.0;
    if (u_shadow_enabled)
        vis = mix(1.0, shadow_map_visibility(), u_shadow_strength);
    vis = min(vis, 1.0 - u_sscs_strength * sscs_occlusion());

    // NEOTKO_SHADOW_TAG s229: ambient is never shadowed (see v_ambient in shells_lit.vs) — only
    // the diffuse and specular/fresnel terms are. With vis == 1.0 this is algebraically identical
    // to the pre-s229 line it replaces:
    //     vec3 lit = vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor);
    // so a disabled/failed shadow path looks exactly like it did before.
    vec3 lit = uniform_color.rgb * (v_ambient + emission_factor)
             + (uniform_color.rgb * max(intensity.x - v_ambient, 0.0) + vec3(intensity.y)) * vis;
    lit *= mix(1.0, ao, u_ao_strength);
    gl_FragColor = vec4(lit, uniform_color.a);
}
