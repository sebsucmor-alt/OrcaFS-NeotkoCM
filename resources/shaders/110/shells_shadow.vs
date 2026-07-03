#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4.3): legacy/compat-profile counterpart of
// 140/shells_shadow.vs — see that file for the full rationale, identical math here, just
// attribute instead of in.

uniform mat4 volume_world_matrix;
uniform mat4 u_view_matrix;
uniform mat4 projection_matrix;

attribute vec3 v_position;

const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);

void main()
{
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    float t = max(world_pos.z, 0.0) / LIGHT_TOP_DIR.z;
    vec3 flat_pos = world_pos.xyz - t * LIGHT_TOP_DIR;
    flat_pos.z = 0.001;
    gl_Position = projection_matrix * u_view_matrix * vec4(flat_pos, 1.0);
}
