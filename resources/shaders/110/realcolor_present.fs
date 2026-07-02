#version 110

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_present.fs. Single
// output, so this one uses gl_FragColor directly (no MRT needed, no gl_FragData involved) —
// texture2D() replaces texture() and gl_FragDepth stays a builtin in every GLSL version.
// Presents the cached/just-computed composite into whichever framebuffer is bound (the
// default one, when called from render_toolpaths_realcolor()). Writes gl_FragDepth from peel
// pass 0 (the nearest toolpath surface) so the normal GL depth test integrates correctly with
// shells already drawn by render_shells(). Discards where peel pass 0 never wrote anything.

uniform sampler2D u_accum_color;   // linear RGB, see realcolor_accum.fs
uniform sampler2D u_peel_meta0;    // pass-0 meta: a=1 marks "toolpath here"
uniform sampler2D u_peel_depth0;   // pass-0 depth
uniform vec2 u_texel_size;         // 1 / supersampled peel/accum texture resolution

varying vec2 uv;

float linear_to_srgb(float c) { c = clamp(c, 0.0, 1.0); return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// NEOTKO_REALCOLOR_TAG: 2x2 box-filter downsample from the REALCOLOR_SUPERSAMPLE-resolution
// peel/accum textures to the real canvas pixel — fixes thin ColorStitch/PathBlend geometry
// vanishing at single-sample resolution (root cause: the default framebuffer gets 4x MSAA via
// WX_GL_SAMPLE_BUFFERS/SAMPLES, these offscreen FBOs don't, so sub-pixel-wide toolpath lines
// can miss every sample point entirely). Coverage-weighted: taps with no geometry (meta.a==0)
// don't dilute the averaged color/depth of taps that DO have geometry — coverage itself becomes
// this pixel's alpha, blended against whatever render_shells()/background is already in the
// framebuffer (see glEnable(GL_BLEND) in render_toolpaths_realcolor's present-pass setup) so
// thin/faint features fade in smoothly instead of disappearing outright.
void main()
{
    vec2 o1 = vec2(-0.5, -0.5) * u_texel_size;
    vec2 o2 = vec2( 0.5, -0.5) * u_texel_size;
    vec2 o3 = vec2(-0.5,  0.5) * u_texel_size;
    vec2 o4 = vec2( 0.5,  0.5) * u_texel_size;

    float a1 = step(0.5, texture2D(u_peel_meta0, uv + o1).a);
    float a2 = step(0.5, texture2D(u_peel_meta0, uv + o2).a);
    float a3 = step(0.5, texture2D(u_peel_meta0, uv + o3).a);
    float a4 = step(0.5, texture2D(u_peel_meta0, uv + o4).a);

    float coverage = a1 + a2 + a3 + a4;
    if (coverage < 0.5) // none of the 4 taps saw any toolpath geometry here
        discard;

    vec3 c1 = a1 * texture2D(u_accum_color, uv + o1).rgb;
    vec3 c2 = a2 * texture2D(u_accum_color, uv + o2).rgb;
    vec3 c3 = a3 * texture2D(u_accum_color, uv + o3).rgb;
    vec3 c4 = a4 * texture2D(u_accum_color, uv + o4).rgb;
    vec3 lin = (c1 + c2 + c3 + c4) / coverage;

    float d1 = mix(1.0, texture2D(u_peel_depth0, uv + o1).r, a1);
    float d2 = mix(1.0, texture2D(u_peel_depth0, uv + o2).r, a2);
    float d3 = mix(1.0, texture2D(u_peel_depth0, uv + o3).r, a3);
    float d4 = mix(1.0, texture2D(u_peel_depth0, uv + o4).r, a4);
    float min_depth = min(min(d1, d2), min(d3, d4));

    gl_FragColor = vec4(linear_to_srgb(lin.r), linear_to_srgb(lin.g), linear_to_srgb(lin.b), coverage / 4.0);
    gl_FragDepth = min_depth;
}
