#version 140

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);

// NEOTKO_REALCOLOR_TAG s251c — LA LUZ DIRECTA, POR FIN CON MANDOS.
//
// EL DIAGNÓSTICO QUE TRAJO ESTO (s251c, reportado por el usuario y confirmado con una prueba de un
// clic). El usuario veía en TODOS los contornos una veta brillante que leía como metalizada y que
// no podía ni subir ni bajar con ningún slider, pusiera el material que pusiera. Y tenía razón:
// hasta hoy la luz directa vivía en estos #define, o sea en constantes de compilación. s214 sacó a
// uniform el ambiente y el rim, pero se dejó dentro la luz PRINCIPAL — ésa era la asimetría.
//
// 🚨 Y ENCIMA HABÍA UNA TRAMPA QUE ENGAÑA AL QUE LO INVESTIGA: `surface finish` a 0 no apaga el
// brillo, lo devuelve al NEUTRO, que vale 1.0 (ver el return de realcolor_surface_finish() en
// GCodeViewer.cpp: `1.0 + (gloss-1.0)*k`). Bajar ese slider no quita brillo, quita la DIFERENCIA
// entre superficies. Así que la lectura razonable de "lo he bajado todo y sigue ahí" era correcta.
//
// 🔑 Y EL CULPABLE NO ERA EL ESPECULAR, ERA LA DIFUSA — verificado por el usuario poniendo el Gloss
// del material a 0 (que es lo único que hoy llegaba a intensity.y) y comprobando que las vetas NO
// desaparecían. Tiene sentido geométrico y conviene entenderlo: **un cilindro lambertiano ya
// produce una línea brillante a lo largo de su eje**, porque NdotL es máximo en la generatriz que
// mira a la luz y cae a cero en los flancos. Un render de toolpath es un campo de cilindros, así
// que la difusa sola dibuja una veta por cordón sin ayuda de ningún especular. Sobre un color
// saturado y a plena intensidad, esa veta se satura hacia el blanco y se lee como metal.
//
// u_light_wrap ES LA PIEZA NUEVA, y no es un mando más: es la que ataca la causa. Sustituye
// max(N·L, 0) por (N·L + w)/(1 + w), o sea difusa envolvente (half-Lambert). Suaviza el terminador
// y sube el nivel del flanco en sombra, que es exactamente lo que hace la dispersión subsuperficial
// en un plástico translúcido de verdad.
//
// 🔑 Y ESO CIERRA EL SEGUNDO PROBLEMA DEL MISMO REPORTE. El usuario también decía que el SSS de
// pantalla (realcolor_present.fs, s214) no hacía absolutamente nada, ni con el TD al máximo. No
// está roto: su corte bilateral pesa cada muestra por `max(dot(center_n, s_n), 0)`, y en esta
// geometría el píxel de al lado está en otra faceta de otro cordón con una normal muy distinta, así
// que las 8 muestras se rechazan y el resultado colapsa al color nítido con el que se siembra el
// acumulador. **El SSS de pantalla lo rechaza precisamente la corrugación que define esta
// geometría.** El wrap consigue el mismo objetivo perceptual (luz que se cuela más allá del
// terminador) SIN muestrear vecinos, así que la corrugación no puede rechazarlo. No se toca el SSS
// de pantalla: sigue sirviendo para lo suyo, sangrar color entre materiales distintos en un
// sandwich, que es donde sí hay contraste de color entre vecinos.
//
// ⚠️ NEUTRO EXACTO = (0.48, 0.18, 0.075, 20.0, 0.0), que son los #define de antes ya multiplicados
// por INTENSITY_CORRECTION (0.6). Con esos cinco valores la imagen es bit-idéntica a s251b.
uniform float u_light_key;       // difusa de la luz principal   (era LIGHT_TOP_DIFFUSE   = 0.8*0.6)
uniform float u_light_fill;      // difusa de la luz de relleno  (era LIGHT_FRONT_DIFFUSE = 0.3*0.6)
uniform float u_light_spec;      // especular directo            (era LIGHT_TOP_SPECULAR  = 0.125*0.6)
uniform float u_light_shininess; // exponente del especular      (era LIGHT_TOP_SHININESS = 20.0)
uniform float u_light_wrap;      // 0 = Lambert puro (idéntico a antes), 1 = difusa envolvente

