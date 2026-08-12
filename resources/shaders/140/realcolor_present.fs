#version 140

// NEOTKO_REALCOLOR_TAG: presents the cached/just-computed composite into whichever
// framebuffer is bound (the default one, when called from render_toolpaths_realcolor()).
// Writes gl_FragDepth from peel pass 0 (the nearest toolpath surface) so the normal GL
// depth test integrates correctly with shells already drawn by render_shells() — same
// occlusion contract as every other EViewType, just via an explicit depth write instead of
// an implicit one. Discards where peel pass 0 never wrote anything (no toolpath at this
// pixel), letting shells/background show through untouched.
// s166 (item 3): also applies a cheap screen-space AO pass — see compute_ao() below.

uniform sampler2D u_accum_color;   // linear RGB, see realcolor_accum.fs
uniform sampler2D u_peel_meta0;    // pass-0 meta: r=tool_id, g=thickness, b=eye_z (linear mm), a=written
uniform sampler2D u_peel_depth0;   // pass-0 depth
uniform sampler2D u_peel_normal0;  // NEOTKO_REALCOLOR_TAG s166 (item 3): pass-0 packed view-space normal
uniform vec2 u_texel_size;         // 1 / supersampled peel/accum texture resolution
uniform float u_ao_radius;         // px, in the same supersampled texel space as u_texel_size
uniform float u_ao_strength;       // 0 = AO off, 1 = full strength

// NEOTKO_REALCOLOR_TAG s214 (PBR item 2, docs/WIP/REALCOLOR_VIEW/08_PBR_IBL_SSS_PLAN.md):
// single-pass screen-space subsurface scattering — same material TD array realcolor_accum.fs
// already indexes dynamically by tool_id (proven to work on this hardware since s163), reused
// here to scale the blur radius per-pixel by how translucent the surface under it is.
uniform float u_material_td[16];
uniform float u_sss_strength;      // 0 = off (bit-identical to pre-s214), 1 = full blur
uniform float u_sss_radius_px;     // px, same supersampled texel space as u_ao_radius
uniform float u_sss_reference_td;  // mm — a material at exactly this TD blurs at u_sss_radius_px

// NEOTKO_REALCOLOR_TAG s243 (F5 = A7 de docs/WIP/REALCOLOR_PSEUDOREALISTIC.md, "el honesto y el
// más barato"): buena parte de la sensación de "render de CAD" no viene de la geometría ni del
// shading, viene de colores demasiado saturados y de un rango dinámico demasiado abierto. Una
// foto real de una pieza impresa tiene los negros levantados y los colores un punto más apagados
// que el hex del filamento.
// u_grade_contrast = 1.0 y u_grade_saturation = 1.0 dejan la imagen EXACTAMENTE como antes de
// s243 — el gradeo entero se apaga poniendo los dos a 1.
uniform float u_grade_contrast;    // <1 comprime hacia el gris medio, >1 abre
uniform float u_grade_saturation;  // <1 desatura hacia la luminancia

// NEOTKO_REALCOLOR_TAG s243 (F6, "silueta opaca"): 0 = apagado (≡ pre-F6), 1 = sella del todo los
// huecos INTERIORES. Ver compute_interior() más abajo.
uniform float u_fill_interior;

// NEOTKO_PHOTOMODE_TAG s253: sombras de contacto — ver compute_contact_shadow() más abajo.
// u_proj/u_inv_proj son la proyección de la cámara y su inversa: este pase es un quad a pantalla
// completa y no recibe posición de vista, así que hay que reconstruirla (y la cámara puede ser
// ortográfica, de ahí el par completo en vez de la fórmula corta de perspectiva).
uniform mat4  u_proj;
uniform mat4  u_inv_proj;
uniform vec3  u_sscs_light_dir_view;  // luz PRINCIPAL, en espacio de VISTA y unitaria (la marcha)
uniform vec3  u_sscs_light_dir_world; // la MISMA luz en MUNDO (la puerta por N·L, ver abajo)
uniform float u_sscs_strength;        // 0 = apagado y bit-idéntico a s252
uniform float u_sscs_length_mm;       // cuánto se marcha hacia la luz, en mm de mundo
uniform float u_sscs_thickness_mm;    // un ocluyente más grueso que esto es pared, no contacto

