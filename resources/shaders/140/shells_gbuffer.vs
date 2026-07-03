#version 140

// NEOTKO_REALCOLOR_TAG s166 (item 4): normal+depth pre-pass over m_shells.volumes, feeding
// the screen-space AO sampled in shells_lit.fs. Same attribute/uniform contract as
// gouraud_light.vs (view_model_matrix/projection_matrix/view_normal_matrix/v_position/
// v_normal) so GLVolumeCollection::render() — which sets these uniforms unconditionally by
// name per volume regardless of which shader is bound, see 3DScene.cpp:986-990 — drives this
// shader exactly like it already drives gouraud_light. Gated behind NeoDebug::REALCOLOR, see
// render_shells() in GCodeViewer.cpp.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;

in vec3 v_position;
in vec3 v_normal;

out vec3 v_view_normal;
out float v_eye_z;

void main()
{
    v_view_normal = normalize(view_normal_matrix * v_normal);
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    v_eye_z = -position.z; // right-handed eye space, camera looks down -Z — matches realcolor_peel.vs
    gl_Position = projection_matrix * position;
}
