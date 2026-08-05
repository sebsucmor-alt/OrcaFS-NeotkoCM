#version 110

// NEOTKO_PHOTOMODE_TAG s242 — the cyclorama ("lightbox") that replaces the print bed in Photo
// Mode: a large floor plane sweeping up into a curved back wall, so there is no visible seam
// behind the object. See docs/FUTURE/PHOTO_MODE_PLAN.md.
//
// Why this needs a shader of its own instead of reusing "flat" or "printbed":
// the cyclorama has to RECEIVE the s229 directional shadow map. That map is filled during pass 0
// of GCodeViewer::render_volumes_lit(), which runs earlier in the frame and only knows about
// GLVolumes — the cyclorama is not one. So it cannot be shaded by shells_lit; it samples the
// finished map itself, here, with the light matrix handed over by the C++ side.
//
// It is a receiver, NOT an occluder: it is deliberately absent from the bbox that fits the light's
// ortho frustum (that fit iterates `volumes`, GCodeViewer.cpp). Including a floor three times the
// size of the bed would blow the frustum up and collapse the map's effective resolution — the
// shadows would go blocky for no gain, since a flat floor casts nothing onto itself anyway.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;

// World -> light clip, straight from GCodeViewer::m_shadow_light_proj_view.
uniform mat4  u_light_proj_view;
uniform float u_shadow_normal_offset_mm;

attribute vec3 v_position;   // already in WORLD space, see _render_photo_stage()
attribute vec3 v_normal;

varying vec3  v_view_normal;
varying vec4  v_shadow_coord;
varying float v_world_ndotl;

uniform vec3 u_light_key_dir_world;

void main()
{
    v_view_normal = normalize(view_normal_matrix * v_normal);

    // The cyclorama mesh is generated directly in world coordinates, so there is no model matrix
    // to undo — position IS world position. (shells_lit has to go through volume_world_matrix
    // because its geometry is per-volume local.)
    vec3 world_pos    = v_position;
    vec3 world_normal = normalize(v_normal);

    v_world_ndotl  = max(dot(world_normal, u_light_key_dir_world), 0.0);
    // Same normal-offset bias trick as shells_lit.vs: nudge the lookup along the surface normal
    // in world mm. On a huge, almost perfectly flat floor at a grazing light angle this matters
    // more here than it does on the objects — a plain depth bias would either stripe the floor
    // with acne or lift the shadow visibly off the contact point.
    v_shadow_coord = u_light_proj_view * vec4(world_pos + world_normal * u_shadow_normal_offset_mm, 1.0);

    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
