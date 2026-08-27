// NEOTKO_COLORSCI_TAG_START — P0 (Fase A) + GD2
// StackFlatten — puente entre el formato autoritativo SurfacePassStack
// (ColorStitch.hpp:480) y las primitivas ColorSci. Port de la expansión
// de passes de SandwichDialog::blend_preview_zone (Tab.cpp:5372-5424).
//
// Tres consumidores previstos:
//   1. SandwichDialog (CS-1): blend_preview_zone/stacked_preview_color migran
//      a zone_colour()/sandwich_colour_legacy() — paridad visual exacta.
//   2. ColorStitch Studio (CS-3): sandwich_colour_stacked() para el preview
//      de la rampa (modelo físico — el sweep de grosores top SOLO tiene
//      sentido con Beer-Lambert apilado).
//   3. Futuro gizmo painter (footprint tint, roadmap punto 11): color
//      representativo de un profile = sandwich_colour_* de sus blobs.
#ifndef slic3r_ColorSci_StackFlatten_hpp_
#define slic3r_ColorSci_StackFlatten_hpp_

#include <string>
#include <vector>

#include "ColorSci.hpp"
#include "../ColorStitch.hpp"   // SurfacePassStack / SurfacePass / PathBlendPassConfig

namespace Slic3r {
namespace ColorSci {

// Expande un stack en franjas ponderadas (port 1:1 de Tab.cpp:5383-5424):
//   Solid     → 1 slice {tool, ratio}.
//   ColorStitch  → 1 slice por dígito del pattern, peso = ratio * count/total.
//               El pattern se busca en este orden:
//                 a) kv["pattern"] (clave corta que usa el editor per-pass),
//                 b) kv["interlayer_colormix_pattern_top" | "_penultimate"]
//                    (claves largas de los stacks self-contained — Designer
//                    y bake Plan1 de zone_colorstitch_snapshot),
//                 c) `fallback_pattern` (el caller la saca de su config viva;
//                    el Designer pasa "" porque sus stacks son autocontenidos).
//   PathBlend → 2 slices 50/50 (tool_bottom/tool_top del blob v2).
//   None      → nada.
// Stack disabled/empty → vector vacío.
std::vector<Slice> flatten_stack(const SurfacePassStack& st,
                                 bool penu,
                                 const std::string& fallback_pattern);

// Color de zona = blend_parallel(flatten_stack(...)). Equivalente funcional
// de blend_preview_zone; out_weight es el "transmit" que la UI muestra.
// Devuelve false si el stack está vacío/disabled (el caller decide el color
// de fondo, igual que el original devuelve GetBackgroundColour()).
bool zone_colour(const SurfacePassStack& st,
                 bool penu,
                 const std::string& fallback_pattern,
                 const Material mats[4],
                 float out_rgb[3],
                 float* out_weight = nullptr);

// Composición top-sobre-penu con α = clamp(weight_top, 0, 1) — port exacto
// de stacked_preview_color (Tab.cpp:5455-5466). Para paridad CS-1.
bool sandwich_colour_legacy(const SurfacePassStack& top,
                            const SurfacePassStack& penu,
                            const std::string& fallback_pattern_top,
                            const std::string& fallback_pattern_penu,
                            const Material mats[4],
                            float out_rgb[3]);

// Composición física: cada pass del stack es una capa Beer-Lambert apilada
// bottom→top sobre `bg_rgb` (passes[0] = el más profundo, mismo orden que
// SurfacePassStack). Un pass ColorStitch/PathBlend se colapsa primero a una
// capa equivalente:
//   rgb = blend_parallel de sus franjas (mezcla óptica lateral del dither),
//   td  = media de los td de sus tools ponderada por el peso de cada franja
//         (APROXIMACIÓN — el dither real transmite por línea, no en bloque;
//         válida mientras las líneas son finas frente a la distancia de
//         visión, que es la premisa de ColorStitch. Calibrar contra el grid
//         físico de gen_gradient_grid.py — ver plan §4).
// El orden de composición del sandwich completo: penu primero, top encima.
void sandwich_colour_stacked(const SurfacePassStack& top,
                             const SurfacePassStack& penu,
                             const Material mats[4],
                             const float bg_rgb[3],
                             float out_rgb[3],
                             float* out_transmit = nullptr);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_StackFlatten_hpp_
// NEOTKO_COLORSCI_TAG_END
