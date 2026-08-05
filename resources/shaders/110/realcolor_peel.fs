#version 110

// NEOTKO_REALCOLOR_TAG: legacy/compat-profile counterpart of 140/realcolor_peel.fs. Same
// front-to-back depth-peel math (see the 140 file for the full pipeline explanation) but MRT
// via gl_FragData[N] — a core GLSL 1.10 built-in, no layout(location=N) qualifier and no
// GL_ARB_explicit_attrib_location extension pragma needed. This is what lets RealColor run
// under macOS's Legacy/Compatibility GL profile (GL_VERSION capped at "2.1" even on modern
// Apple Silicon — see GCodeViewer.cpp::realcolor_gpu_supported() for why the app ends up
// there), which core-3.1-only GLSL 140 with explicit-location outputs cannot target.
// gl_FragData[0] = color, gl_FragData[1] = meta, gl_FragData[2] = view-space normal (s166,
// SSAO-only, ignored by the accumulate pass) — must match the attachment order set up in
// GCodeViewer.cpp::ensure_realcolor_fbos() (GL_COLOR_ATTACHMENT0/1/2 via glDrawBuffers).

uniform sampler2D u_prev_meta;  // previous pass's meta (tool_id, thickness, eye_z, written) — NOT a depth texture
uniform bool  u_has_prev_depth; // false for peel pass 0 (nothing to peel behind yet)
uniform int   u_tool_id;        // physical tool (0-3) for this draw, see RenderPath/Path::extruder_id
uniform float u_thickness;      // Path::height for this draw, mm
uniform vec3  u_material_rgb[4];
uniform vec2  u_viewport;

// NEOTKO_REALCOLOR_TAG s214 (PBR item 3, docs/WIP/REALCOLOR_VIEW/09_HDR_ENVIRONMENT_PLAN.md):
// procedural equirect environment — u_env_irradiance for the ambient/diffuse term, u_env_mirror
// (sharp) for the rim/specular reflection. u_camera_pos_world is WORLD space (matches
// v_view_normal/v_world_pos, see 09's coordinate-space note — do NOT reuse eye-space `position`
// from the specular highlight below, that would mix reference frames and make the reflection
// "swim" as the camera orbits instead of staying fixed in the room).
uniform sampler2D u_env_irradiance;
uniform sampler2D u_env_mirror;
uniform vec3 u_camera_pos_world;
uniform float u_fresnel_power;
uniform float u_fresnel_strength;
uniform vec3 u_fresnel_tint;

// NEOTKO_REALCOLOR_TAG s243 (F4 = A4+A5): acabado por tipo de superficie, mandado por sub-draw.
// .x = brillo, .y = rugosidad 0..1 (elige entre mapa nítido y pre-difuminado, no toca exponentes).
// Neutro = (1.0, 0.0) ≡ comportamiento pre-s243. Ver 140/realcolor_peel.fs.
uniform vec2 u_finish;

// x = direct-light diffuse only, y = specular; matches realcolor_peel.vs
varying vec2 intensity;
varying float v_eye_z; // linear eye-space depth, matches realcolor_peel.vs
varying vec3 v_view_normal; // NEOTKO_REALCOLOR_TAG s166 (item 3): view-space normal, for SSAO in present
varying vec3 v_world_pos; // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): world-space position, see realcolor_peel.vs

const float REALCOLOR_PI = 3.14159265359;

// NEOTKO_REALCOLOR_TAG s214: same convention as the CPU-side generator (realcolor_env_sample()
// in GCodeViewer.cpp) — v=0 is straight up (sky), v=1 is straight down (ground), matching the
// dir.y-based sky_mix already established by PBR item 1b.
// ✅ NEOTKO_REALCOLOR_TAG s243 (F3) — EJE ARRIBA, RESUELTO. Latitud desde dir.z (cielo en +Z
// mundo, encima de la cama) y longitud girando alrededor de Z. El generador
// (realcolor_env_sample) NO se toca: trabaja en (u,v) con el contrato "v=0 arriba", que era
// correcto; lo que estaba mal era este mapeo dirección → (u,v). Razonamiento completo en
// 140/realcolor_peel.fs.
vec2 equirect_uv(vec3 dir)
{
    dir = normalize(dir);
    float u = atan(dir.y, dir.x) / (2.0 * REALCOLOR_PI) + 0.5;
    float v = 0.5 - asin(clamp(dir.z, -1.0, 1.0)) / REALCOLOR_PI;
    return vec2(u, v);
}

