#version 110

// NEOTKO_PHOTOMODE_TAG s242 — cyclorama shading. See photo_stage.vs for why this exists at all.
//
// Deliberately minimal: a flat base colour, the same hemisphere ambient the objects use so floor
// and object agree on where "sky" is, and the s229 shadow map. No SSAO and no contact shadows —
// both are screen-space effects driven by a G-buffer that only contains the objects, so on the
// floor they would either read as noise or as a hard edge where the buffer ends.
//
// Texturing (wood / brushed metal / user PNGs) and the planar floor reflection are pass 2; the
// uniforms they need are not stubbed here on purpose, so this file stays readable until then.

uniform vec4 uniform_color;

uniform sampler2D u_shadow_map;
uniform bool  u_shadow_enabled;
uniform vec2  u_shadow_texel;
uniform float u_shadow_bias_min;
uniform float u_shadow_bias_max;
uniform float u_shadow_strength;

uniform float u_ambient_ground;
uniform float u_ambient_sky;
uniform float u_light_key_diffuse;
uniform vec3  u_light_key_tint;

// NEOTKO_PHOTOMODE_TAG s242 — "shadow catcher" mode, used only when exporting with a transparent
// background.
//
// Without it the transparent-background toggle would be a no-op in Lightbox/Backdrop: the
// cyclorama is opaque and covers the frame, so clearing to alpha 0 leaves nothing transparent.
// The floor still has to be DRAWN, though, or the shadow has nothing to land on and the object
// floats. So in this mode the floor renders as pure shadow: fully transparent where it is lit,
// progressively opaque black where it is shadowed. Composited over any background, that reads as
// the object's own contact shadow — which is precisely what a shop listing or a quick crop wants.
uniform bool  u_shadow_catcher;

// NEOTKO_PHOTOMODE_TAG s242 (F6a) — how much of the mirrored scene shows through this floor.
// 0 = matte (opaque floor, no reflection). The mirrored geometry has already been drawn UNDER
// this surface, so "reflectivity" here is literally the floor's transparency.
uniform float u_reflect_strength;

varying vec3  v_view_normal;
varying vec4  v_shadow_coord;
varying float v_world_ndotl;

// 3x3 PCF, identical to shells_lit.fs. Kept as a copy rather than shared: GLSL has no include,
// and the two shaders' varyings differ enough that a "common" file would have to be parameterised
// more than it saves.
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
    // Outside the map = lit. On the objects this is nearly unreachable, but the cyclorama extends
    // far past the light frustum by design, so MOST of this surface takes this branch every frame.
    // That is the intended behaviour — a floor lit everywhere except where something shadows it —
    // and it is also why the C++ side has to grow the frustum enough to cover the cast shadow: a
    // shadow that runs past the map's edge does not fade, it stops dead.
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
    vec3 n = normalize(v_view_normal);
    float sky_mix = n.y * 0.5 + 0.5;
    float ambient = mix(u_ambient_ground, u_ambient_sky, sky_mix);

    // World N.L, already computed in the vertex stage for the shadow bias — reused so the floor's
    // shading tracks the light exactly like the objects' does.
    vec3 diffuse = u_light_key_tint * (v_world_ndotl * u_light_key_diffuse);

    float vis = 1.0;
    if (u_shadow_enabled)
        vis = mix(1.0, shadow_map_visibility(), u_shadow_strength);

    if (u_shadow_catcher) {
        // Alpha carries the shadow, colour is black. Nothing else about the floor is drawn.
        gl_FragColor = vec4(0.0, 0.0, 0.0, clamp(1.0 - vis, 0.0, 1.0));
        return;
    }

    // Ambient is never shadowed — same rule as shells_lit.fs. Shadowing it too turns the shadow
    // into a flat black hole instead of a soft patch, which on a white cyclorama is the single
    // most obvious way to make the shot look fake.
    vec3 lit = uniform_color.rgb * (ambient + diffuse * vis);

    // NEOTKO_PHOTOMODE_TAG s242 (F6a): let the reflection through, more at grazing angles than
    // face-on — which is why a wet street mirrors the sky ahead but not the tarmac at your feet.
    // NdotV is exact under the orthographic camera this slicer defaults to (the view direction is
    // constant, so it collapses to the normal's view-space z) and a good approximation otherwise.
    float alpha = uniform_color.a;
    if (u_reflect_strength > 0.0) {
        float ndotv = clamp(abs(n.z), 0.0, 1.0);
        float grazing = pow(1.0 - ndotv, 4.0);
        float refl = clamp(u_reflect_strength * mix(0.35, 1.0, grazing), 0.0, 0.95);
        alpha = uniform_color.a * (1.0 - refl);
    }
    gl_FragColor = vec4(lit, alpha);
}
