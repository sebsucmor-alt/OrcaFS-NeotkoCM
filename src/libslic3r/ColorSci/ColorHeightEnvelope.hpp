// NEOTKO_ALHCOLOR_TAG_START — Fase 0 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md)
// ColorHeightEnvelope — dado el color pintado en una zona (uno o varios tools
// con su TD) y, opcionalmente, si esa zona es top surface con un sandwich de
// N passes, calcula el rango de ALTURA DE CAPA (mm) en el que ese color imprime
// bien:
//
//   h_min  — suelo por FIDELIDAD: la capa más fina cuyo pass alcanza la
//            opacidad objetivo para el tool más translúcido (si es más fina, el
//            color se ve lavado sobre lo de debajo). Invierte slice_opacity.
//   h_max  — techo por RESOLUCIÓN de patrón: por encima de esto la banda de
//            mezcla objetivo (o el pass del sandwich) ya no cabe en una capa.
//   h_opt  — el mayor h dentro de [h_min, h_max] (menos capas, misma fidelidad).
//
// Función PURA: sin wxWidgets, sin app_config, sin estado global. El caller
// (gizmo Precision ALH o un paso headless) construye los Material y pasa el
// contexto. Toda la física reutiliza slice_opacity de ColorSci — sin matemática
// nueva, solo su inverso.
//
// UNIDADES (crítico, bug s166): Material::td está en "ratio units" = fracción de
// una altura de referencia. Para convertir a mm hay que multiplicar por esa
// altura de referencia (td_reference_height_mm). Todo el I/O de este header es
// en MILÍMETROS; la conversión ratio↔mm ocurre SOLO aquí, explícita.
#ifndef slic3r_ColorSci_ColorHeightEnvelope_hpp_
#define slic3r_ColorSci_ColorHeightEnvelope_hpp_

#include <vector>

#include "ColorSci.hpp"

namespace Slic3r {
namespace ColorSci {

// Contexto de una zona con color, tal como lo ve el envelope.
struct ColorHeightContext
{
    // Tools (0..3) pintados en esta zona. Vacío = sin color → el envelope es
    // no-op (devuelve los bounds de nozzle sin recortar).
    std::vector<int>     painted_tools;

    // Propiedades ópticas de los 4 tools (índice = tool id 0..3). Solo se leen
    // los que aparecen en painted_tools.
    Material             mats[4];

    // Altura de referencia (mm) a la que está definido Material::td (fracción de
    // layer height). Por defecto la layer height nominal del perfil. Con esto
    // td_mm[c] = td[c] * td_reference_height_mm.
    double               td_reference_height_mm = 0.2;

    // Bounds duros de slicing/nozzle (mm) — el envelope nunca sale de aquí.
    double               nozzle_min_height_mm = 0.04;
    double               nozzle_max_height_mm = 0.30;

    // --- Top surface / sandwich (opcional) ---------------------------------
    // Si esta zona es top surface Y se quiere reservar espacio para un sandwich
    // de color de `sandwich_passes` passes (auto-sandwich del ColorStitch
    // Painter / MixedFilament Object mode), cada pass necesita al menos
    // min_pass_height_mm. Entonces la capa top debe medir al menos
    // sandwich_passes * min_pass_height_mm para que la receta quepa.
    // sandwich_passes <= 1 desactiva esta restricción.
    int                  sandwich_passes       = 0;
    double               min_pass_height_mm    = 0.04;  // engine MinLayer
    double               min_top_visible_mm    = 0.05;  // suelo del pass visible
};

struct ColorHeightEnvelope
{
    double h_min = 0.04;   // suelo válido (mm)
    double h_max = 0.30;   // techo válido (mm)
    double h_opt = 0.20;   // altura óptima sugerida (mm)

    // true cuando fidelidad y patrón/sandwich NO tienen intersección
    // (h_min > h_max antes del clamp). En ese caso se prioriza fidelidad
    // (h_min gana) y h_max se sube a h_min; el caller debe avisar en UI.
    bool   conflict = false;

    // true cuando no había color (painted_tools vacío): envelope = bounds
    // de nozzle sin recortar (no-op).
    bool   passthrough = false;
};

// Opacidad objetivo por defecto para el suelo de fidelidad (90%). Configurable
// por el caller. Rango útil ~[0.7, 0.98].
constexpr float kDefaultTargetOpacity = 0.90f;

// Inverso de slice_opacity para un canal: grosor mínimo en mm para que el
// material `m` alcance `target_opacity` en el canal `c`, dado el reference
// height. td[c] ~0 → opaco → devuelve 0 (cualquier grosor sirve).
//   opacidad = 1 - 0.1^(t_mm / td_mm)  ⇒  t_mm = td_mm * log10(1/(1-op))
double min_thickness_for_opacity_mm(const Material& m,
                                    int             channel,
                                    float           target_opacity,
                                    double          td_reference_height_mm);

// Núcleo. Combina fidelidad (peor canal del tool más translúcido) + techo de
// patrón/sandwich + top-surface, y clampa a los bounds de nozzle.
// `mix_band_upper_mm` = techo de banda de color (p.ej.
// mixed_filament_height_upper_bound, default 0.16); <=0 desactiva ese techo.
ColorHeightEnvelope compute_color_height_envelope(const ColorHeightContext& ctx,
                                                  float  target_opacity   = kDefaultTargetOpacity,
                                                  double mix_band_upper_mm = 0.16);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_ColorHeightEnvelope_hpp_
// NEOTKO_ALHCOLOR_TAG_END
