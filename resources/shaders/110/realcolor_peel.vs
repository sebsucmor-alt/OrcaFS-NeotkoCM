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

#define INTENSITY_AMBIENT    0.3

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

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_peel.vs (see
// GLShadersManager.cpp comment for why both variants exist). Identical lighting math to
// gouraud_light.vs, just attribute/varying instead of in/out.
void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);

    float NdotL = max(dot(normal, LIGHT_TOP_DIR), 0.0);

    intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, normal)), 0.0), LIGHT_TOP_SHININESS);
    v_eye_z = -position.z; // right-handed eye space, camera looks down -Z

    NdotL = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    gl_Position = projection_matrix * position;
}
