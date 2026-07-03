#version 110

// NEOTKO_REALCOLOR_TAG s166 (item 4): legacy/compat-profile counterpart of
// 140/shells_lit.vs — see that file for the full rationale, identical math here, just
// attribute/varying instead of in/out.

#define INTENSITY_CORRECTION 0.6

const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define AMBIENT_GROUND    0.18
#define AMBIENT_SKY       0.32
#define FRESNEL_POWER     5.0
#define FRESNEL_STRENGTH  0.06

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;

attribute vec3 v_position;
attribute vec3 v_normal;

varying vec2 intensity;
varying vec3 v_view_normal;

void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);
    v_view_normal = normal;

    float NdotL = max(dot(normal, LIGHT_TOP_DIR), 0.0);

    float sky_mix = normal.y * 0.5 + 0.5;
    intensity.x = mix(AMBIENT_GROUND, AMBIENT_SKY, sky_mix) + NdotL * LIGHT_TOP_DIFFUSE;
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, normal)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    float fres = pow(1.0 - max(dot(normalize(-position.xyz), normal), 0.0), FRESNEL_POWER);
    intensity.y += FRESNEL_STRENGTH * fres;

    gl_Position = projection_matrix * position;
}
