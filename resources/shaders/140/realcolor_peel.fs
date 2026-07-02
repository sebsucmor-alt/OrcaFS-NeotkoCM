#version 140
#extension GL_ARB_explicit_attrib_location : require

// NEOTKO_REALCOLOR_TAG: one pass of front-to-back depth peeling over the SAME toolpath
// geometry render_toolpaths() already draws (see render_toolpaths_realcolor() in
// GCodeViewer.cpp). Pass i keeps only fragments strictly farther than pass i-1's depth,
// so each pass extracts the next visible layer. Writes color (attachment 0) and
// (tool_id, thickness, -, written-flag) meta (attachment 1) for the accumulate pass.

uniform sampler2D u_prev_meta;  // previous pass's meta (tool_id, thickness, eye_z, written) — NOT a depth texture
uniform bool  u_has_prev_depth; // false for peel pass 0 (nothing to peel behind yet)
uniform int   u_tool_id;        // physical tool (0-3) for this draw, see RenderPath/Path::extruder_id
uniform float u_thickness;      // Path::height for this draw, mm
uniform vec3  u_material_rgb[4];
uniform vec2  u_viewport;

// x = tainted, y = specular; matches gouraud_light.vs
in vec2 intensity;
in float v_eye_z; // linear eye-space depth, matches realcolor_peel.vs

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_meta;

void main()
{
    if (u_has_prev_depth) {
        vec2 uv = gl_FragCoord.xy / u_viewport;
        float prevEyeZ = texture(u_prev_meta, uv).b;
        // NEOTKO_REALCOLOR_TAG: slope-scaled bias in LINEAR eye-space depth (mm), not NDC
        // gl_FragCoord.z — NDC depth is nonlinear w.r.t. real distance, so a bias there breaks
        // differently at every zoom level (confirmed: the noisy band moved with zoom). Linear
        // depth is zoom-invariant; fwidth(v_eye_z) still self-scales for grazing-angle surfaces
        // (same purpose as before, just in a well-behaved space). Floor is a real mm distance,
        // small vs. any real layer height (>=0.05mm typical) so distinct layers stay separable.
        float bias = max(fwidth(v_eye_z) * 2.0, 2e-3);
        if (v_eye_z <= prevEyeZ + bias)
            discard; // already surfaced in an earlier peel pass
    }

    vec3 base = u_material_rgb[u_tool_id];
    vec3 lit = vec3(intensity.y) + base * intensity.x; // same combine as gouraud_light.fs

    out_color = vec4(lit, 1.0);
    out_meta  = vec4(float(u_tool_id), u_thickness, v_eye_z, 1.0); // a=1 marks "fragment written"
}
