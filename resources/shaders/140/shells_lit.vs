#version 140

// NEOTKO_PHOTOMODE_TAG s242: what used to be the const/#define block below is now a set of
// uniforms, so Photo Mode can move the light. The C++ side (render_volumes_lit, GCodeViewer.cpp)
// sends the ORIGINAL values verbatim whenever Photo Mode is inactive, which is why nothing here
// needs a branch and why "Photo Mode off" is bit-identical to the previous build.
//
// The old values, kept as documentation of what the defaults must reproduce:
//   LIGHT_TOP_DIR        vec3(-0.4574957, 0.4574957, 0.7624929)  // (-0.6, 0.6, 1.0)/1.31
//   LIGHT_TOP_DIFFUSE    0.8   * 0.6 = 0.48
//   LIGHT_TOP_SPECULAR   0.125 * 0.6 = 0.075
//   LIGHT_TOP_SHININESS  20.0
//   LIGHT_FRONT_DIR      vec3(0.6985074, 0.1397015, 0.6985074)   // (1.0, 0.2, 1.0)/1.43
//   LIGHT_FRONT_DIFFUSE  0.3   * 0.6 = 0.18
//   AMBIENT_GROUND 0.18 / AMBIENT_SKY 0.32 / FRESNEL_POWER 5.0 / FRESNEL_STRENGTH 0.06
// (the ambient/fresnel pair came from RealColor's peel shader, s165/s166 — see realcolor_peel.vs
// for the original tuning rationale.)
//
// *** SPACE, and it is not the same for every consumer — read before touching ***
// The direct lighting below is CAMERA-space: dotting a light vector against a VIEW-space normal
// pins the light to the observer, the Slic3r/Orca convention this fork inherited. The shadow map
// (v_world_ndotl / v_shadow_coord, bottom of main()) instead needs a WORLD direction, because it
// is rendered from the light's own point of view. Until s242 both used the same constant, so the
// contradiction was invisible. Now there are two uniforms and the C++ side is responsible for
// keeping them consistent: with Photo Mode ON the light is authored in WORLD space and
// u_light_key_dir_view is derived from it per frame (view_normal_matrix * dir_world); with it OFF
// both simply carry the old constant, exactly as before.
uniform vec3  u_light_key_dir_view;
uniform vec3  u_light_key_dir_world;
uniform float u_light_key_diffuse;
uniform float u_light_key_specular;
uniform vec3  u_light_key_tint;

uniform vec3  u_light_fill_dir_view;
uniform float u_light_fill_diffuse;
uniform vec3  u_light_fill_tint;

// NEOTKO_PHOTOMODE_TAG s242: third light, which has never existed in this shader. Its diffuse is
// 0.0 unless a Photo Mode preset turns it on, so it contributes literally nothing by default.
uniform vec3  u_light_rim_dir_view;
uniform float u_light_rim_diffuse;
uniform vec3  u_light_rim_tint;

uniform float u_ambient_ground;
uniform float u_ambient_sky;
uniform float u_fresnel_power;
uniform float u_fresnel_strength;

// NEOTKO_SHADOW_TAG s229 (Fase 2): struct mirrored verbatim from gouraud.vs purely to reach
// `slope.volume_world_normal_matrix` — the WORLD-space inverse-transpose normal matrix, which
// GLVolumeCollection::render() already uploads per volume unconditionally (3DScene.cpp:983-985),
// so no engine change is needed to get it. Nothing here reads `actived`/`normal_z`; they are
// declared so the struct layout — and therefore the uniform NAME the engine sets — matches.
struct SlopeDetection
{
    bool  actived;
    float normal_z;
    mat3  volume_world_normal_matrix;
};

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
// NEOTKO_SHADOW_TAG s229 (Fase 2): also set unconditionally per volume by render() (3DScene.cpp:981).
uniform mat4 volume_world_matrix;
uniform SlopeDetection slope;

// NEOTKO_SHADOW_TAG s229 (Fase 2): world -> light clip transform (ortho fitted to the scene bbox,
// built in GCodeViewer::compute_shadow_light_matrix) and the normal-offset bias distance in world
// mm. Both set by render_volumes_lit(); u_shadow_normal_offset_mm is derived from the shadow map's
// texel footprint in world units, see SHADOW_MAP_NORMAL_OFFSET_TEXELS in GCodeViewer.cpp.
uniform mat4  u_light_proj_view;
uniform float u_shadow_normal_offset_mm;

in vec3 v_position;
in vec3 v_normal;

// NEOTKO_PHOTOMODE_TAG s242: was `out vec2 intensity` (x = ambient+diffuse, y = specular), the
// scalar pair inherited from gouraud_light.vs. Split into two vec3s because a per-light tint
// cannot survive a scalar — and kept SEPARATE from the ambient term, which the .fs must be able
// to exclude from shadowing (see v_ambient below).
//
// With every tint white and the rim off, v_diffuse == max(old intensity.x - v_ambient, 0.0) and
// v_specular == vec3(old intensity.y), so the .fs produces the same pixels it always did.
out vec3 v_diffuse;    // direct diffuse only, ambient NOT included

// NEOTKO_PHOTOMODE_TAG s242 (F3/F5): v_specular is GONE from the vertex stage.
//
// Specular used to be computed per-vertex, which was defensible at a fixed exponent of 20 (a very
// broad lobe). Per-slot materials make the exponent go up to ~120 for Glossy, and a highlight that
// tight interpolated across a large STL triangle breaks into visible facets. It is computed in the
// fragment shader now, from v_view_normal / v_view_pos — both of which were ALREADY varyings for
// the AO and SSCS, so this costs no extra interpolants.
//
// Precedent: s214 moved ambient+rim out of realcolor_peel.vs for exactly this reason.

