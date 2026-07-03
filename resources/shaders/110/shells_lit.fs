#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4): legacy/compat-profile counterpart of
// 140/shells_lit.fs — see that file for the full rationale. texture2D() replaces texture(),
// gl_FragColor stays a builtin in every GLSL version.

uniform vec4 uniform_color;
uniform float emission_factor;
uniform sampler2D u_gbuffer;
uniform vec2 u_viewport;
uniform float u_ao_radius;
uniform float u_ao_strength;

varying vec2 intensity;
varying vec3 v_view_normal;

const float SHELLS_AO_MAX_DELTA_MM = 1.5;

float ao_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir)
{
    vec2 s_uv = center_uv + dir * (u_ao_radius / u_viewport);
    vec4 s = texture2D(u_gbuffer, s_uv);
    if (s.a <= 0.0)
        return 0.0;
    float delta = center_z - s.a;
    if (delta <= 0.0)
        return 0.0;
    float range_falloff = clamp(1.0 - delta / SHELLS_AO_MAX_DELTA_MM, 0.0, 1.0);
    vec3 s_n = normalize(s.rgb * 2.0 - 1.0);
    float normal_agreement = max(dot(center_n, s_n), 0.0);
    return range_falloff * normal_agreement;
}

float compute_ao(vec2 uv)
{
    vec4 center = texture2D(u_gbuffer, uv);
    if (center.a <= 0.0)
        return 1.0;
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
