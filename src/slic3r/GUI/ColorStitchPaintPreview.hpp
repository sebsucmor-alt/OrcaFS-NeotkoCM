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

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
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
};

// Contexto resuelto sin slice (preset actual).
double weave_layer_height();
double weave_top_line_width();
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
std::vector<int> colorstitch_tool_sequence(const std::map<std::string, std::string>& kv,
                                           bool penu, int n_lines);
// Round-trip del blob PathBlend de un pase.
Slic3r::PathBlendPassConfig pro_pb_read(const Slic3r::SurfacePass& p);

std::map<std::string, std::string> colorstitch_kv_from_stack(const Slic3r::SurfacePassStack& st);
bool  pathblend_from_stack(const Slic3r::SurfacePassStack& st, Slic3r::PathBlendPassConfig& out);
std::map<std::string, std::string> colorstitch_top_kv(const Slic3r::SurfaceEffectProfile& prof);
bool  pathblend_top_config(const Slic3r::SurfaceEffectProfile& prof, Slic3r::PathBlendPassConfig& out);
float colorstitch_weave_theta(const std::map<std::string, std::string>& kv, bool& is_auto);

WeaveParams colorstitch_make_weave(const std::map<std::string, std::string>& kv,
                                   const std::vector<std::string>& fcolors,
                                   float theta, float pmin, float pmax, float line_w);
// NEOTKO_PATHBLEND_TAG — s280e: `theta` (orientación de las bandas) es un parámetro, ya no
// va clavado a 0. Estaba hardcodeado y por eso el ángulo de PathBlend NO se veía en el
// preview ni con el eje del degradado ya arreglado: las bandas salían siempre horizontales.
WeaveParams pathblend_make_weave(const Slic3r::PathBlendPassConfig& pbc,
                                 const Slic3r::ColorSci::Material mats[4],
                                 const float bg_rgb[3], double layer_h_mm,
                                 float theta,
                                 float pmin, float pmax, float line_w);

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
