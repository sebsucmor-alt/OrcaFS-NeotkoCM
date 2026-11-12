#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4): legacy/compat-profile counterpart of
// 140/shells_gbuffer.vs — see that file for the full rationale, identical math here, just
// attribute/varying instead of in/out.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
// NEOTKO_SHAREDVERTS_TAG 4-nov — malla compartida sin normales: la normal plana sale en el fragment.
uniform bool u_derived_normal;

attribute vec3 v_position;
attribute vec3 v_normal;

varying vec3 v_view_normal;
varying float v_eye_z;
varying vec3 v_view_pos;

void main()
{
    v_view_normal = u_derived_normal ? vec3(0.0, 0.0, 1.0) : normalize(view_normal_matrix * v_normal);
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    v_eye_z = -position.z;
    v_view_pos = position.xyz;
    gl_Position = projection_matrix * position;
}
