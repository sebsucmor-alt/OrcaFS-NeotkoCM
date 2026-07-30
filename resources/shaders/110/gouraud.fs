#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;

//BBS: add outline_color
uniform bool is_outline;
uniform sampler2D depth_tex;
uniform vec2 screen_size;


#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;

uniform float z_far;
uniform float z_near;

varying vec3 clipping_planes_dots;
varying float color_clip_plane_dot;

// x = diffuse, y = specular;
varying vec2 intensity;

varying vec4 world_pos;
varying vec4 weave_model_pos;   // NEOTKO_PROFILE_TAG s233
varying float world_normal_z;
varying vec3 eye_normal;
varying vec3 eye_pos;   // NEOTKO_SMOOTHNORMALS_TAG s229

// NEOTKO_SMOOTHNORMALS_TAG s229 — diagnostic views, pushed by GLShaderProgram::start_using().
//   0 = off (normal shading)
//   1 = eye-space shading normal as RGB
//   2 = deviation between the shading normal and the true geometric normal of the triangle
//       (recovered from screen-space derivatives of the eye position), amplified. Green = the
//       shading normal matches the facet, red = it has been smoothed away from it. On a badly
//       tessellated flat face this lights up exactly along the sliver triangles.
uniform int   shading_debug_view;
uniform float shading_debug_amplify;



// NEOTKO_PROFILE_TAG s233 — weave/degradado de la pintura ColorMix en la vista 3D
// NORMAL (fuera del gizmo). Copia literal del bloque que mm_gouraud.fs ya usaba para
// el preview del painter, para que dentro y fuera se vea EXACTAMENTE lo mismo.
// Completamente inerte mientras u_weave_on sea false: GLVolume::simple_render lo pone a
// false antes y después de cada trozo, y ningún otro camino de dibujo lo enciende, así
// que todo lo que no sea pintura ColorMix se renderiza bit a bit como antes.
uniform bool  u_weave_on;
uniform bool  u_weave_tile;       // true = repetir el patrón al ancho de línea real
uniform int   u_weave_n;          // bandas en la secuencia (<= 64)
uniform float u_weave_angle;      // radianes — orientación de las bandas
uniform float u_weave_pitch;      // mm — paso de banda
uniform float u_weave_p0;         // mm — proyección del borde de la superficie
uniform vec3  u_weave_cols[64];

vec3 weave_color(vec3 base)
{
    if (!u_weave_on || u_weave_n <= 0)
        return base;
    float s = sin(u_weave_angle);
    float c = cos(u_weave_angle);
    float proj = -weave_model_pos.x * s + weave_model_pos.y * c;
    float line = floor((proj - u_weave_p0) / max(u_weave_pitch, 0.0001));
    int   idx;
    if (u_weave_tile) {
        float fn = float(u_weave_n);
        idx = int(line - fn * floor(line / fn));
    } else {
        idx = int(line);
        if (idx < 0)             idx = 0;
        if (idx > u_weave_n - 1) idx = u_weave_n - 1;
    }
    for (int i = 0; i < 64; ++i)
        if (i == idx) return u_weave_cols[i];
    return base;
}

vec3 getBackfaceColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}

// Silhouette edge detection & rendering algorithem by leoneruggiero
// https://www.shadertoy.com/view/DslXz2
#define INFLATE 1

float GetTolerance(float d, float k)
{
    // -------------------------------------------
    // Find a tolerance for depth that is constant
    // in view space (k in view space).
    //
    // tol = k*ddx(ZtoDepth(z))
    // -------------------------------------------
    
    float A=-   (z_far+z_near)/(z_far-z_near);
    float B=-2.0*z_far*z_near /(z_far-z_near);
    
    d = d*2.0-1.0;
    
    return -k*(d+A)*(d+A)/B;   
}

float DetectSilho(vec2 fragCoord, vec2 dir)
{
    // -------------------------------------------
    //   x0 ___ x1----o 
    //          :\    : 
    //       r0 : \   : r1
    //          :  \  : 
    //          o---x2 ___ x3
    //
    // r0 and r1 are the differences between actual
    // and expected (as if x0..3 where on the same
    // plane) depth values.
    // -------------------------------------------
    
    float x0 = abs(texture2D(depth_tex, (fragCoord + dir*-2.0) / screen_size).r);
    float x1 = abs(texture2D(depth_tex, (fragCoord + dir*-1.0) / screen_size).r);
    float x2 = abs(texture2D(depth_tex, (fragCoord + dir* 0.0) / screen_size).r);
    float x3 = abs(texture2D(depth_tex, (fragCoord + dir* 1.0) / screen_size).r);
    
    float d0 = (x1-x0);
    float d1 = (x2-x3);
    
    float r0 = x1 + d0 - x2;
    float r1 = x2 + d1 - x1;
    
    float tol = GetTolerance(x2, 0.04);
    
    return smoothstep(0.0, tol*tol, max( - r0*r1, 0.0));

}

float DetectSilho(vec2 fragCoord)
{
    return max(
        DetectSilho(fragCoord, vec2(1,0)), // Horizontal
        DetectSilho(fragCoord, vec2(0,1))  // Vertical
        );
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
	if (use_color_clip_plane) {
		color.rgb = (color_clip_plane_dot < 0.0) ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
		color.a = uniform_color.a;
    }
    else
	    color = uniform_color;

    // NEOTKO_PROFILE_TAG s233 — pintura ColorMix tejida. Inerte (devuelve color.rgb tal
    // cual) mientras u_weave_on sea false, que es TODO el resto del tiempo. Va después
    // del clip-plane a propósito: ese modo pinta con sus propios colores de corte y no
    // debe llevar tejido.
    if (!use_color_clip_plane)
        color.rgb = weave_color(color.rgb);

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color.rgb = LightBlue;
                color.a = 0.8;
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }
    // if the fragment is outside the print volume -> use darker color
	vec3 pv_check_min = ZERO;
	vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
		// rectangle
		pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
		pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
	}
	else if (print_volume.type == 1) {
		// circle
		float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
		pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
		pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
	}
	color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    //BBS: add outline_color
    if (is_outline) {
        color = vec4(vec3(intensity.y) + color.rgb * intensity.x, color.a);
        vec2 fragCoord = gl_FragCoord.xy;
        float s = DetectSilho(fragCoord);
        // Makes silhouettes thicker.
        for(int i=1;i<=INFLATE; i++)
        {
           s = max(s, DetectSilho(fragCoord.xy + vec2(i, 0)));
           s = max(s, DetectSilho(fragCoord.xy + vec2(0, i)));
        }   
        gl_FragColor = vec4(mix(color.rgb, getBackfaceColor(color.rgb), s), color.a);
    }
#ifdef ENABLE_ENVIRONMENT_MAP
    else if (use_environment_tex)
        gl_FragColor = vec4(0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * intensity.x, color.a);
#endif
    else
        gl_FragColor = vec4(vec3(intensity.y) + color.rgb * intensity.x, color.a);

    // NEOTKO_SMOOTHNORMALS_TAG s229: diagnostic overrides, last word on the colour.
    if (shading_debug_view != 0) {
        vec3 n = normalize(eye_normal);
        if (shading_debug_view == 1)
            gl_FragColor = vec4(n * 0.5 + 0.5, 1.0);
        else {
            vec3 g = normalize(cross(dFdx(eye_pos), dFdy(eye_pos)));
            if (dot(g, n) < 0.0)
                g = -g;
            float dev = clamp(length(n - g) * shading_debug_amplify, 0.0, 1.0);
            gl_FragColor = vec4(dev, 1.0 - dev, 0.0, 1.0);
        }
    }
}
