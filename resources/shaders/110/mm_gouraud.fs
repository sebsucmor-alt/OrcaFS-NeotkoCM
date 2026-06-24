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

const vec3  ZERO    = vec3(0.0, 0.0, 0.0);
const float EPSILON = 0.0001;
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
uniform vec4 uniform_color;

// NEOTKO_COLORSTITCH_TAG — weave preview: procedural stripes reproducing the
// ColorStitch per-line tool sequence, projected in world space. Driven per-slot
// from GLGizmoColorMixPainter. Inert (identical to stock) when u_weave_on=false.
uniform bool  u_weave_on;
uniform bool  u_weave_tile;       // true = repeat pattern at real line width (wrap); false = span once
uniform int   u_weave_n;          // stripes in the sequence LUT (<= 64)
uniform float u_weave_angle;      // radians — band orientation (along the fill lines)
uniform float u_weave_pitch;      // mm — stripe pitch (real line width when tiling)
uniform float u_weave_p0;         // mm — projection of the surface edge (object-local axis)
uniform vec3  u_weave_cols[64];   // per-stripe colour, already sequenced (tool colours)

uniform bool volume_mirrored;

uniform mat4 view_model_matrix;
uniform mat3 view_normal_matrix;

varying vec3 clipping_planes_dots;
varying vec4 model_pos;
varying vec4 world_pos;

struct SlopeDetection
{
    bool actived;
	 float normal_z;
    mat3 volume_world_normal_matrix;
};
uniform SlopeDetection slope;

//BBS: add wireframe logic
varying vec3 barycentric_coordinates;
float edgeFactor(float lineWidth) {
    vec3 d = fwidth(barycentric_coordinates);
    vec3 a3 = smoothstep(vec3(0.0), d * lineWidth, barycentric_coordinates);
    return min(min(a3.x, a3.y), a3.z);
}

vec3 wireframe(vec3 fill, vec3 stroke, float lineWidth) {
    return mix(stroke, fill, edgeFactor(lineWidth));
	//if (any(lessThan(barycentric_coordinates, vec3(0.005, 0.005, 0.005))))
	//	return vec3(1.0, 0.0, 0.0);
	//else
	//	return fill;
}

vec3 getWireframeColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}
uniform bool show_wireframe;

// NEOTKO_COLORSTITCH_TAG — pick the stripe colour for this fragment. Projects the
// world-space position onto the axis perpendicular to the fill lines, quantises
// to the stripe pitch, and looks up the tiled tool sequence. Constant-index loop
// avoids dynamic uniform-array indexing (portable across drivers).
vec3 weave_color(vec3 base)
{
    if (!u_weave_on || u_weave_n <= 0)
        return base;
    float s = sin(u_weave_angle);
    float c = cos(u_weave_angle);
    // Object-local projection (model_pos) onto the axis across the fill lines, then
    // index the per-line sequence spanning the surface [p0 .. p0 + n*pitch]. Clamp
    // (no wrap) so gradients run once and patterns tile via the sequence itself.
    float proj = -model_pos.x * s + model_pos.y * c;
    float line = floor((proj - u_weave_p0) / max(u_weave_pitch, 0.0001));
    int   idx;
    if (u_weave_tile) {
        // PATTERN: wrap into [0, n) at real line width. GLSL 1.10 has no integer % ,
        // so use float modulo (floor handles negatives: -1 mod 2 -> 1).
        float fn = float(u_weave_n);
        idx = int(line - fn * floor(line / fn));
    } else {
        // GRADIENT: clamp (run once across the surface). No integer clamp() in 1.10.
        idx = int(line);
        if (idx < 0)             idx = 0;
        if (idx > u_weave_n - 1) idx = u_weave_n - 1;
    }
    for (int i = 0; i < 64; ++i)
        if (i == idx) return u_weave_cols[i];
    return base;
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;
    vec3  color = weave_color(uniform_color.rgb);
    float alpha = uniform_color.a;

    vec3 triangle_normal = normalize(cross(dFdx(model_pos.xyz), dFdy(model_pos.xyz)));
#ifdef FLIP_TRIANGLE_NORMALS
    triangle_normal = -triangle_normal;
#endif

    if (volume_mirrored)
        triangle_normal = -triangle_normal;

    vec3 transformed_normal = normalize(slope.volume_world_normal_matrix * triangle_normal);
     
    if (slope.actived) {
        if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
              color = LightBlue;
              alpha = 1.0;
         }
         else if( transformed_normal.z < slope.normal_z - EPSILON)
        {
            color = color * 0.5 + LightRed * 0.5;
            alpha = 1.0;
        }
    }
    // First transform the normal into camera space and normalize the result.
    vec3 eye_normal = normalize(view_normal_matrix * triangle_normal);

    // Compute the cos of the angle between the normal and lights direction. The light is directional so the direction is constant for every vertex.
    // Since these two are normalized the cosine is the dot product. We also need to clamp the result to the [0,1] range.
    float NdotL = max(dot(eye_normal, LIGHT_TOP_DIR), 0.0);

    // x = diffuse, y = specular;
    vec2 intensity = vec2(0.0);
    intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    vec3 position = (view_model_matrix * model_pos).xyz;
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position), reflect(-LIGHT_TOP_DIR, eye_normal)), 0.0), LIGHT_TOP_SHININESS);

    // Perform the same lighting calculation for the 2nd light source (no specular applied).
    NdotL = max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    if (show_wireframe) {
        vec3 wireframeColor = show_wireframe ? getWireframeColor(color) : color;
        vec3 triangleColor = wireframe(color, wireframeColor, 1.0);
        gl_FragColor = vec4(vec3(intensity.y) + triangleColor * intensity.x, alpha);
    }
    else {
        gl_FragColor = vec4(vec3(intensity.y) + color * intensity.x, alpha);
    }
}
