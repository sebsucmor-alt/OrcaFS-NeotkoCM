#version 140

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

// NEOTKO_REALCOLOR_TAG s166 (item 4): same two-tone ambient + fresnel treatment already
// shipped for RealColor's peel shader (item 2, s165) — reused here instead of inventing new
// constants, see realcolor_peel.vs for the original rationale/tuning notes. Fixed #defines
// (not live-tunable uniforms like RealColor's debug panel) since this shader is gated
// separately and isn't part of the RealColor Tuning panel — see render_shells() in
// GCodeViewer.cpp.
#define AMBIENT_GROUND    0.18
#define AMBIENT_SKY       0.32
#define FRESNEL_POWER     5.0
#define FRESNEL_STRENGTH  0.06

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

// x = tainted, y = specular; matches gouraud_light.vs
out vec2 intensity;
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

// NEOTKO_REALCOLOR_TAG s166 (item 4): drop-in replacement for gouraud_light.vs when rendering
// shells in render_shells() — same view_model_matrix/projection_matrix/view_normal_matrix/
// v_position/v_normal contract, so GLVolumeCollection::render() drives it identically. Adds
// the ambient/fresnel treatment already validated for RealColor's peel shader.
void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);
    v_view_normal = normal;

    float NdotL = max(dot(normal, LIGHT_TOP_DIR), 0.0);

    float sky_mix = normal.y * 0.5 + 0.5; // 0 = ground, 1 = sky
    v_ambient = mix(AMBIENT_GROUND, AMBIENT_SKY, sky_mix);
    intensity.x = v_ambient + NdotL * LIGHT_TOP_DIFFUSE;
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    v_view_pos = position.xyz;
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, normal)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    float fres = pow(1.0 - max(dot(normalize(-position.xyz), normal), 0.0), FRESNEL_POWER);
    intensity.y += FRESNEL_STRENGTH * fres;

    // NEOTKO_SHADOW_TAG s229 (Fase 2): everything above is CAMERA-space lighting (LIGHT_TOP_DIR
    // dotted against a view-space normal = light pinned to the observer, the Slic3r/Orca
    // convention this fork inherited). The shadow instead needs a WORLD direction, because the
    // shadow map is rendered from the light's own point of view. Config C3 of the study: reuse
    // LIGHT_TOP_DIR as that world direction — which is exactly what shells_shadow.vs has always
    // done for the bed contact shadow, so the new object shadow and the old bed shadow finally
    // agree on where the light is.
    vec4 world_pos    = volume_world_matrix * vec4(v_position, 1.0);
    vec3 world_normal = normalize(slope.volume_world_normal_matrix * v_normal);
    v_world_ndotl     = max(dot(world_normal, LIGHT_TOP_DIR), 0.0);
    // Normal-offset bias: nudge the lookup off the surface along its own normal, in world mm.
    // Far more robust than a pure depth bias — kills shadow acne on grazing faces without the
    // visible "floating" (peter-panning) a large constant depth bias produces.
    v_shadow_coord = u_light_proj_view * vec4(world_pos.xyz + world_normal * u_shadow_normal_offset_mm, 1.0);

    gl_Position = projection_matrix * position;
}