in vec2 uv;
out vec4 frag_color;

float linear_to_srgb(float c) { c = clamp(c, 0.0, 1.0); return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// NEOTKO_REALCOLOR_TAG s166 (item 3): cheap screen-space AO, adapted (not ported) from the
// classic depth-buffer-only SSAO (Kajalin, "Screen-Space Ambient Occlusion", ShaderX7/GDC 2007,
// as later shipped in Crysis) — a fixed 8-tap disc around the pixel, comparing each neighbor's
// LINEAR eye-space depth (peel_meta0.b, already computed for the peel-order bias fix, see
// realcolor_peel.fs) against the center's. A neighbor sitting closer to the camera than the
// center indicates a nearby wall/crevice occluding it; contribution falls off past
// REALCOLOR_AO_MAX_DELTA_MM so unrelated far/near geometry (a different silhouette edge, not a
// local crease) doesn't darken the pixel. No view-space position reconstruction (no inverse-
// projection matrix here — this is a screen-space fullscreen quad, see realcolor_quad.vs), so
// this is 2D-disc, not a true 3D hemisphere kernel like Alchemy AO/HBAO — this project's own
// addition on top of the classic technique: weight each sample by how well its normal (now
// available via peel_normal_tex) agrees with the center's, since a neighbor whose surface faces
// a similar direction is a more plausible actual occluder of the SAME local crevice than one
// with a wildly different normal (more likely to be unrelated silhouette geometry poking
// through a gap) — cuts down on false-positive dark halos across silhouette edges.
// NEOTKO_REALCOLOR_TAG: 8 samples unrolled by hand (no array-constructor syntax, e.g.
// `vec2[](...)`, which is GLSL 1.20+ only) instead of a const array — keeps this identical to
// the 110/ variant, which doesn't have array constructors at all.
const float REALCOLOR_AO_MAX_DELTA_MM = 1.5;

float ao_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir)
{
    vec2 s_uv = center_uv + dir * u_texel_size * u_ao_radius;
    vec4 s_meta = texture(u_peel_meta0, s_uv);
    if (s_meta.a < 0.5)
        return 0.0; // background — doesn't occlude
    float delta = center_z - s_meta.b; // positive: neighbor is CLOSER to camera than center
    if (delta <= 0.0)
        return 0.0; // neighbor is farther away or coplanar — not an occluder of this pixel
    float range_falloff = clamp(1.0 - delta / REALCOLOR_AO_MAX_DELTA_MM, 0.0, 1.0);
    vec3 s_n = normalize(texture(u_peel_normal0, s_uv).rgb * 2.0 - 1.0);
    float normal_agreement = max(dot(center_n, s_n), 0.0);
    return range_falloff * normal_agreement;
}