// NEOTKO_REALCOLOR_TAG: two-tone sky/ground ambient replacing the old flat INTENSITY_AMBIENT
// (0.3) — mixed by normal.y so faces pointing up read a touch brighter (open sky) and faces
// pointing down a touch darker (occluded by the print itself), instead of a uniform wash.
// Fresnel/rim term added into intensity.y (the channel realcolor_peel.fs already treats as
// additive/white, see "lit = vec3(intensity.y) + base*intensity.x" there) — gives a subtle edge
// highlight typical of translucent plastic/resin under directional light. Both live-tunable via
// GCodeViewer::RealColorTuning + the debug panel in render_toolpaths_realcolor() (see
// GCodeViewer.cpp set_uniform calls right before this shader's peel draw) — uniforms instead of
// #define so they can be retuned without recompiling shaders.
// NEOTKO_REALCOLOR_TAG s214 (PBR item 3, docs/WIP/REALCOLOR_VIEW/09_HDR_ENVIRONMENT_PLAN.md):
// u_ambient_ground/sky, the tints, and u_fresnel_power/strength/tint all moved OUT of this file
// — ambient/rim are now real texture samples in realcolor_peel.fs (see that file), because
// texture sampling from a vertex shader (VTF) isn't guaranteed on the legacy/compatibility GL
// profile this app already knows it can land on (s164). item 1b's v_ambient_rgb/v_rim_rgb
// varyings and their per-vertex mix are gone too, superseded (not run in parallel) by item 3's
// per-fragment env sampling.
uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;

// NEOTKO_REALCOLOR_TAG s243 (F1, "huecos" — versión barata de A1 en
// docs/WIP/REALCOLOR_PSEUDOREALISTIC.md): desplazamiento del vértice a lo largo de su propia
// normal, en MILÍMETROS de mundo (v_position ya es mundo, no hay matriz de modelo para los
// toolpaths — ver v_world_pos abajo).
//
// EL PORQUÉ. La geometría del toolpath se genera con el `width` NOMINAL del path, cinta contra
// cinta y sin solape. Una extrusión real se aplasta contra la de al lado y SOLAPA: en la pieza
// impresa no hay junta entre líneas contiguas. En el render sí la hay, y por esa junta se ve el
// fondo (la cama, con su logo) de arriba abajo — es la queja de "huecos" del usuario, y es
// geometría real, no aliasing (el aliasing es el otro frente, ver el filtro de caja en
// realcolor_present.fs). Hinchar cada tubo media junta hacia fuera las hace tocarse.
//
// Por qué aquí y no regenerando los buffers: los TBuffers de vértices los comparten TODAS las
// vistas (Tool, FeatureType, Height...), así que ensanchar la sección de verdad obligaría a
// regenerarlos al entrar y salir de RealColor. Esto es un uniform y cero buffers.
//
// ⚠️ Hincha también la silueta exterior y las paredes finas — por eso sale como knob
// (RealColorTuning::swell_*_mm, panel de debug) y no como constante. A (0,0) el resultado es
// bit-idéntico al de antes de s243.
//
// 🔑 s243b — ANISÓTROPO, y esto es lo importante: .x = flancos, .y = caras de arriba/abajo.
// Un solo escalar estaba haciendo dos trabajos OPUESTOS, y de ahí que el usuario viera que 0.1
// arreglaba unas zonas y 0.014 otras distintas.
//
// Un tubo de toolpath tiene dos familias de caras. El hueco entre dos líneas contiguas de la
// MISMA capa está bordeado por los FLANCOS de cada tubo (normal horizontal): acercarlos es lo
// único que cierra ese hueco, que es el que se ve al mirar una superficie desde arriba. La cara
// de arriba no bordea ese hueco — hincharla no cierra nada, sólo hace la capa más alta, funde
// cada capa con la de encima y borra la costura entre capas (la pared sale abombada y sin
// definición: exactamente lo que se veía a swell=0.1 uniforme en la banda baja).
//
// Se separan por |n.z| porque las normales llegan en espacio MUNDO y este mundo es Z-arriba —
// el mismo hecho que F3 acaba de dejar fijado en equirect_uv(). |n.z|≈1 es cara horizontal
// (arriba/abajo), |n.z|≈0 es flanco.
//
// ⚠️ El componente vertical hay que dejarlo CORTO por una razón que no es estética: el bias de
// peel de realcolor_peel.fs se acota con `u_thickness`, la altura REAL del path. Si se engorda
// la geometría en Z mucho más que eso, la comparación de profundidad del peel empieza a medir
// sobre una altura que ya no existe y se descartan capas que sí deberían componer. Por eso su
// slider llega mucho más abajo que el de los flancos.
uniform vec2 u_swell_mm;

