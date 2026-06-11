// NEOTKO_COLORSCI_TAG_START — Predict (ronda Flat/Mixed, COLORSTITCH_STUDIO_PLAN.md)
// ColorPredict — generación de paletas-predicción y match inverso a un color
// objetivo, sobre el forward model de ColorSci. Dos familias:
//
//   FLAT  — solo passes Solid apilados (Beer-Lambert). El gamut "cortable"
//           con los filamentos tal cual. Robusto, predecible.
//   MIXED — base penu ColorStitch (dither óptico, p.ej. patrón "12") + un
//           Solid top translúcido encima. Añade un primario nuevo (el
//           promedio óptico del dither) → GAMUT EXTENDIDO: colores que el
//           flat NO alcanza (no existe un filamento que sea "50% A + 50% B",
//           pero el dither lo aproxima y el top lo tiñe).
//
// Cada entrada es una RECETA completa (par de SurfacePassStack top/penu) +
// el color predicho. La UI la usa para: (a) pintar swatches de paleta,
// (b) al hacer click CARGAR la receta en el sandwich vivo (m_stack[]),
// (c) exportar como SurfaceEffectProfile (preview_argb = color predicho).
//
// Todo reutiliza blend_stacked / sandwich_colour_stacked / delta_e2000 de
// ColorSci — sin matemática nueva, solo enumeración + búsqueda.
#ifndef slic3r_ColorSci_ColorPredict_hpp_
#define slic3r_ColorSci_ColorPredict_hpp_

#include <array>
#include <string>
#include <vector>

#include "ColorSci.hpp"
#include "GradientRamp.hpp"         // GradientSpec / GradientStep (dispatcher)
#include "../SurfaceColorMix.hpp"   // SurfacePassStack

namespace Slic3r {
namespace ColorSci {

// Una receta = sandwich completo + color predicho. `delta_e` >= 0 solo
// cuando viene de un match contra target (browse = -1).
struct ColorRecipe {
    SurfacePassStack     top;
    SurfacePassStack     penu;          // vacío/disabled en recetas Flat puras
    std::array<float, 3> rgb { 0.5f, 0.5f, 0.5f };  // color predicho sRGB [0..1]
    float                delta_e = -1.f;
    std::string          desc;          // etiqueta corta human-readable
};

struct PredictOptions {
    double layer_height = 0.2;
    int    max_entries  = 64;     // tope tras dedup (paletas browse)
    float  dedup_de     = 2.5f;   // ΔE2000 mínimo entre dos swatches distintos
    int    ratio_steps  = 8;      // granularidad del sweep de ratios/grosores
    float  bg_rgb[3]    = { 0.f, 0.f, 0.f };   // fondo (negro = lecho/oscuro)

    // Mínimos de impresión (el GENERADOR es más conservador que el modo
    // Gradient manual — ahí el usuario puede bajar a 0.04 bajo su riesgo).
    //   min_pass_mm        = suelo de CUALQUIER pass (engine MinLayer 0.04).
    //   min_top_visible_mm = suelo del pass VISIBLE arriba (0.05 "seguro": un
    //                        top fino de 0.04 imprime mal en cara vista; 0.04
    //                        solo es válido cuando va DEBAJO de otro color).
    // Ej. capa 0.20: en vez de top (0.16 abajo / 0.04 arriba) → (0.15 / 0.05).
    double min_pass_mm        = 0.04;
    double min_top_visible_mm = 0.05;
};

// --- Paletas browse (enumerar el gamut alcanzable) -------------------------

// FLAT: 1 solid (cada tool) + 2 solids apilados (todas las parejas, sweep de
// ratio). Dedup por ΔE2000. Ordenadas por luminosidad (L de Lab).
std::vector<ColorRecipe> predict_flat_palette(const Material mats[4],
                                              const PredictOptions& opt);

// MIXED: por cada pareja de tools (A,B) → penu dither "AB" → sweep de top
// Solid (cada tool candidato, grosor en `ratio_steps`). Dedup. El gamut
// extendido incluye los promedios ópticos que el flat no puede.
std::vector<ColorRecipe> predict_mixed_palette(const Material mats[4],
                                               const PredictOptions& opt);

// --- Match inverso (target → mejor receta) ---------------------------------

// Mejor stack de solids (1..3 passes) que minimiza ΔE2000 al target.
// Migra la idea de mp_suggest pero produce un SurfacePassStack y usa ΔE2000.
ColorRecipe suggest_flat(const float target_rgb[3],
                         const Material mats[4],
                         const PredictOptions& opt);

// Mejor receta mixed (penu dither + top solid) que minimiza ΔE2000 al target.
ColorRecipe suggest_mixed(const float target_rgb[3],
                          const Material mats[4],
                          const PredictOptions& opt);

// --- Dispatcher (PR.1 — COLORSTITCH_PAINTER_REVAMP_PLAN.md) -----------------
//
// Frontera única de generación de paletas: unifica las tres familias en un
// solo `vector<ColorRecipe>` homogéneo para que las DOS UIs (SandwichDialog
// wxDC · gizmo ImGui) consuman el mismo modelo. Si cambia la matemática de las
// recetas se toca SOLO aquí.
//
//   Flat          → predict_flat_palette
//   Mixed         → predict_mixed_palette
//   GradientRamp  → build_ramp(*ramp), envolviendo cada GradientStep en un
//                   ColorRecipe (rgb = sandwich_colour_stacked con `mats`/bg;
//                   desc = "A x.xx / B x.xx mm"). `ramp` debe ser no-null para
//                   este kind; con null devuelve vacío.
enum class PaletteKind { Flat, Mixed, GradientRamp };

std::vector<ColorRecipe> build_palette(PaletteKind kind,
                                       const Material mats[4],
                                       const PredictOptions& opt,
                                       const GradientSpec* ramp = nullptr);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_ColorPredict_hpp_
// NEOTKO_COLORSCI_TAG_END
