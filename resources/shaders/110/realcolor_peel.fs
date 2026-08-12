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
uniform int   u_tool_id;        // physical tool (0..REALCOLOR_MAX_TOOLS-1) for this draw, see RenderPath/Path::extruder_id
uniform float u_thickness;      // Path::height for this draw, mm
uniform vec3  u_material_rgb[16]; // NEOTKO_REALCOLOR_TAG s253: 16 = GCodeViewer::REALCOLOR_MAX_TOOLS (era 4; con mas de 4 filamentos esto se leia fuera de rango y salia negro)
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

// NEOTKO_REALCOLOR_TAG s251 (Fase 0, docs/FUTURE/REALCOLOR_ANISOTROPIC_SHEEN_PLAN.md): brillo
// anisótropo. .x = fuerza (0 = neutro exacto), .y = exponente del lóbulo, .z = confianza mínima
// del eje. u_aniso_debug pinta el eje en vez de la escena (herramienta de medida, no efecto).
// Ver 140/realcolor_peel.fs para el razonamiento completo.
uniform vec3 u_aniso;
uniform float u_aniso_debug;

// NEOTKO_REALCOLOR_TAG s251d: AISLADOR DE TÉRMINOS. 0 = imagen normal. Ver 140/realcolor_peel.fs.
uniform int u_light_solo;

// x = direct-light diffuse only, y = specular; matches realcolor_peel.vs
varying vec2 intensity;
varying float v_eye_z; // linear eye-space depth, matches realcolor_peel.vs
varying vec3 v_view_normal; // NEOTKO_REALCOLOR_TAG s166 (item 3): view-space normal, for SSAO in present
varying vec3 v_world_pos; // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): world-space position, see realcolor_peel.vs
varying vec4 v_bead_tangent; // NEOTKO_REALCOLOR_TAG s251: eje del cordón (xyz) + confianza (w)

const float REALCOLOR_PI = 3.14159265359;

// NEOTKO_PHOTOMODE_TAG s253 (P0): la constante pasó a uniform — misma dirección que
// u_rc_light_key_dir en realcolor_peel.vs. Ver la nota larga en 140/realcolor_peel.fs.
// ⚠️ Es válida en MUNDO porque view_normal_matrix se manda como IDENTIDAD desde
// render_toolpaths_realcolor() — el nombre v_view_normal es herencia, el contenido es mundo.
uniform vec3 u_rc_light_key_dir;

// ---------------------------------------------------------------------------------------------
// NEOTKO_PHOTOMODE_TAG s253 — mapa de sombras en RealColor. Gemelo de perfil legacy. El porqué
// completo —de dónde sale el mapa (las shells del objeto, invisibles, como ocluyente), por qué la
// sombra de contacto en pantalla no valía para esto, y por qué el sesgo por normal tiene que
// contar con el relieve del cordón— está en 140/realcolor_peel.fs.
// ---------------------------------------------------------------------------------------------
uniform sampler2D u_shadow_map;
uniform mat4  u_shadow_proj_view;      // mundo -> clip de la luz
uniform bool  u_shadow_enabled;        // false => visibilidad 1.0 y bit-idéntico a s252
uniform float u_shadow_strength;       // 0 = sin sombra, 1 = sombra plena
uniform vec2  u_shadow_texel;          // 1 / resolución del mapa
uniform float u_shadow_normal_bias_mm; // desplazamiento a lo largo de la normal, en mm de mundo

float realcolor_shadow_tap(vec2 uv_s, float r)
{
    // 1.0 = iluminado, 0.0 = ocluido. Fuera del mapa se lee SIEMPRE como iluminado: inventar sombra
    // fuera del frustum de la luz es cómo aparecen bordes rectos donde no hay nada.
    return (r > texture2D(u_shadow_map, uv_s).r) ? 0.0 : 1.0;
}