// NEOTKO_REALCOLOR_TAG s251 (Fase 0, docs/FUTURE/REALCOLOR_ANISOTROPIC_SHEEN_PLAN.md): EJE DE
// IMPRESIÓN del path que se está dibujando. .xy = eje unitario en XY de MUNDO, .z = confianza 0..1.
// Se manda POR SUB-DRAW desde render_toolpaths_realcolor(), igual que u_thickness / u_finish /
// u_swell_mm y por la misma razón (un RenderPath agrupa sólo por color y mezcla paths distintos) —
// pero aquí el argumento es más fuerte todavía: el eje es LO ÚNICO que distingue una scanline de
// su vecina dentro del mismo top, así que mandarlo una vez por lote no sería impreciso, sería
// justo lo contrario del efecto que se busca. Lo calcula realcolor_bead_axis() en GCodeViewer.cpp
// a partir de Path::bead_axis_acc, que se llena durante la carga.
uniform vec3 u_bead_axis;

in vec3 v_position;
in vec3 v_normal;

// x = direct-light diffuse only (ambient comes from the env texture now, see realcolor_peel.fs),
// y = specular;
out vec2 intensity;

// NEOTKO_REALCOLOR_TAG s214 (PBR item 3): raw world-space position, pass-through (v_position is
// already world space — no per-object model matrix for gcode toolpaths, see 09's coordinate-
// space note). realcolor_peel.fs needs this to build a WORLD-space view direction for the env
// mirror reflection, consistent with the world-space normal (v_view_normal) — reusing the
// eye-space `position` computed below for that would mix reference frames.
out vec3 v_world_pos;

// NEOTKO_REALCOLOR_TAG: linear eye-space depth (camera-space distance along the view axis),
// used by realcolor_peel.fs for the peel-order comparison instead of gl_FragCoord.z. NDC depth
// is nonlinear w.r.t. real distance, so a bias in NDC space breaks differently at every zoom
// level (confirmed empirically: the noisy band moved with zoom). This is zoom-invariant.
out float v_eye_z;

// NEOTKO_REALCOLOR_TAG s166 (item 3, SSAO): view-space normal, forwarded to a 3rd MRT target
// in realcolor_peel.fs so the present pass can run screen-space AO — this is the exact same
// `normal` already computed below for the ambient/fresnel terms, just exposed instead of being
// dropped after use.
out vec3 v_view_normal;

// NEOTKO_REALCOLOR_TAG s251 (Fase 0): eje del cordón resuelto POR VÉRTICE, en espacio de mundo.
// .xyz = eje (T, unitario y perpendicular a la normal), .w = confianza 0..1. Lo consume el lóbulo
// anisótropo de realcolor_peel.fs. Con .w = 0 el fragment shader se apaga solo.
out vec4 v_bead_tangent;

