// NEOTKO_PROFILE_TAG_START — s233: cálculo del COLOR de la pintura ColorStitch
// (ColorStitch / PathBlend / Solid), extraído de GLGizmoColorStitchPainter para que lo
// pueda usar también quien dibuja la vista 3D normal — el objetivo "sandwich visible
// FUERA del gizmo" (docs/FUTURE/SANDWICH_VISIBLE_OUTSIDE_GIZMO_PLAN.md §2.1).
//
// Son los MISMOS cuerpos que vivían como métodos del gizmo (build_ebt_colors_for_volume,
// gizmo_materials, resolve_object_base_bg), con el ModelObject dueño pasado EXPLÍCITO en
// vez de leído de m_c->selection_info(): la vista normal necesita el color de todos los
// objetos pintados del plato a la vez, incluso sin gizmo instanciado. El gizmo conserva
// sus métodos como wrappers finos que rellenan ese default desde la selección.
//
// Vive en GUI (no en libslic3r) porque necesita preset_bundle / app_config.
#ifndef slic3r_GUI_ColorStitchPaintPreview_hpp_
#define slic3r_GUI_ColorStitchPaintPreview_hpp_

#include "libslic3r/ColorSci/ColorSci.hpp"   // ColorSci::Material
#include "libslic3r/Color.hpp"               // ColorRGBA

#include <cmath>     // s318 F3 — WeaveParams::shader_axis
#include <cstdint>
#include <map>
#include <memory>    // s318 F3 — shared_ptr<const TopZoneRecipe>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;   // NEOTKO_COLORSTITCH_TAG — s314
class ModelObject;
class ModelVolume;
class TriangleSelector;
struct SurfaceEffectProfile;
struct SurfacePass;
struct SurfacePassStack;
struct PathBlendPassConfig;
} // namespace Slic3r

