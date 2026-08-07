#version 140
#extension GL_ARB_explicit_attrib_location : require

// NEOTKO_REALCOLOR_TAG: one pass of front-to-back depth peeling over the SAME toolpath
// geometry render_toolpaths() already draws (see render_toolpaths_realcolor() in
// GCodeViewer.cpp). Pass i keeps only fragments strictly farther than pass i-1's depth,
// so each pass extracts the next visible layer. Writes color (attachment 0),
// (tool_id, thickness, -, written-flag) meta (attachment 1) for the accumulate pass, and
// (s166) view-space normal (attachment 2) consumed only by realcolor_present.fs's SSAO —
// the accumulate pass ignores attachment 2 entirely.

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

// NEOTKO_REALCOLOR_TAG s243 (F4 = A4+A5 de docs/WIP/REALCOLOR_PSEUDOREALISTIC.md): acabado por
// tipo de superficie. .x = brillo (multiplica el especular directo y la fuerza del rim), .y =
// rugosidad 0..1. Se manda por sub-draw desde el bucle de peel, en el MISMO sitio donde ya se
// manda u_thickness — o sea con el `role` del Path que originó ese sub-draw, no el del primero
// del lote (ver el comentario de path_ids[] en render_toolpaths_realcolor: un RenderPath agrupa
// por color y puede mezclar roles, igual que ya mezclaba alturas).
//
// La rugosidad no toca ningún exponente: elige DE QUÉ MAPA sale el reflejo. Ya existen los dos
// horneados — u_env_mirror (nítido) y u_env_irradiance (pre-difuminado) — así que una superficie
// rugosa muestrea el difuminado y una pulida el nítido, que es exactamente lo que hace un
// prefiltrado por rugosidad de PBR de verdad, pero con cero texturas nuevas y cero pasadas.
// Un stTop monotónico y un bridge dejan de reflejar igual sin añadir un solo LOD.
uniform vec2 u_finish;

// x = direct-light diffuse only, y = specular; matches realcolor_peel.vs
in vec2 intensity;
in float v_eye_z; // linear eye-space depth, matches realcolor_peel.vs
in vec3 v_view_normal; // NEOTKO_REALCOLOR_TAG s166 (item 3): view-space normal, for SSAO in present
in vec3 v_world_pos; // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): world-space position, see realcolor_peel.vs
in vec4 v_bead_tangent; // NEOTKO_REALCOLOR_TAG s251: eje del cordón (xyz) + confianza (w), ver realcolor_peel.vs

// NEOTKO_REALCOLOR_TAG s251 (Fase 0, docs/FUTURE/REALCOLOR_ANISOTROPIC_SHEEN_PLAN.md): BRILLO
// ANISÓTROPO DEL CORDÓN. .x = fuerza (0 = neutro EXACTO, imagen bit-idéntica a s249b), .y =
// exponente del lóbulo, .z = confianza mínima del eje para aplicarlo.
//
// LA TESIS. Una superficie FDM no es lisa: es un haz de cilindros paralelos. Ópticamente eso se
// comporta como el pelo, la seda o el metal cepillado — el reflejo no es una mancha redonda, es una
// FRANJA perpendicular a los cordones. Es la razón de que una pieza impresa cambie tanto de aspecto
// al girarla bajo una lámpara, y de que un top planchado a 45° no brille igual que el mismo top a
// 0°. RealColor trataba hasta ahora cada cordón como un prisma de caras planas con reflejo isótropo.
//
// 🚨 POR QUÉ ES UN TÉRMINO ADITIVO NUEVO Y NO UNA MODULACIÓN DE LO QUE YA HAY. §2.2 del plan avisa
// de que la difusa de RealColor se calcula POR VÉRTICE (intensity.x, en realcolor_peel.vs) y llega
// aquí ya interpolada; la normal por fragmento sólo alimenta entorno, rim y el MRT del SSAO. O sea
// que cualquier efecto que se cuele modulando lo que hay tocaría únicamente términos residuales
// (especular 0.075, rim 0.05, frente a una difusa de ~0.66) — y la lección de s243c es tajante: un
// parámetro que sólo toca residuales NO EXISTE. La salida no es bajar la difusa al fragment (la
// fase cara y arriesgada, con los dos gemelos de shader por medio): es que este lóbulo traiga su
// propia energía y su propio mando. Si la Fase 0 convence, esa obra ya estará justificada por un
// resultado visto; si no convence, nos la hemos ahorrado entera.
uniform vec3 u_aniso;

