#version 110

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

// NEOTKO_REALCOLOR_TAG: two-tone sky/ground ambient replacing the old flat INTENSITY_AMBIENT
// (0.3) — mixed by normal.y so faces pointing up read a touch brighter (open sky) and faces
// pointing down a touch darker (occluded by the print itself), instead of a uniform wash.
// Fresnel/rim term added into intensity.y (the channel realcolor_peel.fs already treats as
// additive/white, see "lit = vec3(intensity.y) + base*intensity.x" there) — gives a subtle edge
// highlight typical of translucent plastic/resin under directional light. Both live-tunable via
// GCodeViewer::RealColorTuning + the debug panel in render_toolpaths_realcolor() (see
// GCodeViewer.cpp set_uniform calls right before this shader's peel draw) — uniforms instead of
// #define so they can be retuned without recompiling shaders.
uniform float u_ambient_ground;
uniform float u_ambient_sky;
uniform float u_fresnel_power;
uniform float u_fresnel_strength;

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;

attribute vec3 v_position;
attribute vec3 v_normal;

// x = tainted, y = specular;
varying vec2 intensity;

// NEOTKO_REALCOLOR_TAG: linear eye-space depth (camera-space distance along the view axis),
// used by realcolor_peel.fs for the peel-order comparison instead of gl_FragCoord.z. NDC depth
// is nonlinear w.r.t. real distance, so a bias in NDC space breaks differently at every zoom
// level (confirmed empirically: the noisy band moved with zoom). This is zoom-invariant.
varying float v_eye_z;

// NEOTKO_REALCOLOR_TAG s166 (item 3, SSAO): view-space normal, forwarded to a 3rd MRT target
// in realcolor_peel.fs so the present pass can run screen-space AO — same `normal` already
// computed below for the ambient/fresnel terms, just exposed instead of being dropped after use.
varying vec3 v_view_normal;

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_peel.vs (see
// GLShadersManager.cpp comment for why both variants exist). Identical lighting math to
// gouraud_light.vs, just attribute/varying instead of in/out.
void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);
    v_view_normal = normal;

    float NdotL = max(dot(normal, LIGHT_TOP_DIR), 0.0);

    float sky_mix = normal.y * 0.5 + 0.5; // 0 = ground, 1 = sky
    intensity.x = mix(u_ambient_ground, u_ambient_sky, sky_mix) + NdotL * LIGHT_TOP_DIFFUSE;
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, normal)), 0.0), LIGHT_TOP_SHININESS);
    v_eye_z = -position.z; // right-handed eye space, camera looks down -Z

    NdotL = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    float fres = pow(1.0 - max(dot(normalize(-position.xyz), normal), 0.0), u_fresnel_power);
    intensity.y += u_fresnel_strength * fres;

    gl_Position = projection_matrix * position;
}