// WORLD space position and normal, for the environment reflection (F5). The vertex stage already
// computed both for the shadow lookup below — they are simply forwarded now instead of being
// thrown away.
out vec3 v_world_pos;
out vec3 v_world_normal;
// NEOTKO_REALCOLOR_TAG s166: forwarded for the AO normal-agreement weight in shells_lit.fs —
// same pattern as realcolor_peel.vs's v_view_normal.
out vec3 v_view_normal;
// NEOTKO_SHADOW_TAG s229 (Fase 1): view-space position — shells_lit.fs marches from here toward
// the (camera-space) key light to find short-range contact occluders in the existing G-buffer.
out vec3 v_view_pos;
// NEOTKO_SHADOW_TAG s229 (Fase 2): the ambient term carried apart, so the .fs can shadow ONLY the
// direct terms. Shadowing the ambient too would turn every shadowed area into flat ink instead of
// penumbra. intensity.x keeps its original meaning (ambient + diffuse) untouched for exact visual
// parity — the .fs subtracts v_ambient back out to recover the diffuse-only part.
out float v_ambient;
// NEOTKO_SHADOW_TAG s229 (Fase 2): shadow-map lookup coordinate (already normal-offset biased in
// world space, see below) plus the WORLD-space N·L used for the slope-scaled depth bias. World,
// not view, because the shadow's key light is a fixed world direction — see the .fs and
// docs/FUTURE/SHADOWS_REALISTIC_CAST_RESEARCH.md §2 (config C3).
out vec4  v_shadow_coord;
out float v_world_ndotl;
out vec4 weave_model_pos;   // NEOTKO_PROFILE_TAG s233 — posición local para el weave

// NEOTKO_REALCOLOR_TAG s166 (item 4): drop-in replacement for gouraud_light.vs when rendering
// shells in render_shells() — same view_model_matrix/projection_matrix/view_normal_matrix/
// v_position/v_normal contract, so GLVolumeCollection::render() drives it identically. Adds
// the ambient/fresnel treatment already validated for RealColor's peel shader.
void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);
    v_view_normal = normal;

    float sky_mix = normal.y * 0.5 + 0.5; // 0 = ground, 1 = sky
    v_ambient = mix(u_ambient_ground, u_ambient_sky, sky_mix);

    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    v_view_pos = position.xyz;

    // Key. Diffuse only here — the specular moved to the fragment stage, see v_specular's note.
    float NdotL = max(dot(normal, u_light_key_dir_view), 0.0);
    v_diffuse   = u_light_key_tint * (NdotL * u_light_key_diffuse);

    // Fill — diffuse only, as it has always been.
    NdotL       = max(dot(normal, u_light_fill_dir_view), 0.0);
    v_diffuse  += u_light_fill_tint * (NdotL * u_light_fill_diffuse);

    // NEOTKO_PHOTOMODE_TAG s242: rim. u_light_rim_diffuse is 0.0 outside Photo Mode, so this pair
    // of instructions is a no-op there rather than a behaviour change.
    NdotL       = max(dot(normal, u_light_rim_dir_view), 0.0);
    v_diffuse  += u_light_rim_tint * (NdotL * u_light_rim_diffuse);

    // NEOTKO_SHADOW_TAG s229 (Fase 2): everything above is CAMERA-space lighting (LIGHT_TOP_DIR
    // dotted against a view-space normal = light pinned to the observer, the Slic3r/Orca
    // convention this fork inherited). The shadow instead needs a WORLD direction, because the
    // shadow map is rendered from the light's own point of view. Config C3 of the study: reuse
    // LIGHT_TOP_DIR as that world direction — which is exactly what shells_shadow.vs has always
    // done for the bed contact shadow, so the new object shadow and the old bed shadow finally
    // agree on where the light is.
    //
    // NEOTKO_PHOTOMODE_TAG s242: that world direction is now u_light_key_dir_world. Outside Photo
    // Mode it still carries the same LIGHT_TOP_DIR constant, so C3 holds unchanged; inside it, it
    // is the authored world aim and u_light_key_dir_view above is the one derived from it.
    weave_model_pos = vec4(v_position, 1.0);   // NEOTKO_PROFILE_TAG s233
    vec4 world_pos    = volume_world_matrix * vec4(v_position, 1.0);
    vec3 world_normal = normalize(slope.volume_world_normal_matrix * v_normal);
    v_world_ndotl     = max(dot(world_normal, u_light_key_dir_world), 0.0);
    // NEOTKO_PHOTOMODE_TAG s242 (F5): forwarded for the environment reflection. Free — both were
    // already being computed here for the shadow lookup.
    v_world_pos       = world_pos.xyz;
    v_world_normal    = world_normal;
    // Normal-offset bias: nudge the lookup off the surface along its own normal, in world mm.
    // Far more robust than a pure depth bias — kills shadow acne on grazing faces without the
    // visible "floating" (peter-panning) a large constant depth bias produces.
    v_shadow_coord = u_light_proj_view * vec4(world_pos.xyz + world_normal * u_shadow_normal_offset_mm, 1.0);

    gl_Position = projection_matrix * position;
}