// NEOTKO_REALCOLOR_TAG s251: visor de diagnóstico del eje (0 = apagado). NO es un efecto: es LA
// MEDIDA que el plan pedía hacer antes de decidir. Ver el bloque en main().
uniform float u_aniso_debug;

// NEOTKO_REALCOLOR_TAG s251d — AISLADOR DE TÉRMINOS. 0 = imagen normal (neutro exacto).
//
// POR QUÉ EXISTE. El usuario reportó una luz en los contornos que no podía apagar con NINGÚN
// mando. s251c le dio mandos a la luz directa, que era la causa que yo tenía identificada... y
// seguía habiendo residuo: con luz principal 0.010, relleno 0.010, especular 0.009, ambiente ~0 y
// tints en negro, la aritmética dice que `lit` no puede pasar de ~0.025 y la imagen debería salir
// casi negra. No lo está. O sea que hay un término que el análisis sobre el papel no ve.
//
// 🔑 Y LA LECCIÓN DE POR QUÉ ESTO Y NO "RANGOS MÁS AMPLIOS PARA PROBAR", que era lo que se pedía:
// un slider con rango exagerado te dice que algo baja, no QUÉ es. Cuando ya has fallado una
// hipótesis (en s251d fallé una: creía que el .app cargaba shaders viejos, y no — Resources es un
// symlink a resources/, sin paso de copia), lo que hace falta es una MEDIDA que parta el problema,
// no otro mando que lo tantee. Cada iteración cuesta una recompilación del usuario.
//
// Modos, y qué prueba cada uno:
//   0 — normal.
//   1 — SÓLO base × difusa direccional. Si la luz misteriosa aparece aquí, es intensity.x, o sea
//       u_light_key/u_light_fill, y entonces el bug es que el uniform no llega con el valor que
//       marca el slider.
//   2 — SÓLO base × ambiente del entorno. Si aparece aquí, el horneado de la textura de entorno no
//       está respetando ambient_ground/sky o sus tints.
//   3 — SÓLO especular directo.   4 — SÓLO rim/fresnel.   5 — SÓLO el lóbulo anisótropo.
//   6 — SÓLO el color base plano, sin ninguna luz. Es el control: comprueba que lo que se ve es
//       geometría con su color y no otra cosa.
//   7 — NEGRO. **El modo más informativo de todos**: si con lit = vec3(0.0) sigue viéndose algo,
//       entonces eso NO viene del peel, y hay que buscarlo en el accum, en el present, o en que se
//       esté viendo el render normal de toolpaths por debajo del composite. Descarta media tubería
//       de un solo clic.
uniform int u_light_solo;

const float REALCOLOR_PI = 3.14159265359;

// NEOTKO_REALCOLOR_TAG s251: MISMA dirección de luz que LIGHT_TOP_DIR en realcolor_peel.vs. Se
// duplica la constante en vez de pasarla por varying porque en los dos ficheros es una constante de
// compilación; si algún día pasa a uniform, hay que tocar los dos (y los dos gemelos).
// ⚠️ Vale en MUNDO porque view_normal_matrix se manda como IDENTIDAD desde
// render_toolpaths_realcolor() — el nombre v_view_normal es herencia histórica, el contenido es
// espacio de mundo, igual que lo asume el muestreo del entorno de s243 (F3).
const vec3 ANISO_LIGHT_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);

