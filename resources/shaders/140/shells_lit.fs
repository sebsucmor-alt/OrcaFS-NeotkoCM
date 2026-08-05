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
uniform float u_ao_bias_mm; // NEOTKO_SMOOTHNORMALS_TAG s229

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
uniform float u_shadow_strength;

// NEOTKO_SMOOTHNORMALS_TAG s229 — isolate one screen-space term to see what it contributes alone.
// With LibreMode on this shader, not gouraud, is what draws the user's objects, so this is where
// camera-dependent noise on flat faces has to be hunted.
//   0 = off, 1 = AO only, 2 = contact shadows only, 3 = shadow map only, 4 = view normal as RGB
uniform int u_shading_isolate;
  // 0 = no shadow map contribution, 1 = full

// NEOTKO_PHOTOMODE_TAG s242: `intensity` (vec2, x = ambient+diffuse, y = specular) split into two
// vec3s so a per-light tint survives — see shells_lit.vs. v_diffuse excludes ambient, which is why
// main() below no longer subtracts v_ambient back out of it.
in vec3 v_diffuse;
in vec3 v_view_normal;
// NEOTKO_PHOTOMODE_TAG s242 (F5): world-space surface, for the environment reflection.
in vec3 v_world_pos;
in vec3 v_world_normal;
in vec3 v_view_pos;
in float v_ambient;
in vec4 v_shadow_coord;
in float v_world_ndotl;
in vec4 weave_model_pos;   // NEOTKO_PROFILE_TAG s233

// NEOTKO_SHADOW_TAG s229 (Fase 1): the key light in CAMERA space — same value, same space, as
// the Phong in shells_lit.vs. The SSCS march is therefore fully coherent with the shading (config
// C1 of the study); only the Fase 2 shadow map uses this vector as a world direction instead.
//
// NEOTKO_PHOTOMODE_TAG s242: promoted from `const vec3 LIGHT_TOP_DIR = vec3(-0.4574957,
// 0.4574957, 0.7624929)` to a uniform fed from the same place the vertex stage gets its copy.
// Coherence with the shading is the whole point of C1, so this vector MUST keep tracking
// u_light_key_dir_view — leaving it frozen while the light moved would aim the contact shadows
// one way and everything else another.
uniform vec3 u_light_key_dir_view;

// NEOTKO_PHOTOMODE_TAG s242 (F3) — the specular lobe, moved here from the vertex stage. Declared
// in BOTH stages of the program deliberately: they are the same uniforms, set once, and the vertex
// stage still needs the diffuse/tint pair.
uniform float u_light_key_specular;
uniform vec3  u_light_key_tint;
uniform float u_fresnel_power;
uniform float u_fresnel_strength;

// Per-VOLUME material, set in GLVolumeCollection::render() (3DScene.cpp) — the only place in the
// frame that knows which filament slot is being drawn.
//
// The defaults below are what an unset uniform would be, but they are never relied on: the C++
// side sends metallic=0 / shininess=20 / spec_scale=1 whenever Photo Mode is off, which reproduces
// the pre-s242 lobe exactly.
uniform float u_mat_metallic;    // 0 = dielectric (white highlight), 1 = metal (tinted, no diffuse)
uniform float u_mat_shininess;   // Phong exponent; 20.0 == the original constant
uniform float u_mat_spec_scale;  // multiplies u_light_key_specular

// NEOTKO_PHOTOMODE_TAG s242 (F5) — procedural environment. Two equirectangular textures baked on
// the CPU from the Photo Mode lights: `mirror` is sharp (reflections), `irradiance` is tiny and
// pre-averaged (ambient). u_env_enabled is false outside Photo Mode, so none of this runs there.
uniform bool      u_env_enabled;
uniform sampler2D u_env_mirror;
uniform sampler2D u_env_irradiance;
uniform float     u_env_intensity;
uniform float     u_env_rotation;    // radians, around world Z
uniform vec3      u_camera_pos_world;

