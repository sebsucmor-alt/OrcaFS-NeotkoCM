#version 140

// NEOTKO_REALCOLOR_TAG s166 (item 4.3): flattens shell geometry onto the bed (world z=0),
// projected along LIGHT_TOP_DIR (same constant as gouraud_light.vs/realcolor_peel.vs), so
// render_shells() can draw a cheap planar contact shadow with GL_STENCIL_TEST preventing
// double-darkening where multiple flattened silhouettes overlap. Least-confidence sub-item of
// this whole plan per the design doc — build, evaluate visually, discard without remorse if it
// doesn't read well at RealColor's typical (more top-down) camera angles.
//
// Deliberately does NOT use view_model_matrix (view*model pre-composed, as
// GLVolumeCollection::render() uploads it) because flattening has to happen in WORLD space
// (bed z=0), which requires the model and view transforms kept separate. `volume_world_matrix`
// (model-only) IS already set per-volume by render() (see 3DScene.cpp:968) for its own slope
// uniform — reused here. `u_view_matrix` (view-only, NOT part of render()'s uniform list) is
// set once by render_shells() right before this shader's draw call, see GCodeViewer.cpp.
uniform mat4 volume_world_matrix;
uniform mat4 u_view_matrix;
uniform mat4 projection_matrix;

in vec3 v_position;

const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);

void main()
{
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    // project along -LIGHT_TOP_DIR (from the light, through the vertex, down to the bed) until
    // z reaches 0 — clamped to >=0 so points already at/below the bed don't project backwards.
    float t = max(world_pos.z, 0.0) / LIGHT_TOP_DIR.z;
    vec3 flat_pos = world_pos.xyz - t * LIGHT_TOP_DIR;
    flat_pos.z = 0.001; // tiny epsilon above the bed, avoids z-fighting with the printbed mesh
    gl_Position = projection_matrix * u_view_matrix * vec4(flat_pos, 1.0);
}
