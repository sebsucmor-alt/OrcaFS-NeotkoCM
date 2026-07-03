#version 110

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_peel.fs. Same
// front-to-back depth-peel math (see the 140 file for the full pipeline explanation) but MRT
// via gl_FragData[N] — a core GLSL 1.10 built-in, no layout(location=N) qualifier and no
// GL_ARB_explicit_attrib_location extension pragma needed. This is what lets RealColor run
// under macOS's Legacy/Compatibility GL profile (GL_VERSION capped at "2.1" even on modern
// Apple Silicon — see GCodeViewer.cpp::realcolor_gpu_supported() for why the app ends up
// there), which core-3.1-only GLSL 140 with explicit-location outputs cannot target.
// gl_FragData[0] = color, gl_FragData[1] = meta, gl_FragData[2] = view-space normal (s166,
// SSAO-only, ignored by the accumulate pass) — must match the attachment order set up in
// GCodeViewer.cpp::ensure_realcolor_fbos() (GL_COLOR_ATTACHMENT0/1/2 via glDrawBuffers).

uniform sampler2D u_prev_meta;  // previous pass's meta (tool_id, thickness, eye_z, written) — NOT a depth texture
uniform bool  u_has_prev_depth; // false for peel pass 0 (nothing to peel behind yet)
uniform int   u_tool_id;        // physical tool (0-3) for this draw, see RenderPath/Path::extruder_id
uniform float u_thickness;      // Path::height for this draw, mm
uniform vec3  u_material_rgb[4];
uniform vec2  u_viewport;

// x = tainted, y = specular; matches realcolor_peel.vs
varying vec2 intensity;
varying float v_eye_z; // linear eye-space depth, matches realcolor_peel.vs
varying vec3 v_view_normal; // NEOTKO_REALCOLOR_TAG s166 (item 3): view-space normal, for SSAO in present

void main()
{
    if (u_has_prev_depth) {
        vec2 uv = gl_FragCoord.xy / u_viewport;
        float prevEyeZ = texture2D(u_prev_meta, uv).b;
        // NEOTKO_REALCOLOR_TAG: slope-scaled bias in LINEAR eye-space depth (mm), not NDC
        // gl_FragCoord.z — NDC depth is nonlinear w.r.t. real distance, so a bias there breaks
        // differently at every zoom level (confirmed: the noisy band moved with zoom). Linear
        // depth VALUES are zoom-invariant, but fwidth(v_eye_z) (a SCREEN-SPACE derivative) is
        // NOT — at low zoom/far camera distance one screen pixel spans more real mm of depth,
        // so fwidth grows with zoom-out too, not only with grazing angle as originally intended
        // here (s164). NEOTKO_REALCOLOR_TAG s166 FIX: see 140/realcolor_peel.fs for the full
        // rationale — left uncapped, at enough zoom-out the bias can exceed the real gap between
        // two thin adjacent layers, wrongly discarding the layer being peeled and silently
        // dropping it from the Beer-Lambert stack. Capped to half of THIS pass's own real
        // thickness (u_thickness, mm); bias_cap floored at 2e-3 so clamp() never gets
        // minVal>maxVal on a vanishingly thin PathBlend sub-layer.
        float bias_cap = max(u_thickness * 0.5, 2e-3);
        float bias = clamp(fwidth(v_eye_z) * 2.0, 2e-3, bias_cap);
        if (v_eye_z <= prevEyeZ + bias)
            discard; // already surfaced in an earlier peel pass
    }

    vec3 base = u_material_rgb[u_tool_id];
    vec3 lit = vec3(intensity.y) + base * intensity.x; // same combine as gouraud_light.fs

    gl_FragData[0] = vec4(lit, 1.0);
    gl_FragData[1] = vec4(float(u_tool_id), u_thickness, v_eye_z, 1.0); // a=1 marks "fragment written"
    // NEOTKO_REALCOLOR_TAG s166 (item 3): 3rd MRT target for SSAO in realcolor_present.fs —
    // packs the view-space normal [-1,1] -> [0,1], same convention as the 140/ variant.
    gl_FragData[2] = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
}