// ---------------------------------------------------------------------------------------------
// NEOTKO_PHOTOMODE_TAG s253 — SOMBRAS DE CONTACTO EN REALCOLOR (screen-space contact shadows).
//
// EL REPORTE QUE LO TRAJO: el usuario echó en falta la sombra de un texto sobre la cara en la que
// está grabado. En Prepare esa sombra existe (shells_lit.fs, sscs_occlusion(), s229); en RealColor
// no había NADA direccional, sólo la oclusión ambiental de compute_ao() de aquí arriba. Y eso es
// justo lo que se veía: el texto queda hundido, pero no iluminado desde ningún sitio, porque el AO
// oscurece rincones sin saber dónde está la luz.
//
// 🔑 POR QUÉ SE PUEDE HACER AQUÍ Y ES BARATO: las dos entradas que hacen falta —profundidad lineal
// por píxel (u_peel_meta0.b, en mm) y normal (u_peel_normal0)— YA se calculan y ya se muestrean,
// porque el AO las necesita. No hay buffers nuevos, ni pasadas nuevas, ni geometría que volver a
// dibujar. Es el mismo truco del AO apuntado a lo largo de la luz en vez de en un anillo.
//
// ⚠️ Y ES DELIBERADAMENTE LA SOMBRA CORTA, NO LA LARGA. Esto resuelve objeto-sobre-sí-mismo (texto,
// grabados, voladizos): el ocluyente tiene que estar EN PANTALLA y no tapado por otra cosa, y el
// alcance es corto por construcción. La sombra larga de la pieza sobre el suelo es otro problema y
// necesita un shadow map — que en RealColor no se puede llenar porque los toolpaths no son
// GLVolume. Ver §4.B de docs/WIP/PHOTO_MODE_PORT_TO_REALCOLOR_PLAN.md. No confundir las dos:
// intentar estirar ésta hasta hacer de aquélla sólo produce artefactos de borde de pantalla.
//
// A diferencia de shells_lit, aquí NO hay posición de vista por fragmento (esto es un quad a
// pantalla completa, ver realcolor_quad.vs), así que hay que reconstruirla. Se hace con el par
// proyección/inversa en vez de con la fórmula corta de perspectiva a propósito: la cámara de este
// programa puede ser ORTOGRÁFICA, y la fórmula corta (view_x = ndc.x * eye_z / P00) sólo vale en
// perspectiva — en ortográfica daría una marcha que se estrecha con la profundidad y sombras que se
// desplazan al hacer zoom, que es un síntoma difícil de leer.
//
// El resto de constantes replican shells_lit (s229) salvo el ALCANCE, que aquí es mucho más corto:
// allí 6 mm barren una pieza entera vista de lejos; aquí lo que se persigue mide décimas de mm (el
// escalón de un texto grabado), y un alcance largo sobre un campo de cordones sólo mete ruido.
// El reparto sigue EXACTAMENTE el patrón que ya usa el AO de este mismo fichero, no uno nuevo: lo
// que puede querer moverse llega por uniform desde C++ (donde vive como `static constexpr`, junto a
// REALCOLOR_AO_RADIUS_PX/STRENGTH), y lo que es estructural se queda aquí como constante. No hay
// slider en ningún panel: no es un ajuste de usuario, y el panel de tuning es la verdad del color,
// no el aspecto (decisión del usuario, s253).
#define REALCOLOR_SSCS_STEPS 12

// 🔑 EL SESGO PROPIO ES GRANDE A PROPÓSITO — ES "IGNORA EL CORDÓN DE AL LADO".
//
// En shells_lit este número vale 0.08 mm porque allí la superficie es una malla lisa y lo único que
// hay que descartar es el propio píxel. Aquí la superficie es un CAMPO DE MEDIOS CILINDROS: cada
// cordón sobresale unas décimas sobre su vecino. Con un sesgo pequeño, la marcha hacia una luz
// rasante choca SIEMPRE con la cresta de al lado y la pieza entera se auto-sombrea — no una sombra,
// una mancha con un borde recto que se mueve al girar la cámara.
//
// Es la misma propiedad de esta geometría que ya derrotó al SSS de pantalla en s251c (ver la nota
// de u_light_wrap en realcolor_peel.vs): **la corrugación rechaza cualquier prueba que compare con
// el vecino inmediato**. La respuesta aquí es la simétrica: sólo cuenta como ocluyente lo que
// sobresale MÁS que un cordón. Una letra grabada (medio milímetro largo) pasa el filtro; la cresta
// de la extrusión de al lado, no.
const float REALCOLOR_SSCS_SELF_BIAS_MM = 0.25;

// Y por debajo de este N·L no se marcha en absoluto. Con la luz casi paralela a la superficie el
// rayo avanza casi sin ganar altura, así que cualquier microrrelieve entra en la ventana de sombra
// y el resultado es ruido en toda la cara. Cortar ahí es más barato y más honesto que intentar
// afinar el sesgo para que aguante el caso rasante.
const float REALCOLOR_SSCS_MIN_NDL = 0.15;

// Reconstruye la posición de VISTA de un píxel a partir de su uv y su profundidad lineal en mm.
// Vale para perspectiva y para ortográfica: se toma el segmento del rayo que atraviesa el frustum
// en ese uv y se busca el punto cuya eye_z es la pedida.
vec3 realcolor_view_pos(vec2 uv_in, float eye_z)
{
    vec2 ndc = uv_in * 2.0 - 1.0;
    vec4 a = u_inv_proj * vec4(ndc, -1.0, 1.0);
    vec4 b = u_inv_proj * vec4(ndc,  1.0, 1.0);
    vec3 pa = a.xyz / a.w;
    vec3 pb = b.xyz / b.w;
    vec3 d  = pb - pa;
    // eye_z = -view_z (mano derecha, la cámara mira hacia -Z), igual que en realcolor_peel.vs.
    if (abs(d.z) < 1e-9)
        return vec3(pa.xy, -eye_z);
    return pa + d * ((-eye_z - pa.z) / d.z);
}

