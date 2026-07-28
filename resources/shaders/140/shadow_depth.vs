#version 140

// NEOTKO_SHADOW_TAG s229 (Fase 2): depth-only pass that renders the scene FROM THE LIGHT, filling
// the shadow map that shells_lit.fs then samples. See
// docs/FUTURE/SHADOWS_REALISTIC_CAST_RESEARCH.md §5-A.
//
// Deliberately declares nothing but v_position: GLModel::render() looks each attribute up by name
// and silently skips any that resolve to -1 (GLModel.cpp:628-647), so a normal-less shader costs
// nothing and there is no VAO mismatch to worry about.
//
// The trick that makes this free of engine changes: GLVolumeCollection::render() takes the view and
// projection matrices as PARAMETERS (3DScene.hpp:476), so render_shadow_map() simply hands it the
// light's view + ortho matrices instead of the camera's, and this shader's `view_model_matrix`
// arrives as light_view * model without a single line changed in 3DScene.cpp. Also verified there:
// volumes_to_render() does NO camera-frustum culling (3DScene.cpp:846-880), so casters that are
// off-screen from the camera still make it into the map — which is exactly what a shadow pass needs.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

in vec3 v_position;

void main()
{
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
