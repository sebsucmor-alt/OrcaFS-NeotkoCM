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

// NEOTKO_REALCOLOR_TAG s214 (PBR item 3, docs/WIP/REALCOLOR_VIEW/09_HDR_ENVIRONMENT_PLAN.md):
// procedural equirect environment — u_env_irradiance for the ambient/diffuse term, u_env_mirror
// (sharp) for the rim/specular reflection. u_camera_pos_world is WORLD space (matches
// v_view_normal/v_world_pos, see 09's coordinate-space note — do NOT reuse eye-space `position`
// from the specular highlight below, that would mix reference frames and make the reflection
// "swim" as the camera orbits instead of staying fixed in the room).
uniform sampler2D u_env_irradiance;
uniform sampler2D u_env_mirror;
uniform vec3 u_camera_pos_world;
uniform float u_fresnel_power;
uniform float u_fresnel_strength;
uniform vec3 u_fresnel_tint;

// x = direct-light diffuse only, y = specular; matches realcolor_peel.vs
in vec2 intensity;
in float v_eye_z; // linear eye-space depth, matches realcolor_peel.vs
in vec3 v_view_normal; // NEOTKO_REALCOLOR_TAG s166 (item 3): view-space normal, for SSAO in present
in vec3 v_world_pos; // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): world-space position, see realcolor_peel.vs

const float REALCOLOR_PI = 3.14159265359;

// NEOTKO_REALCOLOR_TAG s214: same convention as the CPU-side generator (realcolor_env_sample()
// in GCodeViewer.cpp) — v=0 is straight up (sky), v=1 is straight down (ground), matching the
// dir.y-based sky_mix already established by PBR item 1b.
vec2 equirect_uv(vec3 dir)
{
    dir = normalize(dir);
    float u = atan(dir.z, dir.x) / (2.0 * REALCOLOR_PI) + 0.5;
    float v = 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / REALCOLOR_PI;
    return vec2(u, v);
}

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

    vec3 N = normalize(v_view_normal); // world-space, see realcolor_peel.vs
    vec3 ambient = texture(u_env_irradiance, equirect_uv(N)).rgb;

    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): world-space view direction (surface -> camera),
    // built from u_camera_pos_world + v_world_pos — NOT from the eye-space `position` the
    // specular highlight below still uses, see the uniform block comment above for why.
    vec3 view_dir = normalize(u_camera_pos_world - v_world_pos);
    vec3 R = reflect(-view_dir, N);
    float fres = pow(1.0 - max(dot(view_dir, N), 0.0), u_fresnel_power);
    vec3 rim = u_fresnel_strength * fres * u_fresnel_tint * texture(u_env_mirror, equirect_uv(R)).rgb;

    vec3 base = u_material_rgb[u_tool_id];
    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): diffuse = direct light (achromatic) + env-sampled
    // ambient; lit = env-sampled rim + achromatic specular highlight + base*diffuse. Supersedes
    // PBR item 1's flat-tint mix (see realcolor_peel.vs) — not a superset of it.
    vec3 diffuse = vec3(intensity.x) + ambient;
    vec3 lit = vec3(intensity.y) + rim + base * diffuse;

    out_color = vec4(lit, 1.0);
    out_meta  = vec4(float(u_tool_id), u_thickness, v_eye_z, 1.0); // a=1 marks "fragment written"
    out_normal = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
}