float realcolor_shadow_visibility()
{
    if (!u_shadow_enabled || u_shadow_strength <= 0.0)
        return 1.0;

    // Desplazamiento por normal ANTES de proyectar: mueve el punto de muestreo fuera de su propia
    // superficie, que es lo que evita el acné sin despegar la sombra del objeto (lo que sí haría un
    // sesgo puro en profundidad). v_view_normal es de MUNDO aquí, igual que v_world_pos.
    vec3 n = normalize(v_view_normal);
    vec4 clip = u_shadow_proj_view * vec4(v_world_pos + n * u_shadow_normal_bias_mm, 1.0);
    if (clip.w <= 0.0)
        return 1.0;

    vec3 proj = clip.xyz / clip.w;
    vec2 uv_s = proj.xy * 0.5 + 0.5;
    if (uv_s.x < 0.0 || uv_s.x > 1.0 || uv_s.y < 0.0 || uv_s.y > 1.0)
        return 1.0;
    float r = proj.z * 0.5 + 0.5;
    if (r > 1.0)
        return 1.0;

    // PCF 3x3, desenrollado a mano igual que el kernel de AO de este proyecto y por el mismo motivo
    // (el gemelo 110 no tiene constructores de array).
    float s = 0.0;
    s += realcolor_shadow_tap(uv_s + vec2(-1.0, -1.0) * u_shadow_texel, r);
    s += realcolor_shadow_tap(uv_s + vec2( 0.0, -1.0) * u_shadow_texel, r);
    s += realcolor_shadow_tap(uv_s + vec2( 1.0, -1.0) * u_shadow_texel, r);
    s += realcolor_shadow_tap(uv_s + vec2(-1.0,  0.0) * u_shadow_texel, r);
    s += realcolor_shadow_tap(uv_s,                                    r);
    s += realcolor_shadow_tap(uv_s + vec2( 1.0,  0.0) * u_shadow_texel, r);
    s += realcolor_shadow_tap(uv_s + vec2(-1.0,  1.0) * u_shadow_texel, r);
    s += realcolor_shadow_tap(uv_s + vec2( 0.0,  1.0) * u_shadow_texel, r);
    s += realcolor_shadow_tap(uv_s + vec2( 1.0,  1.0) * u_shadow_texel, r);
    return mix(1.0, s / 9.0, u_shadow_strength);
}

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

// NEOTKO_REALCOLOR_TAG s251 (Fase 0): lóbulo especular anisótropo de Kajiya-Kay, el modelo
// estándar de pelo/fibra. Para un haz de cilindros paralelos la normal no es única: hay un ANILLO
// entero de normales alrededor del eje, así que el reflejo no es un punto sino una FRANJA
// perpendicular al eje. Kajiya-Kay resuelve ese anillo en forma cerrada usando sólo el eje T, sin
// necesidad de una normal por fibra — que es exactamente lo que aquí no tenemos.
//
// La expresión es el coseno del ángulo entre la dirección reflejada ideal y la vista, escrito con
// senos y cosenos respecto de T: sin(T,L)·sin(T,V) − cos(T,L)·cos(T,V). Sólo entran los productos
// escalares con T, y siempre en combinaciones simétricas, así que el SIGNO de T no llega al
// resultado — que es la razón por la que basta con un EJE y no hace falta una dirección orientada.
float realcolor_aniso_lobe(vec3 T, vec3 L, vec3 V, float exponent)
{
    float t_l = dot(T, L);
    float t_v = dot(T, V);
    float sin_l = sqrt(max(1.0 - t_l * t_l, 0.0));
    float sin_v = sqrt(max(1.0 - t_v * t_v, 0.0));
    return pow(max(sin_l * sin_v - t_l * t_v, 0.0), exponent);
}