const float PHOTO_PI = 3.14159265358979;

// Equirectangular lookup. NOTE the up axis is Z, not Y: this project's world has the bed on XY
// with Z up, unlike realcolor_peel.fs's copy of this function, which is Y-up because RealColor
// feeds it vectors in its own space. Getting this wrong does not error — it silently lies the
// environment on its side — so the two are kept separate rather than shared.
vec2 photo_env_uv(vec3 dir)
{
    dir = normalize(dir);
    float ang = atan(dir.y, dir.x) + u_env_rotation;
    float u = ang / (2.0 * PHOTO_PI) + 0.5;
    float v = 0.5 - asin(clamp(dir.z, -1.0, 1.0)) / PHOTO_PI;   // v=0 up, v=1 down
    return vec2(u, v);
}

// NEOTKO_REALCOLOR_TAG s166: same depth-diff + normal-agreement AO as
// realcolor_present.fs's compute_ao() (see that file for the full rationale / Kajalin 2007
// citation) — separate copy because this samples a different G-buffer texture (shells_gbuffer,
// single opaque pass) at native resolution, not RealColor's supersampled peel textures.
const float SHELLS_AO_MAX_DELTA_MM = 1.5;

// NEOTKO_SMOOTHNORMALS_TAG s229 — tangent-plane rejection.
//
// The intent of the old code is right there in its own comment: "farther away or coplanar — not an
// occluder". But `delta = center_z - s.a` only detects coplanarity when the surface faces the
// camera. Tilt a large flat face and every neighbour on its far side is legitimately deeper while
// lying in the very same plane, so the face permanently self-occludes; the amount is modulated by
// the per-triangle normals in the G-buffer, which is why the streaks traced the tessellation and
// crawled with the camera - and why they vanished in exact top view, where a flat face has
// constant depth and every delta is 0.
//
// So: compare against the depth the neighbour would have IF it were coplanar with the centre,
// rather than against the centre's own depth. The screen-space derivatives of the view depth give
// that tangent plane exactly and for free, with no matrix inverse: over a plane, view depth is
// linear in screen space, so the expected depth at a pixel offset is centre + gradient . offset.
// A perfectly flat surface now self-occludes zero at any angle, while a real crease or a
// neighbouring part still reads as an occluder.
//
// u_ao_bias_mm absorbs what is left: G-buffer quantization and the residual curvature of the
// linear approximation. It is slope-scaled (same idea as a shadow map's slope-scaled bias) because
// at grazing angles a one-pixel error in position is worth many millimetres of depth.
float ao_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir, vec2 depth_grad)
{
    vec2 offset_px = dir * u_ao_radius;
    vec2 s_uv = center_uv + offset_px / u_viewport;
    vec4 s = texture(u_gbuffer, s_uv);
    if (s.a <= 0.0)
        return 0.0; // background — doesn't occlude

    // Depth this sample would have if it sat on the centre's tangent plane.
    float expected_z = center_z + dot(depth_grad, offset_px);
    float bias = max(u_ao_bias_mm, 0.25 * length(depth_grad) * u_ao_radius);
    float delta = expected_z - s.a; // positive: neighbor rises ABOVE the centre's own plane
    if (delta <= bias)
        return 0.0; // coplanar (at any viewing angle now) or farther away — not an occluder
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

    // NEOTKO_SMOOTHNORMALS_TAG s229: screen-space gradient of this fragment's own view depth, in
    // mm per pixel. v_view_pos is interpolated per fragment, so these derivatives describe the real
    // tangent plane of the triangle being shaded — the G-buffer is never asked for it.
    float vz = -v_view_pos.z;
    vec2 depth_grad = vec2(dFdx(vz), dFdy(vz));

    float occlusion = 0.0;
    occlusion += ao_sample(uv, center_z, center_n, vec2( 1.0,  0.0), depth_grad);
    occlusion += ao_sample(uv, center_z, center_n, vec2(-1.0,  0.0), depth_grad);
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.0,  1.0), depth_grad);
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.0, -1.0), depth_grad);
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.7,  0.7), depth_grad);
    occlusion += ao_sample(uv, center_z, center_n, vec2(-0.7,  0.7), depth_grad);
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.7, -0.7), depth_grad);
    occlusion += ao_sample(uv, center_z, center_n, vec2(-0.7, -0.7), depth_grad);
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

    vec3 rd = normalize(u_light_key_dir_view);
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