float compute_contact_shadow(vec2 center_uv, float center_z)
{
    if (u_sscs_strength <= 0.0 || u_sscs_length_mm <= 0.0)
        return 0.0;

    // Puerta por orientación, en espacio de MUNDO: la normal que guarda el peel es de mundo (el
    // nombre v_view_normal es herencia histórica, ver realcolor_peel.fs) y por eso la luz se recibe
    // aquí también en mundo, además de en vista para la marcha. Mezclar los dos espacios en este
    // producto escalar daría una puerta que se abre y se cierra al orbitar — un fallo que parece
    // parpadeo de sombra y se diagnostica fatal.
    vec3 n_world = normalize(texture(u_peel_normal0, center_uv).rgb * 2.0 - 1.0);
    float ndl = dot(n_world, normalize(u_sscs_light_dir_world));
    if (ndl < REALCOLOR_SSCS_MIN_NDL)
        return 0.0;

    vec3 ro = realcolor_view_pos(center_uv, center_z);
    vec3 rd = normalize(u_sscs_light_dir_view);
    float step_mm = u_sscs_length_mm / float(REALCOLOR_SSCS_STEPS);
    // Jitter del primer paso por píxel: un paso fijo produce bandas visibles en superficies suaves.
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float t = step_mm * (0.5 + 0.5 * jitter);

    for (int i = 0; i < REALCOLOR_SSCS_STEPS; ++i) {
        vec3 p = ro + rd * t;
        vec4 clip = u_proj * vec4(p, 1.0);
        if (clip.w <= 0.0)
            break; // detrás del ojo, no queda nada que muestrear
        vec2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
            break; // se salió de pantalla — ahí no hay información
        vec4 s_meta = texture(u_peel_meta0, suv);
        if (s_meta.a >= 0.5) {
            float delta = (-p.z) - s_meta.b;
            if (delta > REALCOLOR_SSCS_SELF_BIAS_MM && delta < u_sscs_thickness_mm)
                // Cuanto más cerca el impacto, más oscuro: una sombra de contacto es más cerrada
                // justo en el contacto.
                return 1.0 - t / u_sscs_length_mm;
        }
        t += step_mm;
    }
    return 0.0;
}

float compute_ao(vec2 center_uv)
{
    vec4 center_meta = texture(u_peel_meta0, center_uv);
    if (center_meta.a < 0.5) // center tap missed geometry (can happen right at a thin edge,
        return 1.0;          // even though the box filter's 4 taps found coverage) — skip, no darkening
    float center_z = center_meta.b;
    vec3 center_n = normalize(texture(u_peel_normal0, center_uv).rgb * 2.0 - 1.0);

    float occlusion = 0.0;
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 1.0,  0.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2(-1.0,  0.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.0,  1.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.0, -1.0));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.7,  0.7));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2(-0.7,  0.7));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2( 0.7, -0.7));
    occlusion += ao_sample(center_uv, center_z, center_n, vec2(-0.7, -0.7));
    occlusion /= 8.0;
    return 1.0 - clamp(occlusion, 0.0, 1.0);
}

// NEOTKO_REALCOLOR_TAG s214 (PBR item 2): single-pass screen-space subsurface scattering —
// same 8-tap disc pattern as compute_ao() above, adapted to accumulate blurred NEIGHBOR COLOR
// (from u_accum_color) instead of occlusion, with the same bilateral edge-stopping (depth delta
// + normal agreement) so the blur never bleeds color across a real silhouette edge. Deliberately
// NOT a separable 2-pass Gaussian (the "textbook" SSS technique) — see 08_PBR_IBL_SSS_PLAN.md for
// why a single disc pass was chosen (reuses these exact bound textures, zero new FBOs/passes).
const float REALCOLOR_SSS_MAX_DELTA_MM = 1.5; // same silhouette-edge cutoff as REALCOLOR_AO_MAX_DELTA_MM

