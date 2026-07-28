#version 110

// NEOTKO_SHADOW_TAG s229 (Fase 2): legacy/compat-profile counterpart of 140/shadow_depth.vs —
// see that file for the full rationale, identical math here, just attribute instead of in.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

attribute vec3 v_position;

void main()
{
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
