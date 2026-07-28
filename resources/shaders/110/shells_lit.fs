#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4): legacy/compat-profile counterpart of
// 140/shells_lit.fs — see that file for the full rationale. texture2D() replaces texture(),
// gl_FragColor stays a builtin in every GLSL version.
//
// NEOTKO_SHADOW_TAG s229: SSCS (Fase 1) + directional shadow map with 3x3 PCF (Fase 2), math
// identical to the 140 variant. See 140/shells_lit.fs for the reasoning behind every block and
// docs/FUTURE/SHADOWS_REALISTIC_CAST_RESEARCH.md for the technique study.

uniform vec4 uniform_color;
uniform float emission_factor;
uniform sampler2D u_gbuffer;
uniform vec2 u_viewport;
uniform float u_ao_radius;
uniform float u_ao_strength;

uniform mat4  projection_matrix;
uniform float u_sscs_length_mm;
uniform float u_sscs_thickness_mm;
uniform float u_sscs_strength;

uniform sampler2D u_shadow_map;
uniform bool  u_shadow_enabled;
uniform vec2  u_shadow_texel;
uniform float u_shadow_bias_min;
uniform float u_shadow_bias_max;
uniform float u_shadow_strength;

varying vec2 intensity;
varying vec3 v_view_normal;
varying vec3 v_view_pos;
varying float v_ambient;
varying vec4 v_shadow_coord;
varying float v_world_ndotl;

const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);

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

// NEOTKO_SHADOW_TAG s229 (Fase 1) — screen-space contact shadows, see 140/shells_lit.fs.
#define SSCS_STEPS 12
const float SSCS_SELF_BIAS_MM = 0.08;

float sscs_occlusion()
{
    if (u_sscs_strength <= 0.0 || u_sscs_length_mm <= 0.0)
        return 0.0;

    vec3 rd = normalize(LIGHT_TOP_DIR);
    float step_mm = u_sscs_length_mm / float(SSCS_STEPS);
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float t = step_mm * (0.5 + 0.5 * jitter);

    for (int i = 0; i < SSCS_STEPS; ++i) {
        vec3 p = v_view_pos + rd * t;
        vec4 clip = projection_matrix * vec4(p, 1.0);
        if (clip.w <= 0.0)
            break;
        vec2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
            break;
        float scene_z = texture2D(u_gbuffer, suv).a;
        if (scene_z > 0.0) {
            float delta = (-p.z) - scene_z;
            if (delta > SSCS_SELF_BIAS_MM && delta < u_sscs_thickness_mm)
                return 1.0 - t / u_sscs_length_mm;
        }
        t += step_mm;
    }
    return 0.0;
}

// NEOTKO_SHADOW_TAG s229 (Fase 2) — shadow map lookup, 3x3 PCF, see 140/shells_lit.fs.
float shadow_tap(vec2 uv, float r)
{
    return (r > texture2D(u_shadow_map, uv).r) ? 0.0 : 1.0;
}

float shadow_map_visibility()
{
    if (v_shadow_coord.w <= 0.0)
        return 1.0;
    vec3 proj = v_shadow_coord.xyz / v_shadow_coord.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;
    float ref = proj.z * 0.5 + 0.5;
    if (ref <= 0.0 || ref >= 1.0)
        return 1.0;

    float bias = max(u_shadow_bias_min, u_shadow_bias_max * (1.0 - v_world_ndotl));
    float r = ref - bias;

    float s = 0.0;
    s += shadow_tap(uv + vec2(-1.0, -1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 0.0, -1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 1.0, -1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2(-1.0,  0.0) * u_shadow_texel, r);
    s += shadow_tap(uv,                                     r);
    s += shadow_tap(uv + vec2( 1.0,  0.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2(-1.0,  1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 0.0,  1.0) * u_shadow_texel, r);
    s += shadow_tap(uv + vec2( 1.0,  1.0) * u_shadow_texel, r);
    return s / 9.0;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / u_viewport;
    float ao = compute_ao(uv);

    float vis = 1.0;
    if (u_shadow_enabled)
        vis = mix(1.0, shadow_map_visibility(), u_shadow_strength);
    vis = min(vis, 1.0 - u_sscs_strength * sscs_occlusion());

    vec3 lit = uniform_color.rgb * (v_ambient + emission_factor)
             + (uniform_color.rgb * max(intensity.x - v_ambient, 0.0) + vec3(intensity.y)) * vis;
    lit *= mix(1.0, ao, u_ao_strength);
    gl_FragColor = vec4(lit, uniform_color.a);
}