// NEOTKO_REALCOLOR_TAG s214: same convention as the CPU-side generator (realcolor_env_sample()
// in GCodeViewer.cpp) — v=0 is straight up (sky), v=1 is straight down (ground), matching the
// dir.y-based sky_mix already established by PBR item 1b.
// ✅ NEOTKO_REALCOLOR_TAG s243 (F3) — EJE ARRIBA, RESUELTO. Cierra la nota que dejó s242 aquí.
//
// El diagnóstico de s242 era correcto: esta función usaba dir.y como "arriba" y
// realcolor_env_sample() (GCodeViewer.cpp) hornea con la misma convención, o sea que eran
// coherentes ENTRE SÍ pero respecto a un "arriba" que no es el del mundo. render_toolpaths_realcolor
// pasa view_normal_matrix = Identity(), así que los vectores que llegan aquí están en espacio
// MUNDO, y el mundo de este proyecto tiene la cama en XY con Z arriba. Resultado: el cielo del IBL
// caía hacia +Y mundo en vez de +Z — el entorno entero girado 90°, ambiente y reflejos viniendo del
// sitio equivocado. No daba error, sólo mentía.
//
// EL ARREGLO va SÓLO aquí, no en el generador. realcolor_env_sample() no trabaja con direcciones:
// recibe (u,v) y decide qué color tiene esa celda de la equirectangular, con el contrato "v=0 es
// arriba del todo (cielo), v=1 abajo del todo (suelo)". Ese contrato es correcto y no se toca. Lo
// que estaba mal era el mapeo dirección → (u,v) de este lado. Ahora:
//   - la latitud (v) sale de dir.z  → el cielo está en +Z mundo, encima de la cama
//   - la longitud (u) sale de atan(dir.y, dir.x) → el giro pasa a ser alrededor de Z
// Con esto coincide con shells_lit.fs :: photo_env_uv(), que ya usaba Z arriba a propósito. Ya no
// divergen: una sola convención de "arriba" en todo el proyecto.
//
// ⚠️ Esto CAMBIA el look de un render existente (el ambiente ya no llega de un lado, llega de
// arriba). Es lo correcto, pero significa que cualquier calibración a ojo hecha antes de s243 está
// hecha contra el entorno girado. Por eso F3 va ANTES que A4/A5/A6 y no después:
// docs/WIP/REALCOLOR_PSEUDOREALISTIC.md.
vec2 equirect_uv(vec3 dir)
{
    dir = normalize(dir);
    float u = atan(dir.y, dir.x) / (2.0 * REALCOLOR_PI) + 0.5;
    float v = 0.5 - asin(clamp(dir.z, -1.0, 1.0)) / REALCOLOR_PI;
    return vec2(u, v);
}

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_meta;
// NEOTKO_REALCOLOR_TAG s166 (item 3): 3rd MRT target for SSAO in realcolor_present.fs — packs
// the view-space normal [-1,1] -> [0,1] (same texture format family, GL_RGBA32F, as out_meta;
// packing rather than a signed format keeps every peel attachment on the same convention).
layout(location = 2) out vec4 out_normal;

