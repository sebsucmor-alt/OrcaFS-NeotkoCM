// NEOTKO_COLORSCI_TAG_START — GD1 (P1 del plan COLORSTITCH_STUDIO_PLAN.md)
// GradientRamp — motor del Gradient Designer, SIN UI. Port literal de
// docs/MULTITEST/gen_gradient_grid.py (make_penu_colormix_pass +
// make_solid_top_pass + compute_sweep), construyendo SurfacePassStack
// directamente en vez de pasar por .3mf.
//
// VALIDACIÓN (obligatoria en CS-2, antes de cualquier UI): volcar
// top.to_json()/penu.to_json() de un spec default y diffear contra los
// atributos neotko_surface_passes_top/_penu que emite
//   python3 gen_gradient_grid.py --non-interactive
// para el mismo spec. Si los JSON no coinciden campo a campo, el port está
// mal — el Python es el ground-truth (validado contra 8rectangles.3mf).
#ifndef slic3r_ColorSci_GradientRamp_hpp_
#define slic3r_ColorSci_GradientRamp_hpp_

#include <string>
#include <vector>

#include "../ColorStitch.hpp"   // SurfacePassStack / SurfacePass

namespace Slic3r {
namespace ColorSci {

struct GradientSpec {
    int         tool_a = 0;            // 0-based — top arriba + base del pattern
    int         tool_b = 1;            // 0-based — top abajo + contraste
    std::string penu_pattern = "12";   // dígitos '1'..'4'; o salida de WeaveLibrary
    int         steps = 8;             // cubos/swatches de la rampa (>=1)
    double      split_min_mm = 0.04;   // clamp ≥ kMinPassMM (Tab.cpp pliega <0.04)
    double      split_max_mm = 0.16;
    double      layer_height = 0.2;    // LH del preset activo al abrir el Designer

    // Knobs penu — defaults canónicos capturados de 8rectangles.3mf
    // (DEFAULT_PENU_KV_EXTRAS del Python).
    double overlap     = 0.6;
    int    pct_a       = 50;
    int    pct_b       = 33;
    int    gamma       = 1;
    int    band_a      = 10;
    int    band_b      = 10;
    int    repetitions = 1;
};

struct GradientStep {
    double           a_mm = 0.0;       // grosor top A (arriba) — informativo/UI
    double           b_mm = 0.0;       // grosor top B (abajo)
    SurfacePassStack top;              // passes = [SOLID B, SOLID A] (bottom→top)
    SurfacePassStack penu;             // passes = [ColorStitch self-contained]
};

// Espesor mínimo de pass que sobrevive a la normalización del SandwichDialog
// (kMinPassMM en Tab.cpp — el fold de normalized_zone_json). El Designer
// clampea el sweep para no generar passes que el pipeline plegaría.
constexpr double kMinSweepMM = 0.04;

// Saneado del spec: clamp split a [kMinSweepMM, layer_height - kMinSweepMM],
// min<=max, steps>=1, pattern filtrado a dígitos '1'..'4' (vacío → "12").
// Devuelve copia corregida; `warnings` (opcional) recibe una línea por ajuste.
GradientSpec sanitize(const GradientSpec& spec,
                      std::vector<std::string>* warnings = nullptr);

// Construye la rampa completa. Paso i (0-based, f = i/(steps-1)):
//   b_mm = split_min + f * (split_max - split_min)
//   a_mm = split_max - f * (split_max - split_min)
// (port de compute_sweep — paso 1 = "casi todo A", paso N = "casi todo B").
// ratios = mm / layer_height. El penu es idéntico en todos los pasos.
std::vector<GradientStep> build_ramp(const GradientSpec& spec);

// El pass penu kind=2 con kv canónico completo (port de
// make_penu_colormix_pass + DEFAULT_PENU_KV_EXTRAS). Expuesto para que la
// pestaña Weaves (P5) pueda construir passes sueltos fuera de una rampa.
SurfacePass make_penu_colorstitch_pass(const GradientSpec& spec);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_GradientRamp_hpp_
// NEOTKO_COLORSCI_TAG_END