// NEOTKO_REALCOLOR_TAG s251: matiz a partir de un ángulo, sólo para el visor de diagnóstico.
vec3 realcolor_hue(float h)
{
    vec3 p = abs(fract(vec3(h) + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return clamp(p - 1.0, 0.0, 1.0);
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

// NEOTKO_REALCOLOR_TAG s253: el indice se ACOTA antes de indexar. Leer fuera de un array en
// GLSL no es un error, es comportamiento indefinido — devuelve basura o ceros, y eso fue
// exactamente el bug de "los filamentos 5+ salen negros". La tabla ya llega llena hasta
// REALCOLOR_MAX_TOOLS, asi que esto no deberia dispararse nunca; existe para que el dia que
// alguien cargue un gcode con mas tools de los previstos el fallo sea un color repetido y no
// una pantalla negra sin explicacion.
    // NEOTKO_REALCOLOR_TAG s253: acotado a mano con dos `if`, NO con clamp().
    // 🚨 clamp()/min()/max() SOLO tienen version para FLOAT en GLSL 1.10 — las sobrecargas de
    // enteros llegaron en la 1.30. Escribir clamp(int,int,int) compila en el gemelo 140 y
    // REVIENTA en el 110, que es justo el que corre macOS. Costo un build: el dialogo decia
    // "Unable to load shaders: realcolor_peel/accum/present", los tres exactamente donde
    // estaba este clamp. Regla: en estos shaders, cualquier operacion sobre enteros se
    // escribe a mano.
    int tool_idx = u_tool_id;
    if (tool_idx < 0)  tool_idx = 0;
    if (tool_idx > 15) tool_idx = 15;
    vec3 base = u_material_rgb[tool_idx];
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

    // NEOTKO_PHOTOMODE_TAG s253: la sombra multiplica sólo los términos DIRECTOS (ver 140/).
    float shadow_vis = realcolor_shadow_visibility();

    vec3 diffuse = vec3(intensity.x * diffuse_directional * shadow_vis) + ambient * diffuse_ambient;
    vec3 lit = vec3(intensity.y) * u_finish.x * shadow_vis + rim + base * diffuse; // s243 (F4): brillo por acabado

    // NEOTKO_REALCOLOR_TAG s251 (Fase 0): BRILLO ANISÓTROPO. Ver 140/realcolor_peel.fs para el
    // razonamiento largo; el resumen es que se SUMA un lóbulo propio en vez de modular la difusa,
    // porque la difusa se calcula por vértice (§2.2 del plan) y bajarla al fragment era la fase
    // cara. Con u_aniso.x = 0 esto es exactamente cero y la imagen es bit-idéntica a s249b.
    float bead_conf = v_bead_tangent.w;
    float bead_len = length(v_bead_tangent.xyz);
    vec3 T = (bead_len > 1e-4) ? (v_bead_tangent.xyz / bead_len) : vec3(0.0);
    float aniso_term = 0.0; // s251d: guardado aparte para poder aislarlo

    if (u_aniso.x > 0.0 && bead_conf > 0.0 && bead_len > 1e-4) {
        // La confianza entra con un FUNDIDO, no con un salto: un umbral duro pondría un borde
        // visible entre paths casi idénticos, y el efecto pasaría a delatar la estructura de datos
        // en vez de la geometría. Los flancos llegan aquí con confianza 1 y no se enteran de esto.
        float gate = smoothstep(u_aniso.z, u_aniso.z + 0.15, bead_conf);
        float lobe = realcolor_aniso_lobe(T, u_rc_light_key_dir, view_dir, max(u_aniso.y, 1.0));
        // Lo escala el mismo brillo del acabado que ya escala especular y rim (u_finish.x): una
        // primera capa aplastada y un soporte mal formado no pueden tener el mismo satinado.
        // Una superficie rugosa además dispersa: pierde franja. Es el mismo reparto de s243c.
        // s253: el lóbulo también se apaga en sombra — ver 140/.
        aniso_term = u_aniso.x * gate * lobe * u_finish.x * (1.0 - 0.75 * roughness) * shadow_vis;
        lit += vec3(aniso_term);
    }

    // NEOTKO_REALCOLOR_TAG s251d: AISLADOR. Sustituye la imagen por UN SOLO término. Ver la nota
    // larga en 140/realcolor_peel.fs — resumen: sirve para responder "¿de dónde sale esta luz?"
    // con una medida en vez de con una hipótesis.
    if (u_light_solo == 1)      lit = base * vec3(intensity.x * diffuse_directional);
    else if (u_light_solo == 2) lit = base * ambient * diffuse_ambient;
    else if (u_light_solo == 3) lit = vec3(intensity.y) * u_finish.x;
    else if (u_light_solo == 4) lit = rim;
    else if (u_light_solo == 5) lit = vec3(aniso_term);
    else if (u_light_solo == 6) lit = base;
    else if (u_light_solo == 7) lit = vec3(0.0);

    // Visor de diagnóstico (u_aniso_debug). Matiz = ángulo del eje con periodo 180° (es un EJE:
    // 0° y 180° son el mismo), saturación = confianza. Un top monotónico tiene que salir de un
    // color plano y uniforme; un perímetro externo, GRIS.
    //
    // 🚨 VA FUERA DEL if DE ARRIBA A PROPÓSITO. El caso que más interesa comprobar es justo el de
    // confianza 0 (el perímetro), y dejarlo dentro del guard lo pintaría con su color normal en vez
    // de gris: el control de la medida se perdería justo donde hay que mirarlo.
    // ⚠️ Lo que se ve es el composite de varias pasadas de peel, así que en zonas translúcidas el
    // color se mezcla con el de detrás — para leerlo, mirar una cara opaca de cerca, no la silueta.
    if (u_aniso_debug > 0.0) {
        float ang = atan(T.y, T.x) / REALCOLOR_PI; // -1..1, periodo 180° tras el fract()
        vec3 dbg = mix(vec3(0.5), realcolor_hue(fract(ang)), bead_conf);
        lit = mix(lit, dbg, clamp(u_aniso_debug, 0.0, 1.0));
    }

    gl_FragData[0] = vec4(lit, 1.0);
    gl_FragData[1] = vec4(float(u_tool_id), u_thickness, v_eye_z, 1.0); // a=1 marks "fragment written"
    // NEOTKO_REALCOLOR_TAG s166 (item 3): 3rd MRT target for SSAO in realcolor_present.fs —
    // packs the view-space normal [-1,1] -> [0,1], same convention as the 140/ variant.
    gl_FragData[2] = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
}