// NEOTKO_REALCOLOR_TAG: identical to gouraud_light.vs — depth peeling only changes the
// fragment stage (material lookup + prev-depth test + MRT output), not the lighting math.
void main()
{
    vec3 normal = normalize(view_normal_matrix * v_normal);
    v_view_normal = normal;

    // NEOTKO_REALCOLOR_TAG s243 (F1): el hinchado va sobre la normal SIN transformar (v_normal,
    // espacio de mundo igual que v_position) — no sobre `normal`, que ya ha pasado por
    // view_normal_matrix. Hoy esa matriz es la identidad (ver el set_uniform en
    // render_toolpaths_realcolor), así que darían lo mismo; se escribe explícito para que el día
    // que deje de serlo esto no empiece a desplazar vértices en la dirección equivocada en
    // silencio. Todo lo que se deriva de la posición (mundo, eye_z, gl_Position) usa YA la
    // posición hinchada, de modo que el orden del peel y el bias de realcolor_peel.fs siguen
    // midiendo sobre la misma geometría que se rasteriza.
    vec3 n_world = normalize(v_normal);
    float facing_up = abs(n_world.z); // 1 = cara horizontal (arriba/abajo), 0 = flanco
    vec3 swollen_pos = v_position + n_world * mix(u_swell_mm.x, u_swell_mm.y, facing_up);
    v_world_pos = swollen_pos; // pass-through, already world space

    // NEOTKO_REALCOLOR_TAG s251 (Fase 0 del brillo anisótropo): EL FRAME LOCAL DEL CORDÓN, o al
    // menos la parte que hace falta para el brillo — el eje T. Sale de DOS fuentes distintas según
    // hacia dónde mire la cara, y esa asimetría no es un apaño: es que la información realmente
    // está en un sitio y no en el otro.
    //
    // 🔑 FLANCOS — GRATIS, Y EXACTO POR VÉRTICE. Al generar el tubo (add_vertices_as_solid en
    // GCodeViewer.cpp) las normales laterales se construyen como right = (dir.y, −dir.x, 0), o sea
    // la dirección del cordón GIRADA 90° en XY. Deshacer ese giro es una línea:
    //     T = (−n.y, n.x, 0)
    // Ni uniform, ni atributo nuevo, ni tocar los TBuffers (que los comparten TODAS las vistas —
    // regenerarlos al entrar en RealColor es justo lo que F1 evitó en s243). El dato llevaba ahí
    // desde siempre, escondido dentro de la normal.
    //
    // 🔑 CARAS DE ARRIBA/ABAJO — AHÍ NO HAY NADA QUE RECUPERAR. La normal de un top es ±Z pura, sea
    // cual sea la dirección del cordón: matemáticamente no contiene el dato. Y es justo la cara que
    // importa para un top planchado y para un sandwich. Por eso existe u_bead_axis, que trae el eje
    // promediado del path desde C++. No es redundancia con lo de arriba: es el único sitio de donde
    // puede salir.
    //
    // La elección es por |n.z| y con un umbral duro a propósito: las caras de un prisma de cordón
    // son o flanco (|n.z|≈0) o tapa (|n.z|≈1), no hay una población real en medio, y un mix() entre
    // dos ejes que pueden ser casi opuestos daría un vector corto y sin sentido justo en las pocas
    // caras intermedias (rampas de Z, uniones mitradas) en vez de elegir una de las dos respuestas
    // buenas.
    vec3 t_flank = vec3(-n_world.y, n_world.x, 0.0);
    float flank_len = length(t_flank);
    vec4 bead;
    if (facing_up > 0.5) {
        // Tapa: el eje viene del path entero. La confianza la manda C++ y puede ser 0 (un perímetro
        // externo es un bucle cerrado y no tiene eje dominante) — el .fs se apaga solo con eso.
        bead = vec4(u_bead_axis.x, u_bead_axis.y, 0.0, u_bead_axis.z);
    } else if (flank_len > 1e-4) {
        // Flanco: exacto para ESTE vértice, así que confianza 1 sin depender del promedio del path.
        // Un perímetro externo curvo tiene confianza 0 como path y aun así sus flancos brillan
        // bien, porque cada uno conoce su propia dirección. Es una ventaja real, no un descuido.
        bead = vec4(t_flank / flank_len, 1.0);
    } else {
        // n_world casi paralelo a Z pero clasificado como flanco: no puede pasar con la geometría
        // que genera add_vertices_as_solid, pero un cordón puramente vertical dejaría right = 0/0.
        bead = vec4(0.0, 0.0, 0.0, 0.0);
    }

    // Gram-Schmidt contra la normal. En los dos casos de arriba T ya es perpendicular a N por
    // construcción, así que esto normalmente no hace nada; hace falta en las uniones mitradas, donde
    // la normal interpolada entre dos segmentos se aparta de las dos originales. Sin esto el lóbulo
    // se ensucia justo en las esquinas, que es donde más se mira.
    bead.xyz = bead.xyz - n_world * dot(n_world, bead.xyz);
    float bl = length(bead.xyz);
    v_bead_tangent = (bl > 1e-4) ? vec4(bead.xyz / bl, bead.w) : vec4(0.0, 0.0, 0.0, 0.0);

    // NEOTKO_REALCOLOR_TAG s251c: difusa ENVOLVENTE, (N·L + w)/(1 + w) en vez de max(N·L, 0).
    // El denominador renormaliza para que una cara que mira de frente a la luz siga valiendo 1 —
    // sin él, subir el wrap subiría el brillo global además de suavizar, y serían dos efectos en un
    // solo mando. El max() exterior sigue haciendo falta: con w < 1 las caras muy a contraluz
    // todavía dan negativo.
    float wrap_denom = 1.0 + max(u_light_wrap, 0.0);
    float NdotL = max((dot(normal, LIGHT_TOP_DIR) + u_light_wrap) / wrap_denom, 0.0);
    intensity.x = NdotL * u_light_key; // direct light only, ambient is env-sampled in .fs

    vec4 position = view_model_matrix * vec4(swollen_pos, 1.0);
    // El wrap NO toca el especular a propósito: es un modelo de difusa (aproxima que la luz entra,
    // se dispersa bajo la superficie y sale por donde no le tocaba). Un reflejo especular es
    // superficial y no hace nada de eso.
    intensity.y = u_light_spec * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, normal)), 0.0), max(u_light_shininess, 1.0));
    v_eye_z = -position.z; // right-handed eye space, camera looks down -Z

    NdotL = max((dot(normal, LIGHT_FRONT_DIR) + u_light_wrap) / wrap_denom, 0.0);
    intensity.x += NdotL * u_light_fill;

    gl_Position = projection_matrix * position;
}
