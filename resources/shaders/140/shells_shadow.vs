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

// NEOTKO_PHOTOMODE_TAG s242: was a constant copy of LIGHT_TOP_DIR. It is the WORLD aim of the key
// light — the same one the s229 shadow map is rendered along — so it must track it, or the
// projected bed shadow and the real cast shadow point in different directions as soon as the user
// drags the light. Fed from photo_key_dir_world() (GCodeViewer.cpp), which returns the original
// constant whenever Photo Mode is off.
uniform vec3 u_light_key_dir_world;

void main()
{
    vec4 world_pos = volume_world_matrix * vec4(v_position, 1.0);
    // project along -u_light_key_dir_world (from the light, through the vertex, down to the bed)
    // until z reaches 0 — clamped to >=0 so points already at/below the bed don't project
    // backwards.
    // NEOTKO_PHOTOMODE_TAG s242: the denominator is now clamped. With a fixed 40-deg light .z was
    // permanently ~0.76; a movable light can point at the horizon, where .z -> 0 sends this
    // projection to infinity. The clamp caps the shadow's length rather than letting the geometry
    // blow up across the screen.
    float denom = max(u_light_key_dir_world.z, 0.05);
    float t = max(world_pos.z, 0.0) / denom;
    vec3 flat_pos = world_pos.xyz - t * u_light_key_dir_world;
    flat_pos.z = 0.001; // tiny epsilon above the bed, avoids z-fighting with the printbed mesh
    gl_Position = projection_matrix * u_view_matrix * vec4(flat_pos, 1.0);
}