void main()
{
    if (u_has_prev_depth) {
        vec2 uv = gl_FragCoord.xy / u_viewport;
        float prevEyeZ = texture2D(u_prev_meta, uv).b;
        // NEOTKO_REALCOLOR_TAG: slope-scaled bias in LINEAR eye-space depth (mm), not NDC
        // gl_FragCoord.z — NDC depth is nonlinear w.r.t. real distance, so a bias there breaks
        // differently at every zoom level (confirmed: the noisy band moved with zoom). Linear
        // depth VALUES are zoom-invariant, but fwidth(v_eye_z) (a SCREEN-SPACE derivative) is
        // NOT — at low zoom/far camera distance one screen pixel spans more real mm of depth,
        // so fwidth grows with zoom-out too, not only with grazing angle as originally intended
        // here (s164). NEOTKO_REALCOLOR_TAG s166 FIX: see 140/realcolor_peel.fs for the full
        // rationale — left uncapped, at enough zoom-out the bias can exceed the real gap between
        // two thin adjacent layers, wrongly discarding the layer being peeled and silently
        // dropping it from the Beer-Lambert stack. Capped to half of THIS pass's own real
        // thickness (u_thickness, mm); bias_cap floored at 2e-3 so clamp() never gets
        // minVal>maxVal on a vanishingly thin PathBlend sub-layer.
        float bias_cap = max(u_thickness * 0.5, 2e-3);
        float bias = clamp(fwidth(v_eye_z) * 2.0, 2e-3, bias_cap);
        if (v_eye_z <= prevEyeZ + bias)
            discard; // already surfaced in an earlier peel pass
    }

    vec3 N = normalize(v_view_normal); // world-space, see realcolor_peel.vs
    vec3 ambient = texture2D(u_env_irradiance, equirect_uv(N)).rgb;

    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): world-space view direction (surface -> camera),
    // built from u_camera_pos_world + v_world_pos — NOT from the eye-space `position` the
    // specular highlight below still uses, see the uniform block comment above for why.
    vec3 view_dir = normalize(u_camera_pos_world - v_world_pos);
    vec3 R = reflect(-view_dir, N);
    float fres = pow(1.0 - max(dot(view_dir, N), 0.0), u_fresnel_power);

    // NEOTKO_REALCOLOR_TAG s243 (F4): reflejo interpolado nítido↔difuminado por rugosidad. Hay que
    // muestrear el irradiance por R (no reutilizar `ambient`, que va por N — otra dirección).
    float roughness = clamp(u_finish.y, 0.0, 1.0);
    vec3 refl = mix(texture2D(u_env_mirror, equirect_uv(R)).rgb,
                    texture2D(u_env_irradiance, equirect_uv(R)).rgb,
                    roughness);
    vec3 rim = u_fresnel_strength * u_finish.x * fres * u_fresnel_tint * refl;

    vec3 base = u_material_rgb[u_tool_id];
    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): diffuse = direct light (achromatic) + env-sampled
    // ambient; lit = env-sampled rim + achromatic specular highlight + base*diffuse. Supersedes
    // PBR item 1's flat-tint mix (see realcolor_peel.vs) — not a superset of it.
    // NEOTKO_REALCOLOR_TAG s243c: la rugosidad entra en la DIFUSA, que es el término dominante
    // (intensity.x llega a ~0.66, frente a 0.075 del especular y 0.05 del rim). Sin esto el
    // acabado era invisible sobre el objeto — verificado por el usuario. Una superficie rugosa
    // dispersa la direccional y gana peso el ambiente; una lisa conserva el modelado direccional.
    // roughness=0 => ambos pesos a 1.0 => idéntico a pre-s243. Ver 140/realcolor_peel.fs.
    float diffuse_directional = 1.0 - 0.60 * roughness;
    float diffuse_ambient     = 1.0 + 0.25 * roughness;
    vec3 diffuse = vec3(intensity.x * diffuse_directional) + ambient * diffuse_ambient;
    vec3 lit = vec3(intensity.y) * u_finish.x + rim + base * diffuse; // s243 (F4): brillo por acabado

    gl_FragData[0] = vec4(lit, 1.0);
    gl_FragData[1] = vec4(float(u_tool_id), u_thickness, v_eye_z, 1.0); // a=1 marks "fragment written"
    // NEOTKO_REALCOLOR_TAG s166 (item 3): 3rd MRT target for SSAO in realcolor_present.fs —
    // packs the view-space normal [-1,1] -> [0,1], same convention as the 140/ variant.
    gl_FragData[2] = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
}
