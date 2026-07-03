#version 140

// NEOTKO_REALCOLOR_TAG s166 (item 4): drop-in replacement for gouraud_light.fs when rendering
// shells in render_shells() (Preview only, gated by NeoDebug::REALCOLOR — see
// GCodeViewer.cpp). Same uniform_color/emission_factor contract as gouraud_light.fs
// (GLVolumeCollection::render sets these by name regardless of which shader is bound, see
// 3DScene.cpp), plus screen-space AO sampled from the shells_gbuffer pre-pass. Uses
// gl_FragColor (not an explicit `out` var) to match gouraud_light.fs's own convention under
// this same #version — this codebase already runs its "140" shaders under a profile where
// that's valid (gouraud_light.fs does the same), so this isn't a new pattern.

uniform vec4 uniform_color;
uniform float emission_factor;
// NEOTKO_REALCOLOR_TAG s166: shells_gbuffer.fs output — rgb = packed view-space normal
// ([-1,1]->[0,1]), a = linear eye-space depth (mm); a<=0.0 marks "no shell geometry here"
// (see shells_gbuffer.fs / ensure_shells_ao_fbo's clear color).
uniform sampler2D u_gbuffer;
uniform vec2 u_viewport;    // canvas size in px — u_gbuffer is native-resolution, no supersampling here
uniform float u_ao_radius;  // px
uniform float u_ao_strength;

in vec2 intensity;
in vec3 v_view_normal;

// NEOTKO_REALCOLOR_TAG s166: same depth-diff + normal-agreement AO as
// realcolor_present.fs's compute_ao() (see that file for the full rationale / Kajalin 2007
// citation) — separate copy because this samples a different G-buffer texture (shells_gbuffer,
// single opaque pass) at native resolution, not RealColor's supersampled peel textures.
const float SHELLS_AO_MAX_DELTA_MM = 1.5;

float ao_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir)
{
    vec2 s_uv = center_uv + dir * (u_ao_radius / u_viewport);
    vec4 s = texture(u_gbuffer, s_uv);
    if (s.a <= 0.0)
        return 0.0; // background — doesn't occlude
    float delta = center_z - s.a; // positive: neighbor is CLOSER to camera than center
    if (delta <= 0.0)
        return 0.0; // neighbor is farther away or coplanar — not an occluder of this pixel
    float range_falloff = clamp(1.0 - delta / SHELLS_AO_MAX_DELTA_MM, 0.0, 1.0);
    vec3 s_n = normalize(s.rgb * 2.0 - 1.0);
    float normal_agreement = max(dot(center_n, s_n), 0.0);
    return range_falloff * normal_agreement;
}

float compute_ao(vec2 uv)
{
    vec4 center = texture(u_gbuffer, uv);
    if (center.a <= 0.0)
        return 1.0; // shouldn't happen (this fragment IS shell geometry) but stay safe
    vec3 center_n = normalize(center.rgb * 2.0 - 1.0);
    float center_z = center.a;

    float occlusion = 0.0;
    occlusion += ao_sample(uv, center_z, center_n, vec2( 1.0,  0.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2(-1.0,  0.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.0,  1.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.0, -1.0));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.7,  0.7));
    occlusion += ao_sample(uv, center_z, center_n, vec2(-0.7,  0.7));
    occlusion += ao_sample(uv, center_z, center_n, vec2( 0.7, -0.7));
    occlusion += ao_sample(uv, center_z, center_n, vec2(-0.7, -0.7));
    occlusion /= 8.0;
    return 1.0 - clamp(occlusion, 0.0, 1.0);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / u_viewport;
    float ao = compute_ao(uv);

    vec3 lit = vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor);
    lit *= mix(1.0, ao, u_ao_strength);
    gl_FragColor = vec4(lit, uniform_color.a);
}
