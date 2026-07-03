#version 140
#extension GL_ARB_explicit_attrib_location : require

// NEOTKO_REALCOLOR_TAG: one pass of front-to-back depth peeling over the SAME toolpath
// geometry render_toolpaths() already draws (see render_toolpaths_realcolor() in
// GCodeViewer.cpp). Pass i keeps only fragments strictly farther than pass i-1's depth,
// so each pass extracts the next visible layer. Writes color (attachment 0),
// (tool_id, thickness, -, written-flag) meta (attachment 1) for the accumulate pass, and
// (s166) view-space normal (attachment 2) consumed only by realcolor_present.fs's SSAO —
// the accumulate pass ignores attachment 2 entirely.

uniform sampler2D u_prev_meta;  // previous pass's meta (tool_id, thickness, eye_z, written) — NOT a depth texture
uniform bool  u_has_prev_depth; // false for peel pass 0 (nothing to peel behind yet)
uniform int   u_tool_id;        // physical tool (0-3) for this draw, see RenderPath/Path::extruder_id
uniform float u_thickness;      // Path::height for this draw, mm
uniform vec3  u_material_rgb[4];
uniform vec2  u_viewport;

// x = tainted, y = specular; matches gouraud_light.vs
in vec2 intensity;
in float v_eye_z; // linear eye-space depth, matches realcolor_peel.vs
in vec3 v_view_normal; // NEOTKO_REALCOLOR_TAG s166 (item 3): view-space normal, for SSAO in present

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_meta;
// NEOTKO_REALCOLOR_TAG s166 (item 3): 3rd MRT target for SSAO in realcolor_present.fs — packs
// the view-space normal [-1,1] -> [0,1] (same texture format family, GL_RGBA32F, as out_meta;
// packing rather than a signed format keeps every peel attachment on the same convention).
layout(location = 2) out vec4 out_normal;

void main()
{
    if (u_has_prev_depth) {
        vec2 uv = gl_FragCoord.xy / u_viewport;
        float prevEyeZ = texture(u_prev_meta, uv).b;
        // NEOTKO_REALCOLOR_TAG: slope-scaled bias in LINEAR eye-space depth (mm), not NDC
        // gl_FragCoord.z — NDC depth is nonlinear w.r.t. real distance, so a bias there breaks
        // differently at every zoom level (confirmed: the noisy band moved with zoom). Linear
        // depth VALUES are zoom-invariant, but fwidth(v_eye_z) (a SCREEN-SPACE derivative) is
        // NOT — at low zoom/far camera distance one screen pixel spans more real mm of depth,
        // so fwidth grows with zoom-out too, not only with grazing angle as originally intended
        // here (s164). NEOTKO_REALCOLOR_TAG s166 FIX: left uncapped, at enough zoom-out the bias
        // can exceed the real physical gap between two thin adjacent layers (e.g. a 0.13-0.2mm
        // Sandwich top pass) — this peel pass then wrongly treats the layer it's trying to
        // detect as "already surfaced" and discards it whole, silently dropping that layer's
        // entire contribution from the Beer-Lambert stack (reported by the user: the layer right
        // under the top one vanishes from the color mix specifically when zoomed out, correct
        // again zoomed in — exactly this mechanism, not a refresh/debounce issue). Capped to half
        // of THIS pass's own real thickness (u_thickness, mm) — the largest bias that still can't
        // swallow the very layer being peeled right now. bias_cap itself floored at 2e-3 too, so
        // clamp() never gets minVal>maxVal on a vanishingly thin PathBlend sub-layer.
        float bias_cap = max(u_thickness * 0.5, 2e-3);
        float bias = clamp(fwidth(v_eye_z) * 2.0, 2e-3, bias_cap);
        if (v_eye_z <= prevEyeZ + bias)
            discard; // already surfaced in an earlier peel pass
    }

    vec3 base = u_material_rgb[u_tool_id];
    vec3 lit = vec3(intensity.y) + base * intensity.x; // same combine as gouraud_light.fs

    out_color = vec4(lit, 1.0);
    out_meta  = vec4(float(u_tool_id), u_thickness, v_eye_z, 1.0); // a=1 marks "fragment written"
    out_normal = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
}
