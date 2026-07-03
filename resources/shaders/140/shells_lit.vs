#version 140

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

// NEOTKO_REALCOLOR_TAG s166 (item 4): same two-tone ambient + fresnel treatment already
// shipped for RealColor's peel shader (item 2, s165) — reused here instead of inventing new
// constants, see realcolor_peel.vs for the original rationale/tuning notes. Fixed #defines
// (not live-tunable uniforms like RealColor's debug panel) since this shader is gated
// separately and isn't part of the RealColor Tuning panel — see render_shells() in
// GCodeViewer.cpp.
#define AMBIENT_GROUND    0.18
#define AMBIENT_SKY       0.32
#define FRESNEL_POWER     5.0
#define FRESNEL_STRENGTH  0.06

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;

in vec3 v_position;
in vec3 v_normal;

// x = tainted, y = specular; matches gouraud_light.vs
out vec2 intensity;
// NEOTKO_REALCOLOR_TAG s166: forwarded for the AO normal-agreement weight in shells_lit.fs —
// same pattern as realcolor_peel.vs's v_view_normal.
out vec3 v_view_normal;

// NEOTKO_REALCOLOR_TAG s166 (item 4): drop-in replacement for gouraud_light.vs when rendering
// shells in render_shells() — same view_model_matrix/projection_matrix/view_normal_matrix/
// v_position/v_normal contract, so GLVolumeCollection::render() drives it identically. Adds
// the ambient/fresnel treatment already validated for RealColor's peel shader.
void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);
    v_view_normal = normal;

    float NdotL = max(dot(normal, LIGHT_TOP_DIR), 0.0);

    float sky_mix = normal.y * 0.5 + 0.5; // 0 = ground, 1 = sky
    intensity.x = mix(AMBIENT_GROUND, AMBIENT_SKY, sky_mix) + NdotL * LIGHT_TOP_DIFFUSE;
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, normal)), 0.0), LIGHT_TOP_SHININESS);

    NdotL = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    float fres = pow(1.0 - max(dot(normalize(-position.xyz), normal), 0.0), FRESNEL_POWER);
    intensity.y += FRESNEL_STRENGTH * fres;

    gl_Position = projection_matrix * position;
}
