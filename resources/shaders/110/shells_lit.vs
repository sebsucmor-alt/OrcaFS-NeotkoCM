#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4): legacy/compat-profile counterpart of
// 140/shells_lit.vs — see that file for the full rationale, identical math here, just
// attribute/varying instead of in/out.

// NEOTKO_PHOTOMODE_TAG s242: lighting constants became uniforms so Photo Mode can move the light
// — see 140/shells_lit.vs for the full rationale, ESPECIALLY the note on view-space vs world-space
// (the direct lighting is camera-pinned, the shadow map is not). Identical math here.
//
// Old values the defaults must reproduce: LIGHT_TOP_DIR (-0.4574957, 0.4574957, 0.7624929),
// diffuse 0.48, specular 0.075, shininess 20; LIGHT_FRONT_DIR (0.6985074, 0.1397015, 0.6985074),
// diffuse 0.18; ambient 0.18/0.32; fresnel 5.0/0.06.
uniform vec3  u_light_key_dir_view;
uniform vec3  u_light_key_dir_world;
uniform float u_light_key_diffuse;
uniform float u_light_key_specular;
uniform vec3  u_light_key_tint;

uniform vec3  u_light_fill_dir_view;
uniform float u_light_fill_diffuse;
uniform vec3  u_light_fill_tint;

uniform vec3  u_light_rim_dir_view;
uniform float u_light_rim_diffuse;
uniform vec3  u_light_rim_tint;

uniform float u_ambient_ground;
uniform float u_ambient_sky;
uniform float u_fresnel_power;
uniform float u_fresnel_strength;

// NEOTKO_SHADOW_TAG s229 (Fase 2): see 140/shells_lit.vs for why this struct is declared.
struct SlopeDetection
{
    bool  actived;
    float normal_z;
    mat3  volume_world_normal_matrix;
};

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
uniform mat4 volume_world_matrix;
uniform SlopeDetection slope;

uniform mat4  u_light_proj_view;
uniform float u_shadow_normal_offset_mm;

attribute vec3 v_position;
attribute vec3 v_normal;

// NEOTKO_PHOTOMODE_TAG s242: was `varying vec2 intensity` — split into two vec3s so per-light
// tints survive. See 140/shells_lit.vs. With white tints and the rim off this is value-identical.
varying vec3 v_diffuse;    // direct diffuse only, ambient NOT included
// NEOTKO_PHOTOMODE_TAG s242 (F3): v_specular is GONE — the specular lobe moved to the fragment
// stage. See 140/shells_lit.vs: per-slot materials push the exponent to ~120 for Glossy, and a
// highlight that tight facets visibly when interpolated across a large STL triangle.
// NEOTKO_PHOTOMODE_TAG s242 (F5): world position/normal forwarded for the environment reflection.
// Free — both were already computed below for the shadow lookup.
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying vec3 v_view_normal;
varying vec3 v_view_pos;
varying float v_ambient;
varying vec4  v_shadow_coord;
varying float v_world_ndotl;
varying vec4 weave_model_pos;   // NEOTKO_PROFILE_TAG s233 — posición local para el weave

void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);
    v_view_normal = normal;

    float sky_mix = normal.y * 0.5 + 0.5;
    v_ambient = mix(u_ambient_ground, u_ambient_sky, sky_mix);

    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    v_view_pos = position.xyz;

    float NdotL = max(dot(normal, u_light_key_dir_view), 0.0);
    v_diffuse   = u_light_key_tint * (NdotL * u_light_key_diffuse);

    NdotL       = max(dot(normal, u_light_fill_dir_view), 0.0);
    v_diffuse  += u_light_fill_tint * (NdotL * u_light_fill_diffuse);

    NdotL       = max(dot(normal, u_light_rim_dir_view), 0.0);
    v_diffuse  += u_light_rim_tint * (NdotL * u_light_rim_diffuse);

    // NEOTKO_SHADOW_TAG s229 (Fase 2): world-space shadow lookup + normal-offset bias.
    weave_model_pos = vec4(v_position, 1.0);   // NEOTKO_PROFILE_TAG s233
    vec4 world_pos    = volume_world_matrix * vec4(v_position, 1.0);
    vec3 world_normal = normalize(slope.volume_world_normal_matrix * v_normal);
    // NEOTKO_PHOTOMODE_TAG s242: world-space aim of the key, see 140/shells_lit.vs.
    v_world_ndotl     = max(dot(world_normal, u_light_key_dir_world), 0.0);
    v_world_pos       = world_pos.xyz;
    v_world_normal    = world_normal;
    v_shadow_coord = u_light_proj_view * vec4(world_pos.xyz + world_normal * u_shadow_normal_offset_mm, 1.0);

    gl_Position = projection_matrix * position;
}