void sss_sample(vec2 center_uv, float center_z, vec3 center_n, vec2 dir, float radius_px,
                 inout vec3 accum_color, inout float accum_weight)
{
    vec2 s_uv = center_uv + dir * u_texel_size * radius_px;
    vec4 s_meta = texture(u_peel_meta0, s_uv);
    if (s_meta.a < 0.5)
        return; // no geometry at this tap — doesn't contribute
    float delta = abs(center_z - s_meta.b);
    float range_falloff = clamp(1.0 - delta / REALCOLOR_SSS_MAX_DELTA_MM, 0.0, 1.0);
    vec3 s_n = normalize(texture(u_peel_normal0, s_uv).rgb * 2.0 - 1.0);
    float weight = range_falloff * max(dot(center_n, s_n), 0.0);
    accum_color += texture(u_accum_color, s_uv).rgb * weight;
    accum_weight += weight;
}

// NEOTKO_REALCOLOR_TAG s214 (PBR item 2): `sharp_lin` is the already box-filtered color at this
// output pixel — seeding the accumulator with it (weight 1.0) means a neighborhood fully
// rejected by edge-stopping degrades gracefully back to the sharp color instead of going black.
vec3 compute_sss(vec2 center_uv, vec3 sharp_lin)
{
    vec4 center_meta = texture(u_peel_meta0, center_uv);
    if (center_meta.a < 0.5 || u_sss_strength <= 0.0)
        return sharp_lin;

    int tool = int(center_meta.r + 0.5);
    // s253: acotado a mano — clamp() de enteros no existe en GLSL 1.10. Ver realcolor_peel.fs.
    // (El clamp EXTERIOR se queda: ese opera sobre floats y es legal en las dos versiones.)
    int sss_idx = tool;
    if (sss_idx < 0)  sss_idx = 0;
    if (sss_idx > 15) sss_idx = 15;
    float radius_px = u_sss_radius_px * clamp(u_material_td[sss_idx] / max(u_sss_reference_td, 1e-4), 0.15, 4.0);
    if (radius_px < 0.5)
        return sharp_lin;

    float center_z = center_meta.b;
    vec3 center_n = normalize(texture(u_peel_normal0, center_uv).rgb * 2.0 - 1.0);

    vec3 accum_color = sharp_lin;
    float accum_weight = 1.0;

    sss_sample(center_uv, center_z, center_n, vec2( 1.0,  0.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2(-1.0,  0.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.0,  1.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.0, -1.0), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.7,  0.7), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2(-0.7,  0.7), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2( 0.7, -0.7), radius_px, accum_color, accum_weight);
    sss_sample(center_uv, center_z, center_n, vec2(-0.7, -0.7), radius_px, accum_color, accum_weight);

    return mix(sharp_lin, accum_color / accum_weight, u_sss_strength);
}

// NEOTKO_REALCOLOR_TAG s243 (F6, "silueta opaca"): ¿este píxel está DENTRO de la pieza o en su
// contorno? Devuelve 0..1 = fracción de los 8 vecinos que tienen geometría.
//
// EL PROBLEMA QUE RESUELVE. Tras F1 (hinchado) quedan huecos que NO son aliasing: son huecos de
// verdad, columnas de aire que atraviesan la pieza entre líneas de extrusión. Se comprobó con la
// cuenta: a REALCOLOR_SUPERSAMPLE=2 hay exactamente 4 sub-téxeles por píxel, así que los 4 taps
// del filtro de caja cubren la rejilla ENTERA — `coverage/4` no es una estimación con aliasing,
// es el valor exacto. Un tap de 4 significa 25% de cobertura real. O sea que no hay nada que
// medir mejor: la pregunta correcta no es "¿cuánta pieza hay aquí?" sino "¿QUÉ SE VE a través
// de lo que falta?".
//
// Y la respuesta física es: el interior de la propia pieza. Mirando una pieza impresa real, por
// las juntas entre líneas no se ve la mesa — se ve la masa de plástico de detrás, en sombra. Lo
// que rompía la ilusión no era el hueco, era que detrás del hueco hubiera un logo con letras.
//
// EL CRITERIO. Un hueco rodeado de pieza por los 8 lados está DENTRO del contorno: sellarlo es
// correcto. Un píxel con pieza a un lado y aire al otro ES el contorno: ahí la transparencia
// parcial es el antialiasing de la silueta y hay que respetarla, o el objeto sale con el borde
// dentado. La vecindad distingue las dos cosas sin necesidad de conocer la geometría.
//
// Reutiliza el mismo disco de 8 taps que compute_ao()/compute_sss(), a radio de ~1 píxel de
// salida (2 téxeles a supersample 2) — ni una textura ni una pasada nuevas.
float interior_tap(vec2 center_uv, vec2 dir)
{
    return step(0.5, texture(u_peel_meta0, center_uv + dir * u_texel_size * 2.0).a);
}

