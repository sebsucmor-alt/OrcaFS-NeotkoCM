#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4.3): legacy/compat-profile counterpart of
// 140/shells_shadow.vs — see that file for the full rationale, identical math here, just
// attribute instead of in.

uniform mat4 volume_world_matrix;
uniform mat4 u_view_matrix;
uniform mat4 projection_matrix;

attribute vec3 v_position;

// NEOTKO_PHOTOMODE_TAG s242: was `const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957,
// 0.7624929)`. This is the WORLD aim of the key — the same vector the shadow map is rendered
// along — so it has to track it. Leaving it frozen would make the projected bed shadow and the
// real shadow map disagree about where the light is the moment anyone drags it, which is the
// exact contradiction s229 went to the trouble of resolving (config C3).
uniform vec3 u_light_key_dir_world;

void main()
{
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    // NEOTKO_PHOTOMODE_TAG s242: guard the divide. With a fixed 40-deg light .z was always
    // ~0.76; a movable one can be aimed at the horizon, where .z -> 0 sends the projected
    // shadow to infinity and the geometry explodes across the screen. Clamping the denominator
    // caps the shadow's length instead — a very long shadow, which is what a grazing light
    // should look like anyway.
    float denom = max(u_light_key_dir_world.z, 0.05);
    float t = max(world_pos.z, 0.0) / denom;
    vec3 flat_pos = world_pos.xyz - t * u_light_key_dir_world;
    flat_pos.z = 0.001;
    gl_Position = projection_matrix * u_view_matrix * vec4(flat_pos, 1.0);
}