// NEOTKO_PROFILE_TAG s233 — weave/degradado de la pintura ColorMix en la vista 3D
// NORMAL (fuera del gizmo), aquí para el camino de LibreMode. Copia literal del bloque
// que mm_gouraud.fs usa en el preview del painter, para que dentro y fuera se vea lo
// mismo. Inerte mientras u_weave_on sea false — GLVolume::simple_render lo apaga antes
// y después de cada trozo, y nadie más lo enciende.
uniform bool  u_weave_on;
uniform bool  u_weave_tile;
uniform int   u_weave_n;
uniform float u_weave_angle;
uniform float u_weave_pitch;
uniform float u_weave_p0;
uniform vec3  u_weave_cols[64];

vec3 weave_color(vec3 base)
{
    if (!u_weave_on || u_weave_n <= 0)
        return base;
    float s = sin(u_weave_angle);
    float c = cos(u_weave_angle);
    float proj = -weave_model_pos.x * s + weave_model_pos.y * c;
    float line = floor((proj - u_weave_p0) / max(u_weave_pitch, 0.0001));
    int   idx;
    if (u_weave_tile) {
        float fn = float(u_weave_n);
        idx = int(line - fn * floor(line / fn));
    } else {
        idx = int(line);
        if (idx < 0)             idx = 0;
        if (idx > u_weave_n - 1) idx = u_weave_n - 1;
    }
    for (int i = 0; i < 64; ++i)
        if (i == idx) return u_weave_cols[i];
    return base;
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
    // NEOTKO_PROFILE_TAG s233 — el color base pasa por el weave (idéntico a uniform_color
    // .rgb mientras u_weave_on sea false, o sea siempre salvo en pintura ColorMix).
    // NEOTKO_PHOTOMODE_TAG s242 — the `max(intensity.x - v_ambient, 0.0)` that used to sit here
    // was unpacking the direct-diffuse part out of the combined scalar; v_diffuse now carries it
    // on its own, already tinted, so the term is read straight rather than reconstructed.
    vec3 weave_base = weave_color(uniform_color.rgb);

    // --- material split (F3) -----------------------------------------------------------------
    // A metal has no diffuse and tints its highlight with its own colour; a dielectric has full
    // diffuse and a white highlight. One mix() each covers both ends and everything between,
    // which is what the Silk preset actually is.
    // With u_mat_metallic = 0 this is diffuse = base and specular = white — the original.
    float metal     = clamp(u_mat_metallic, 0.0, 1.0);
    vec3  diff_col  = weave_base * (1.0 - metal);
    vec3  spec_col  = mix(vec3(1.0), weave_base, metal);

    // --- specular, per fragment (F3) ---------------------------------------------------------
    vec3  N       = normalize(v_view_normal);
    vec3  view_d  = -normalize(v_view_pos);
    vec3  R_view  = reflect(-u_light_key_dir_view, N);
    float spec    = u_light_key_specular * u_mat_spec_scale
                  * pow(max(dot(view_d, R_view), 0.0), max(u_mat_shininess, 1.0));
    // Fresnel sheen stays white and stays outside the material tint: it is the sky catching the
    // silhouette, not the surface's own colour.
    float fres    = pow(1.0 - max(dot(view_d, N), 0.0), u_fresnel_power);
    vec3  specular = spec_col * (u_light_key_tint * spec) + vec3(u_fresnel_strength * fres);

    // --- environment / reflected light (F5) ---------------------------------------------------
    //
    // *** The metal-is-black bug, s242 — read before touching the fallback below ***
    // Setting metallic=1 zeroes diff_col, which kills BOTH the ambient and the diffuse terms. That
    // is correct: a metal has no diffuse, it only reflects. But the thing that is supposed to
    // replace them — the reflection — used to live entirely inside `if (u_env_enabled)`, and the
    // environment is off by default. So a metal had literally nothing left but a pinpoint specular
    // lobe and rendered BLACK, at every light angle and intensity, which is exactly what it looks
    // like: not a lighting problem, an energy problem.
    //
    // A material must never depend on an unrelated toggle to be visible at all. So the reflection
    // term always exists; only what it reflects changes.
    vec3  ambient_rgb = vec3(v_ambient);
    vec3  mirror_rgb;
    float refl_w;

    if (u_env_enabled) {
        vec3 Nw = normalize(v_world_normal);
        vec3 Vw = normalize(u_camera_pos_world - v_world_pos);
        vec3 Rw = reflect(-Vw, Nw);
        // Ambient comes from the pre-averaged probe, blended against the flat hemisphere so the
        // slider is a dial and not a switch.
        ambient_rgb = mix(ambient_rgb, texture(u_env_irradiance, photo_env_uv(Nw)).rgb, u_env_intensity);
        mirror_rgb  = texture(u_env_mirror, photo_env_uv(Rw)).rgb * u_env_intensity;
        // Fresnel-weighted for dielectrics (a plastic mirrors the room only at grazing angles),
        // full strength for metals (they mirror it everywhere).
        refl_w      = mix(fres, 1.0, metal);
    }
    else {
        // No probe to sample, so stand in for it with the light the surface actually receives:
        // the direct lights plus the hemisphere ambient. That is what the probe would have been
        // baked FROM anyway (see photo_env_sample in GCodeViewer.cpp), so a metal lit this way
        // still tracks the key light's direction, colour and intensity — it just reflects a smooth
        // room instead of a room with visible softboxes in it.
        mirror_rgb = v_diffuse + vec3(v_ambient);
        // ⚠️ `metal`, NOT mix(fres, 1.0, metal). Two reasons, and the second one cost a bug:
        //   1. it must be exactly 0 for a dielectric — before F5 this whole term did not exist,
        //      and without that a plastic would silently gain a fresnel rim;
        //   2. the metal factor must appear ONCE. The first version of this fix also scaled
        //      mirror_rgb by `metal`, so Silk (0.45) got 0.45*0.45 = 0.20 of its reflection back
        //      and came out ~22% darker than the plastic it was meant to look richer than.
        refl_w = metal;
    }

    // Energy adds up rather than going missing: diff_col carries (1 - metal) and refl_w carries
    // metal (plus the small fresnel share), so a metal is about as bright as the plastic it
    // replaced — all of it routed through the tinted specular path instead of the diffuse one.
    vec3 env_refl = spec_col * mirror_rgb * refl_w;

    vec3 lit = diff_col * (ambient_rgb + emission_factor)
             + (diff_col * v_diffuse + specular) * vis
             + env_refl;
    lit *= mix(1.0, ao, u_ao_strength);
    gl_FragColor = vec4(lit, uniform_color.a);

    // NEOTKO_SMOOTHNORMALS_TAG s229: diagnostic isolation, last word on the colour.
    if (u_shading_isolate == 1)
        gl_FragColor = vec4(vec3(ao), 1.0);
    else if (u_shading_isolate == 2)
        gl_FragColor = vec4(vec3(1.0 - sscs_occlusion()), 1.0);
    else if (u_shading_isolate == 3)
        gl_FragColor = vec4(vec3(u_shadow_enabled ? shadow_map_visibility() : 1.0), 1.0);
    else if (u_shading_isolate == 4)
        gl_FragColor = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
}