float compute_interior(vec2 center_uv)
{
    float n = 0.0;
    n += interior_tap(center_uv, vec2( 1.0,  0.0));
    n += interior_tap(center_uv, vec2(-1.0,  0.0));
    n += interior_tap(center_uv, vec2( 0.0,  1.0));
    n += interior_tap(center_uv, vec2( 0.0, -1.0));
    n += interior_tap(center_uv, vec2( 0.7,  0.7));
    n += interior_tap(center_uv, vec2(-0.7,  0.7));
    n += interior_tap(center_uv, vec2( 0.7, -0.7));
    n += interior_tap(center_uv, vec2(-0.7, -0.7));
    return n / 8.0;
}

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

    float a1 = step(0.5, texture(u_peel_meta0, uv + o1).a);
    float a2 = step(0.5, texture(u_peel_meta0, uv + o2).a);
    float a3 = step(0.5, texture(u_peel_meta0, uv + o3).a);
    float a4 = step(0.5, texture(u_peel_meta0, uv + o4).a);

    float coverage = a1 + a2 + a3 + a4;
    if (coverage < 0.5) // none of the 4 taps saw any toolpath geometry here
        discard;

    vec3 c1 = a1 * texture(u_accum_color, uv + o1).rgb;
    vec3 c2 = a2 * texture(u_accum_color, uv + o2).rgb;
    vec3 c3 = a3 * texture(u_accum_color, uv + o3).rgb;
    vec3 c4 = a4 * texture(u_accum_color, uv + o4).rgb;
    vec3 lin = (c1 + c2 + c3 + c4) / coverage;

    float d1 = mix(1.0, texture(u_peel_depth0, uv + o1).r, a1);
    float d2 = mix(1.0, texture(u_peel_depth0, uv + o2).r, a2);
    float d3 = mix(1.0, texture(u_peel_depth0, uv + o3).r, a3);
    float d4 = mix(1.0, texture(u_peel_depth0, uv + o4).r, a4);
    float min_depth = min(min(d1, d2), min(d3, d4));

    // NEOTKO_REALCOLOR_TAG s214 (PBR item 2): subsurface blur runs BEFORE AO — AO is a shading/
    // occlusion modulation that should apply to the already-diffused material color, not the
    // other way around. See compute_sss() above; realcolor_accum.fs is untouched, this never
    // feeds back into the Beer-Lambert composite either.
    lin = compute_sss(uv, lin);

    // NEOTKO_REALCOLOR_TAG s166 (item 3): AO is a post-process on the already box-filtered
    // color, computed once per output pixel (not once per box-filter tap) — see compute_ao()
    // above for the kernel itself and realcolor_accum.fs is untouched, this never feeds back
    // into the Beer-Lambert composite.
    float ao = compute_ao(uv);
    lin *= mix(1.0, ao, u_ao_strength);

    // NEOTKO_PHOTOMODE_TAG s253: sombra de contacto, JUSTO DESPUÉS del AO y por la misma lógica que
    // el AO va después del SSS — cada uno modula el resultado del anterior, del material hacia la
    // presentación. El orden importa poco numéricamente (son dos multiplicaciones) pero mucho
    // conceptualmente: el AO dice "este píxel está en un rincón", la sombra dice "y además la luz
    // no le llega", y lo segundo se evalúa sobre lo primero.
    //
    // Se lee la profundidad del centro otra vez en vez de reutilizar la de compute_ao(): ese valor
    // es local a aquella función y sacarlo fuera obligaría a devolver dos cosas o a un out-param,
    // que en el gemelo 110 es más ruido del que ahorra. Un tap de textura más por píxel.
    vec4 sscs_center = texture(u_peel_meta0, uv);
    if (sscs_center.a >= 0.5)
        lin *= 1.0 - u_sscs_strength * compute_contact_shadow(uv, sscs_center.b);

    // NEOTKO_REALCOLOR_TAG s243 (F5 = A7): gradeo final, lo ÚLTIMO antes del gamma. Va aquí y no
    // en el peel por dos razones: se aplica una vez por píxel de salida en vez de una vez por
    // pasada de peel (hasta REALCOLOR_N_MAX veces), y sobre todo NO puede realimentar el
    // composite Beer-Lambert — desaturar dentro del acumulador cambiaría la mezcla física de
    // colores, que es justo lo que RealColor existe para calcular bien. Esto es presentación,
    // no material.
    //
    // Pivote en 0.18 lineal (gris medio fotográfico estándar, el "18% grey card"): comprimir
    // hacia ahí levanta los negros y baja los blancos sin desplazar la exposición media.
    // Luminancia con los pesos Rec.709 — el espacio aquí es lineal (ver la cabecera del fichero),
    // que es donde esos pesos son válidos; aplicarlos tras el gamma daría grises sucios.
    const float REALCOLOR_MID_GRAY = 0.18;
    const vec3 REALCOLOR_LUMA_709 = vec3(0.2126, 0.7152, 0.0722);
    lin = REALCOLOR_MID_GRAY + (lin - REALCOLOR_MID_GRAY) * u_grade_contrast;
    lin = mix(vec3(dot(max(lin, vec3(0.0)), REALCOLOR_LUMA_709)), lin, u_grade_saturation);
    lin = max(lin, vec3(0.0)); // el pivote puede empujar un canal bajo cero; linear_to_srgb hace pow()

    // NEOTKO_REALCOLOR_TAG s166: curved instead of linear coverage->alpha. Linear (coverage/4.0)
    // let a SINGLE covered tap out of 4 (partial aliasing at thin edges/sparse top-infill
    // hatching) read at alpha=0.25 — 75% of whatever's behind bleeds through, which on the real
    // printbed (has printed text/logo, not a flat color) showed up as bed lettering visibly
    // ghosting through the print. pow(x, 0.4) pushes 1/4 coverage to ~0.57 and 2/4 to ~0.76
    // while still reaching exactly 0 at zero coverage and 1 at full coverage — keeps s164's
    // original intent (thin/faint geometry fades in instead of vanishing outright) without
    // letting genuinely-covered-but-aliased pixels read as mostly-transparent.
    float out_alpha = pow(coverage / 4.0, 0.4);

    // NEOTKO_REALCOLOR_TAG s243 (F6): sella los huecos interiores, respeta el contorno.
    //
    // El smoothstep(0.70, 0.95) es donde vive la decisión: en el borde de silueta la vecindad
    // ronda 0.5 (medio disco dentro, medio fuera) y no se sella nada; para llegar a 0.70 hacen
    // falta ~6 de 8 vecinos con pieza, que sólo pasa dentro del contorno. El tramo entre los dos
    // umbrales evita que la transición cante como un recorte duro.
    //
    // ⚠️ Sólo sube el ALFA. No inventa color ni profundidad, y por eso sólo actúa sobre píxeles
    // que YA tienen geometría (los de cobertura 0 siguen haciendo discard arriba): esos tienen
    // color y profundidad reales de sus propios taps cubiertos, así que subirles el alfa no
    // fabrica información — sólo deja de dejar pasar el fondo por detrás de plástico que sí está
    // ahí. Rellenar además los huecos de cobertura 0 exigiría inventar color Y profundidad
    // interpolando, y taparía agujeros REALES del modelo (una perforación de 2px dejaría de
    // verse). Esa línea no se cruza aquí.
    float interior = smoothstep(0.70, 0.95, compute_interior(uv));
    out_alpha = mix(out_alpha, 1.0, interior * u_fill_interior);

    frag_color = vec4(linear_to_srgb(lin.r), linear_to_srgb(lin.g), linear_to_srgb(lin.b), out_alpha);
    gl_FragDepth = min_depth;
}