// NEOTKO_REALCOLOR_TAG s251 (Fase 0): lóbulo especular anisótropo de Kajiya-Kay, el modelo estándar
// de pelo y fibra.
//
// POR QUÉ ESTE MODELO Y NO UN PHONG NORMAL. En un haz de cilindros paralelos la normal no es única:
// alrededor del eje hay un ANILLO ENTERO de normales válidas. Por eso el reflejo se estira en una
// franja en vez de concentrarse en un punto. Kajiya-Kay resuelve ese anillo en forma cerrada usando
// SÓLO el eje T — que es precisamente lo que aquí tenemos, y no una normal por fibra, que no
// tenemos ni podríamos tener sin geometría nueva.
//
// La expresión es el coseno del ángulo entre la dirección reflejada ideal y la de vista, escrito en
// senos y cosenos respecto de T. Sólo entran productos escalares con T y siempre en combinaciones
// simétricas, así que el SIGNO de T no llega al resultado — que es exactamente la razón por la que
// basta con un EJE y no hace falta una dirección orientada, ni en los flancos ni en el promedio del
// path. Es la misma simetría de orden 2 que hace que el TD del cordón se ajuste como A + B·cos(2θ).
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
        float prevEyeZ = texture(u_prev_meta, uv).b;
        // NEOTKO_REALCOLOR_TAG: slope-scaled bias in LINEAR eye-space depth (mm), not NDC
        // gl_FragCoord.z — NDC depth is nonlinear w.r.t. real distance, so a bias there breaks
        // differently at every zoom level (confirmed: the noisy band moved with zoom). Linear
        // depth VALUES are zoom-invariant, but fwidth(v_eye_z) (a SCREEN-SPACE derivative) is
        // NOT — at low zoom/far camera distance one screen pixel spans more real mm of depth,
        // so fwidth grows with zoom-out too, not only with grazing angle as originally intended
        // here (s164). NEOTKO_REALCOLOR_TAG s166 FIX: left uncapped, at enough zoom-out the bias
        // can exceed the real physical gap between two thin adjacent layers (e.g. a 0.13-0.2mm
        // Sandwich top pass) — this peel pass then wrongly treats the layer it's trying to
        // detect as "already surfaced" and discards it whole, silently dropping that layer's
        // entire contribution from the Beer-Lambert stack (reported by the user: the layer right
        // under the top one vanishes from the color mix specifically when zoomed out, correct
        // again zoomed in — exactly this mechanism, not a refresh/debounce issue). Capped to half
        // of THIS pass's own real thickness (u_thickness, mm) — the largest bias that still can't
        // swallow the very layer being peeled right now. bias_cap itself floored at 2e-3 too, so
        // clamp() never gets minVal>maxVal on a vanishingly thin PathBlend sub-layer.
        float bias_cap = max(u_thickness * 0.5, 2e-3);
        float bias = clamp(fwidth(v_eye_z) * 2.0, 2e-3, bias_cap);
        if (v_eye_z <= prevEyeZ + bias)
            discard; // already surfaced in an earlier peel pass
    }

    vec3 N = normalize(v_view_normal); // world-space, see realcolor_peel.vs
    vec3 ambient = texture(u_env_irradiance, equirect_uv(N)).rgb;

    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): world-space view direction (surface -> camera),
    // built from u_camera_pos_world + v_world_pos — NOT from the eye-space `position` the
    // specular highlight below still uses, see the uniform block comment above for why.
    vec3 view_dir = normalize(u_camera_pos_world - v_world_pos);
    vec3 R = reflect(-view_dir, N);
    float fres = pow(1.0 - max(dot(view_dir, N), 0.0), u_fresnel_power);

    // NEOTKO_REALCOLOR_TAG s243 (F4): el reflejo se interpola entre el mapa nítido y el
    // pre-difuminado según la rugosidad de ESTA superficie. `ambient` de arriba ya es el mapa
    // difuminado muestreado por la normal; aquí hace falta muestrearlo otra vez, pero por R, que
    // es una dirección distinta — reutilizar `ambient` daría un reflejo que no sigue a la cámara.
    float roughness = clamp(u_finish.y, 0.0, 1.0);
    vec3 refl = mix(texture(u_env_mirror, equirect_uv(R)).rgb,
                    texture(u_env_irradiance, equirect_uv(R)).rgb,
                    roughness);
    vec3 rim = u_fresnel_strength * u_finish.x * fres * u_fresnel_tint * refl;

    vec3 base = u_material_rgb[u_tool_id];
    // NEOTKO_REALCOLOR_TAG s214 (PBR item 3): diffuse = direct light (achromatic) + env-sampled
    // ambient; lit = env-sampled rim + achromatic specular highlight + base*diffuse. Supersedes
    // PBR item 1's flat-tint mix (see realcolor_peel.vs) — not a superset of it.
    // s243 (F4): el especular directo lo escala el mismo brillo que el rim, para que primera capa
    // (casi especular, aplastada contra el bed) y soporte (mate, mal formado) no compartan
    // highlight. El NEUTRO de este uniform es (1.0, 0.0): brillo sin tocar y rugosidad cero =
    // reflejo 100% del mapa nítido, que es literalmente lo que hacía el código pre-s243. El knob
    // `finish_strength` del panel interpola todos los acabados hacia ese neutro, así que a 0 la
    // imagen es bit-idéntica a la de antes — condición para poder A/B esto de un tirón.
    // NEOTKO_REALCOLOR_TAG s243c: la rugosidad entra AQUÍ, en la difusa, no sólo en el especular.
    //
    // POR QUÉ (el usuario probó s243b y no veía absolutamente nada): u_finish sólo tocaba
    // intensity.y (LIGHT_TOP_SPECULAR = 0.125*0.6 = 0.075, y encima elevado a la 20 → un punto de
    // brillo minúsculo) y el rim (fresnel_strength = 0.05 por defecto). Escalar por 1.45 algo que
    // vale 0.05 es invisible, y más aún cuando el composite promedia ~18 pasadas de peel. El
    // término que manda en la imagen es intensity.x, que llega a ~0.66 (LIGHT_TOP_DIFFUSE 0.48 +
    // LIGHT_FRONT_DIFFUSE 0.18) — si el acabado no lo toca, el acabado no existe.
    //
    // Y físicamente es lo correcto, no un truco para que se note: una superficie rugosa dispersa
    // la luz direccional en todas direcciones. Pierde contraste direccional (la diferencia entre
    // la cara iluminada y la de sombra se aplana) y gana peso la iluminación ambiente. Una lisa
    // conserva el modelado direccional. Es exactamente la diferencia entre un bridge y un top
    // planchado, y es lo que hace que la pieza deje de parecer un solo material.
    //
    // Con roughness=0 los dos pesos valen 1.0 y esto es IDÉNTICO a pre-s243 — condición para que
    // el knob finish_strength siga sirviendo de A/B limpio.
    float diffuse_directional = 1.0 - 0.60 * roughness;
    float diffuse_ambient     = 1.0 + 0.25 * roughness;
    vec3 diffuse = vec3(intensity.x * diffuse_directional) + ambient * diffuse_ambient;
    vec3 lit = vec3(intensity.y) * u_finish.x + rim + base * diffuse;

    // NEOTKO_REALCOLOR_TAG s251 (Fase 0): BRILLO ANISÓTROPO. Ver el bloque del uniform u_aniso
    // arriba para el porqué de que sea aditivo. Con u_aniso.x = 0 esto suma exactamente cero y la
    // imagen es bit-idéntica a s249b — condición para poder A/B el efecto entero de un tirón, igual
    // que finish_strength y los swell.
    float bead_conf = v_bead_tangent.w;
    float bead_len = length(v_bead_tangent.xyz);
    vec3 T = (bead_len > 1e-4) ? (v_bead_tangent.xyz / bead_len) : vec3(0.0);
    float aniso_term = 0.0; // s251d: guardado aparte para poder aislarlo abajo

    if (u_aniso.x > 0.0 && bead_conf > 0.0 && bead_len > 1e-4) {
        // La confianza entra con un FUNDIDO, no con un salto. Un umbral duro dibujaría un borde
        // visible entre dos paths casi idénticos que caen a los dos lados del corte, y entonces el
        // efecto delataría la estructura de datos en vez de la geometría — que es justo el tipo de
        // artefacto por el que RealColor ya pagó una vez (el bandeo del peel, s164/s166).
        // Los flancos llegan aquí con confianza 1 y no se enteran de este gate.
        float gate = smoothstep(u_aniso.z, u_aniso.z + 0.15, bead_conf);
        float lobe = realcolor_aniso_lobe(T, ANISO_LIGHT_DIR, view_dir, max(u_aniso.y, 1.0));
        // Lo escala el mismo brillo de acabado que ya escala especular y rim (u_finish.x), y lo
        // atenúa la rugosidad: una superficie rugosa dispersa y PIERDE la franja, que es justo la
        // diferencia entre un top planchado y un bridge. Mismo reparto que s243c, no una escala
        // nueva inventada aparte.
        aniso_term = u_aniso.x * gate * lobe * u_finish.x * (1.0 - 0.75 * roughness);
        lit += vec3(aniso_term);
    }

    // NEOTKO_REALCOLOR_TAG s251d: AISLADOR — ver el bloque del uniform u_light_solo arriba para qué
    // prueba cada modo. Va DESPUÉS de todos los términos y antes del visor de eje, para que el
    // aislador gane sobre cualquier cosa que se haya sumado por el camino.
    // ⚠️ Lo que se ve sigue pasando por el accum (que hace srgb_to_linear del color de capa y
    // compone Beer-Lambert sobre N pasadas) y por el present. O sea: esto aísla el término DENTRO
    // del peel, no cortocircuita la tubería. Es lo que se quiere — el modo 7 (negro) es justamente
    // el que distingue "viene del peel" de "viene de más abajo".
    if (u_light_solo == 1)      lit = base * vec3(intensity.x * diffuse_directional);
    else if (u_light_solo == 2) lit = base * ambient * diffuse_ambient;
    else if (u_light_solo == 3) lit = vec3(intensity.y) * u_finish.x;
    else if (u_light_solo == 4) lit = rim;
    else if (u_light_solo == 5) lit = vec3(aniso_term);
    else if (u_light_solo == 6) lit = base;
    else if (u_light_solo == 7) lit = vec3(0.0);

    // Visor de diagnóstico (u_aniso_debug). Matiz = ángulo del eje, con periodo 180° porque es un
    // EJE (0° y 180° son el mismo cordón); saturación = confianza. Lo que hay que ver:
    //   · top solid infill → color PLANO y uniforme, que cambia de matiz al cambiar el ángulo de
    //     relleno. Si sale moteado, el eje no es fiable y el brillo sobra.
    //   · perímetro externo → GRIS. Es un bucle cerrado: no tiene eje dominante y la confianza es 0.
    //   · sandwich → cada pasada del top con SU matiz, que es el objetivo del frente entero.
    //
    // 🚨 VA FUERA DEL if DE ARRIBA A PROPÓSITO. El caso de control más importante es precisamente el
    // de confianza 0; dejarlo dentro del guard lo pintaría con su color normal en vez de gris y se
    // perdería la comprobación justo donde hay que mirarla.
    // ⚠️ Se ve el composite de N pasadas de peel, así que en zonas translúcidas el color se mezcla
    // con el de detrás — leerlo sobre una cara opaca de cerca, no sobre la silueta.
    if (u_aniso_debug > 0.0) {
        float ang = atan(T.y, T.x) / REALCOLOR_PI; // -1..1; el fract() de abajo lo vuelve periódico a 180°
        vec3 dbg = mix(vec3(0.5), realcolor_hue(fract(ang)), bead_conf);
        lit = mix(lit, dbg, clamp(u_aniso_debug, 0.0, 1.0));
    }

    out_color = vec4(lit, 1.0);
    out_meta  = vec4(float(u_tool_id), u_thickness, v_eye_z, 1.0); // a=1 marks "fragment written"
    out_normal = vec4(normalize(v_view_normal) * 0.5 + 0.5, 1.0);
}