namespace Slic3r::GUI::ColorStitchPaintPreview {

// Color de preview determinista cuando el perfil no trae preview_argb.
ColorRGBA fallback_color_for_id(int id);

// Color plano de un perfil: su preview_argb, o el fallback por id.
ColorRGBA color_for_profile(const SurfaceEffectProfile& p);

// Materiales del contexto actual: filament_colour (project_config) + TD
// (app_config neotko_td_N). Mismo origen que el SandwichDialog.
void materials(Slic3r::ColorSci::Material out[4], std::vector<std::string>& fcolors_out);

// Color base ya asignado al objeto (tool físico, o MixedFilament aproximado con
// blend_parallel), para componer los previews contra lo que el objeto va a imprimir de
// verdad en vez del negro que asumía todo antes. false = bg sin tocar (el llamador se
// queda con el suyo).
bool object_base_bg(const Slic3r::ColorSci::Material mats[4],
                    const ModelObject*               mo,
                    float                            bg_rgb[3]);

// Tabla slot→color del volumen: índice 0 = base sin pintar, 1..COLORSTITCH_SLOT_COUNT-1 =
// color compuesto de cada slot ocupado (sandwich_colour_stacked contra el fondo real del
// objeto `owner`). Layout calcado del de MMU, ver la nota en el cuerpo.
std::vector<ColorRGBA> slot_colors(const ModelVolume* mv, const ModelObject* owner);

// Clave de contexto de slot_colors(): cambia cuando cambiaría alguno de los colores que
// devuelve — TD, filament_colour, tool del objeto, tabla slot→perfil, o el contenido de
// los perfiles apuntados. NADA de eso toca el timestamp de colorstitch_paint_facets, así
// que quien cachea los colores necesita esta clave aparte para saber cuándo recalcular.
uint64_t context_key(const ModelVolume* mv, const ModelObject* owner);

// ¿Se debe dibujar la pintura ColorStitch en la vista 3D normal? app_config
// `neotko_show_paint_outside_gizmo` (ausente = sí).
bool show_outside_gizmo();

// ---------------------------------------------------------------------------------
// s235 F5 — MMU × Sandwich: los dos frentes que faltaban del plan de coexistencia
// (docs/FUTURE/MMU_SANDWICH_COEXISTENCE_PLAN.md §3 F5a/F5b).
// ---------------------------------------------------------------------------------

// F5b — ¿se dibuja el preview del sandwich DENTRO del gizmo de MMU? app_config
// `neotko_mmu_show_sandwich` (ausente = sí).
bool show_in_mmu_gizmo();

// F5a — cuánto se solapan las dos pinturas de un objeto. En la zona compartida manda el
// MMU (precedencia del motor desde s234), así que ese trozo NO llevará efecto sandwich:
// es justo lo que el painter tiene que contar antes de que se descubra en el gcode.
//
// ⚠️ `area_mm2` es una COTA SUPERIOR, no una medida exacta: se calcula por faceta original
// como min(área pintada de sandwich, área pintada de MMU) — exacto cuando la faceta está
// entera bajo las dos pinturas, y de más cuando cada una ocupa un trozo distinto de la
// misma faceta original. Para un aviso sobra; para geometría, no vale. Está en malla local
// del volumen (sin la escala de la instancia).
struct CoexistOverlap {
    double area_mm2      = 0.;   // cota superior del área compartida
    double sandwich_mm2  = 0.;   // área total pintada de sandwich (misma escala)
    int    facets        = 0;    // facetas originales con las dos pinturas
    bool   any() const { return facets > 0; }
};
CoexistOverlap mmu_sandwich_overlap(const ModelObject* mo);

// Clave de invalidación de mmu_sandwich_overlap(): timestamps de las DOS pinturas de todos
// los volúmenes del objeto. Cambia si y sólo si podría cambiar el solape.
uint64_t overlap_key(const ModelObject* mo);

// ---------------------------------------------------------------------------------
// Tejido / degradado (s233 F3). Todo esto vivía como `static` dentro de
// GLGizmoColorStitchPainter.cpp; se mudó aquí SIN cambiar una línea de matemáticas, para
// que la vista 3D normal pueda construir exactamente el mismo tejido que el painter.
// El gizmo sigue usándolas por su nombre de siempre (using-declarations en su .cpp).
// ---------------------------------------------------------------------------------

// Parámetros del tejido de UNA zona pintada, tal cual los consumen las uniforms
// u_weave_* (mm_gouraud y, desde s233, también gouraud / shells_lit).
struct WeaveParams {
    bool                   on        = false;
    bool                   tile      = false;      // true = repetir el patrón al ancho de
                                                   // línea real (wrap); false = recorrer la
                                                   // superficie una vez (degradados, clamp)
    float                  angle_rad = 0.7853982f; // orientación de las bandas (líneas de relleno)
    float                  pitch     = 0.45f;      // mm — paso de banda (ancho de línea si tile)
    float                  p0        = 0.f;        // mm — proyección del borde de la superficie
    std::vector<ColorRGBA> cols;                   // color por línea (un periodo si tile)
    // NEOTKO_COLORSTITCH_TAG — la banda está en AUTO (-1): el relleno alterna
    // `angle_rad` y `angle_rad`+90° según la paridad de la capa, así que NINGÚN
    // ángulo único es cierto para la banda entera. s280d — el aviso lo da el
    // contorno violeta pulsante del painter (antes: parpadeo del propio tejido).
    bool                   auto_angle = false;
    // NEOTKO_COLORSTITCH_TAG — s318 F3: eje de proyección en coordenadas de MALLA del
    // volumen, ya con la transformación de instancia y volumen metida (ver weave_frame).
    // El shader hace `proj = dot(u_weave_axis, pos)`. A cero = "no calculado" y se deriva
    // de `angle_rad` como antes (−sin, cos, 0): así un tejido que nadie ha pasado por
    // weave_frame (el color plano del Bottom) sigue pintando igual.
    float                  axis[3]   = {0.f, 0.f, 0.f};
    // NEOTKO_COLORSTITCH_TAG — s318 F3, opción A (ver make_zone_weave). Con `dual` el shader
    // compone DOS pases, cada uno con su eje: el de ARRIBA es esta LUT (`cols` = su color
    // propio y `dual_a` = su transmitancia, los dos en LINEAL) y el de ABAJO es la LUT 2
    // (`cols2` = color lineal de todo lo que queda debajo). color = a·c2 + b, luego a sRGB.
    // dual=false ⇒ nada de esto se lee y `cols` es sRGB como siempre.
    bool                   dual      = false;
    std::vector<ColorRGBA> dual_a;
    bool                   tile2     = false;
    float                  pitch2    = 1.f;
    float                  p0_2      = 0.f;
    float                  axis2[3]  = {0.f, 0.f, 0.f};
    std::vector<ColorRGBA> cols2;
    void shader_axis(float out[3]) const {
        if (axis[0] != 0.f || axis[1] != 0.f || axis[2] != 0.f) {
            out[0] = axis[0]; out[1] = axis[1]; out[2] = axis[2];
        } else {
            out[0] = -std::sin(angle_rad); out[1] = std::cos(angle_rad); out[2] = 0.f;
        }
    }
};

// NEOTKO_COLORSTITCH_TAG — s318 F3: el MARCO del tejido, para que el preview proyecte
// exactamente donde proyecta el motor.
//
// 🔑 El problema que cierra (plan F3 §3.1a). El motor proyecta cada línea en el marco de
// REBANADO: punto = trafo_centered() · M_volumen · v, y le resta el ancla, que es el origen
// del objeto en ese mismo marco. Como trafo_centered() sólo difiere de la matriz de la
// instancia en una traslación, lo que queda es
//     proj_motor − ancla = perp · L·(R_v·v + t_v)
// con L = parte LINEAL de la instancia (rotación, escala, espejo) y R_v/t_v las del volumen.
// El tejido, en cambio, proyectaba el vértice CRUDO de la malla (`−x·sin + y·cos`) y anclaba
// en el borde de la isla. Con un objeto sin rotar y un volumen sin transformar da igual; con
// la pieza girada en el plato las bandas giraban CON la pieza mientras el motor las deja
// fijas a la cama, y con escala el paso salía en mm de malla y no en mm impresos.
//
// Salida: `axis` = (L·R_v)ᵀ · perp, un vector en coordenadas de malla, y `anchor_proj` =
// dot(axis, origen del objeto en coords de malla) = −perp·(L·t_v). Así, para un vértice v:
//     proj_motor − ancla == dot(axis, v) − anchor_proj
// sin aproximaciones (incluye las inclinaciones de "apoyar en cara", que meten Z).
//
// `perp` sale como en lane_perp_axis(): (−sin θ, cos θ) en el marco de rebanado, con θ mod π
// si el ángulo es autorado, y con la canonización de semiplano SÓLO si no lo es
// (`canon_half_plane`), igual que las rutas de campo del motor (s315b).
// ⚠️ Instancia: se usa la primera, como ya hacía facing_of(). Dos instancias con distinta
// rotación son dos PrintObject distintos en el motor; aquí comparten tejido.
// ⚠️ La compensación de encogimiento del filamento (una escala) no se mete: es de décimas.
struct WeaveFrame {
    float axis[3]     = {0.f, 1.f, 0.f};
    float anchor_proj = 0.f;
    float proj(float x, float y, float z) const { return axis[0] * x + axis[1] * y + axis[2] * z; }
};
WeaveFrame weave_frame(const ModelVolume* mv, const ModelObject* owner,
                       float theta_rad, bool canon_half_plane);

// Contexto resuelto sin slice (preset actual).
// NEOTKO_COLORSTITCH_TAG — s314: los tres aceptan un `cfg` opcional. Con nullptr leen el
// preset de impresión EDITADO, que es lo que hacían siempre (todos los call-sites previos
// siguen valiendo sin tocarlos). Pasando un config concreto se puede resolver contra la
// config de UN objeto, que es lo que hace falta cuando el plato tiene varios anchos de
// línea: el preview del diálogo se abre para un objeto, no para el preset.
double weave_layer_height(const Slic3r::DynamicPrintConfig* cfg = nullptr);
double weave_top_line_width(const Slic3r::DynamicPrintConfig* cfg = nullptr);
// NEOTKO_COLORSTITCH_TAG — s314: SEPARACIÓN real entre líneas de relleno, en mm. DUEÑO
// ÚNICO, y no es lo mismo que el ancho: Orca modela el cordón como un rectángulo con los
// flancos redondeados y coloca las líneas a
//     spacing = width - layer_height * (1 - PI/4)
// (Flow::rounded_rectangle_extrusion_spacing, Flow.cpp:183) para que los flancos se solapen
// y no quede valle entre cordones. Con capa 0,2 el solape es 0,0429 mm FIJO, así que cuanto
// más fina la línea mayor es el porcentaje: 10% a 0,42 y 14% a 0,30. Medido contra el gcode
// del usuario en s314, clavado a la tercera cifra en cuatro objetos distintos.
// Cualquier cuenta de "cuántas líneas caben" DEBE dividir por esto y no por el ancho.
double weave_top_line_spacing(const Slic3r::DynamicPrintConfig* cfg = nullptr);
// NEOTKO_COLORSTITCH_TAG — dirección base del relleno sólido (rad). Punto de partida
// de una banda en AUTO; ver el comentario de la definición.
float  weave_solid_infill_dir_rad();
// NEOTKO_COLORSTITCH_TAG — s280d: `weave_animated_angle` y `set_weave_blink` se
// retiraron con el parpadeo. El uniform u_weave_angle recibe `w.angle_rad` tal cual;
// el aviso de "esto no está fijado" lo da el contorno violeta del painter.

// Color real de filamento de un tool 0-based (gris si no se puede leer).
ColorRGBA tool_col_rgba(const std::vector<std::string>& fcolors, int tool0);

// Secuencia de herramientas por línea de un ColorStitch — única fuente de verdad de
// todos los previews, construida con los MISMOS builders del motor.
// NEOTKO_COLORSTITCH_TAG — s314: `spacing_mm` sólo lo usa el modo 4 (bandas en mm), que
// necesita saber cuánto mide una línea para traducir el diseño a la secuencia. Con 0 se
// resuelve solo desde el preset editado (weave_top_line_spacing). Los modos 0-3 lo ignoran.
std::vector<int> colorstitch_tool_sequence(const std::map<std::string, std::string>& kv,
                                           bool penu, int n_lines,
                                           double spacing_mm = 0.0);
// Round-trip del blob PathBlend de un pase.
Slic3r::PathBlendPassConfig pro_pb_read(const Slic3r::SurfacePass& p);

std::map<std::string, std::string> colorstitch_kv_from_stack(const Slic3r::SurfacePassStack& st);
bool  pathblend_from_stack(const Slic3r::SurfacePassStack& st, Slic3r::PathBlendPassConfig& out);
std::map<std::string, std::string> colorstitch_top_kv(const Slic3r::SurfaceEffectProfile& prof);
bool  pathblend_top_config(const Slic3r::SurfaceEffectProfile& prof, Slic3r::PathBlendPassConfig& out);
float colorstitch_weave_theta(const std::map<std::string, std::string>& kv, bool& is_auto);

// s318 F3 — `pmin/pmax` son proyecciones con el eje de weave_frame() y `anchor_proj` la del
// origen del objeto (WeaveFrame::anchor_proj). Con 0 se ancla en el origen de la malla.
WeaveParams colorstitch_make_weave(const std::map<std::string, std::string>& kv,
                                   const std::vector<std::string>& fcolors,
                                   float theta, float pmin, float pmax, float line_w,
                                   float anchor_proj = 0.f,
                                   std::vector<int>* out_tools = nullptr);   // s318 F3: tool por entrada de `cols`
// NEOTKO_PATHBLEND_TAG — s280e: `theta` (orientación de las bandas) es un parámetro, ya no
// va clavado a 0. Estaba hardcodeado y por eso el ángulo de PathBlend NO se veía en el
// preview ni con el eje del degradado ya arreglado: las bandas salían siempre horizontales.
WeaveParams pathblend_make_weave(const Slic3r::PathBlendPassConfig& pbc,
                                 const Slic3r::ColorSci::Material mats[4],
                                 const float bg_rgb[3], double layer_h_mm,
                                 float theta,
                                 float pmin, float pmax, float line_w,
                                 // s318 F3: ancla del objeto (WeaveFrame::anchor_proj) y, si se
                                 // pide, las capas físicas de cada franja (rampa + tapa).
                                 float anchor_proj = 0.f,
                                 std::vector<std::vector<Slic3r::ColorSci::Layer>>* out_layers = nullptr);

// NEOTKO_COLORSTITCH_TAG — s318 F3, OPCIÓN A: la zona Top de un slot compuesta de verdad.
// El color que se ve en un punto es la pila física entera (Penu debajo, Top encima, cada
// pase una capa Beer-Lambert sobre el fondo del objeto). Si el Top y el Penu llevan cada uno
// un pase con efecto (ColorStitch o PathBlend), cada uno tiene SU eje y SU tejido, y en cada
// punto la franja de uno cae sobre la del otro. Eso es lo que ahora se compone por fragmento.
// Receta opaca: vive en el .cpp para no meter ColorStitch.hpp en todos los que incluyen esto.
// nullptr = el perfil no tiene pilas (payload legacy) o no hay ningún pase con efecto; el
// llamador sigue por el camino de siempre.
struct TopZoneRecipe;
std::shared_ptr<const TopZoneRecipe> resolve_top_zone(const Slic3r::SurfaceEffectProfile& prof,
                                                      const ModelVolume* mv, const ModelObject* owner,
                                                      const Slic3r::ColorSci::Material mats[4]);
// Marco del pase de abajo (o del único) con hi=false; del de arriba con hi=true (si no hay
// pase de arriba devuelve el de abajo).
const WeaveFrame& top_zone_frame(const TopZoneRecipe& r, bool hi);
bool              top_zone_auto(const TopZoneRecipe& r);
// Tejido compuesto de UNA isla. lo_* = extremos sobre el eje del pase de abajo, hi_* sobre
// el del pase de arriba. .on=false si no hay tejido que dibujar.
WeaveParams make_zone_weave(const TopZoneRecipe& r,
                            const std::vector<std::string>& fcolors,
                            const Slic3r::ColorSci::Material mats[4], const float bg_rgb[3],
                            float line_w, double layer_h_mm,
                            float lo_min, float lo_max, float hi_min, float hi_max);

// Tejido por ISLA de un volumen pintado. `sel` es un TriangleSelector ya deserializado
// desde mv->colorstitch_paint_facets (el gizmo pasa el suyo, vivo; la vista normal
// construye uno). Rellena facet_weave_idx (faceta → índice en weave_list).
// `any_auto_angle` (opcional) se pone a true si algún slot deja el ángulo en auto.
void weave_islands_for_volume(const ModelVolume*                              mv,
                              const Slic3r::TriangleSelector*                 sel,
                              const ModelObject*                              owner,
                              std::unordered_map<int, int>&                   facet_weave_idx,
                              std::vector<WeaveParams>&                       weave_list,
                              bool*                                           any_auto_angle = nullptr);

} // namespace Slic3r::GUI::ColorStitchPaintPreview

#endif // slic3r_GUI_ColorStitchPaintPreview_hpp_
// NEOTKO_PROFILE_TAG_END
