// NEOTKO_COLORSCI_TAG_START — P0 (Fase A del plan COLORSTITCH_STUDIO_PLAN.md)
// ColorSci — núcleo de predicción de color del fork, extraído de los helpers
// estáticos `mp_*` de slic3r/GUI/Tab.cpp:1320-1473 (NEOTKO_MULTIPASS_TAG) para
// que engine, SandwichDialog y ColorStitch Studio compartan UNA implementación.
//
// Diferencias deliberadas respecto a los mp_* originales:
//   - TD es per-channel (float td[3]); el escalar legacy se carga como r=g=b.
//   - ΔE: se añade CIEDE2000 (Sharma 2005). ΔE76 se conserva para A/B testing
//     y para no romper comparaciones documentadas de sesiones previas.
//   - Dos composiciones explícitas en lugar de una implícita:
//       blend_stacked  — capas apiladas bottom→top (Beer-Lambert secuencial,
//                        port de mp_beer_blend). Modelo físico del sandwich.
//       blend_parallel — contribuciones lado-a-lado en un mismo plano
//                        (port de la agregación de blend_preview_zone:
//                        media ponderada por opacidad). Modelo del dither
//                        ColorMix/ColorStitch visto desde arriba.
//     El preview legacy del SandwichDialog usa parallel para TODO (incluso
//     passes Solid apilados); ver StackFlatten.hpp para las dos variantes de
//     composición de sandwich (legacy-parity vs physical).
//
// Reglas: funciones puras, sin wxWidgets, sin app_config, sin estado global.
// La GUI construye los Material[] (colores de filament_colour + TD de
// app_config) y llama. Modelo interno intercambiable: cuando entre
// Kubelka-Munk (N2), solo cambia el cuerpo de blend_stacked/blend_parallel.
#ifndef slic3r_ColorSci_hpp_
#define slic3r_ColorSci_hpp_

#include <array>
#include <string>
#include <vector>

namespace Slic3r {
namespace ColorSci {

// ---------------------------------------------------------------------------
// Tipos básicos

// Propiedades ópticas de un tool/filamento (0..3).
//   rgb : sRGB [0..1] (de `filament_colour`).
//   td  : transmission distance por canal, en "ratio units" (fracción de
//         layer height, igual que el TD escalar legacy `neotko_td_N`).
//         td[c] < ~1e-6 → opaco total en ese canal.
struct Material {
    std::array<float, 3> rgb { 0.5f, 0.5f, 0.5f };
    std::array<float, 3> td  { 0.f, 0.f, 0.f };
};

// Contribución lado-a-lado dentro de un plano (una "franja" del dither):
// tool 0..3 + peso/espesor relativo (fracción de layer height).
struct Slice {
    int   tool  = 0;
    float ratio = 0.f;
};

// Capa física apilada (para blend_stacked): color propio + td + espesor.
struct Layer {
    std::array<float, 3> rgb { 0.f, 0.f, 0.f };
    std::array<float, 3> td  { 0.f, 0.f, 0.f };
    float                ratio = 0.f;   // fracción de layer height
};

struct Lab { float L = 0.f, a = 0.f, b = 0.f; };

// ---------------------------------------------------------------------------
// Espacios de color (port de mp_rgb_to_lab, Tab.cpp:1327)

float srgb_to_linear(float c);
float linear_to_srgb(float c);
Lab   rgb_to_lab(const float rgb[3]);          // sRGB [0..1] → CIE Lab (D65)

// ΔE76 euclidiano — port exacto de mp_delta_e (Tab.cpp:1341).
float delta_e76(const Lab& x, const Lab& y);

// CIEDE2000 (Sharma, Wu & Dalal 2005). kL=kC=kH=1.
float delta_e2000(const Lab& x, const Lab& y);

// ---------------------------------------------------------------------------
// Composición

// Beer-Lambert apilado bottom→top — port de mp_beer_blend (Tab.cpp:1352) con
// TD per-channel. Entrada/salida en sRGB; internamente trabaja en lineal.
// out_transmit (opcional): transmitancia residual media de la pila completa
// [0..1] — 0 = opaca (no se ve el bg), 1 = transparente.
void blend_stacked(const std::vector<Layer>& layers,
                   const float bg_rgb[3],
                   float out_rgb[3],
                   float* out_transmit = nullptr);

// Media ponderada por opacidad de franjas coplanares — port de la agregación
// de SandwichDialog::blend_preview_zone (Tab.cpp:5431-5452), con TD
// per-channel (la opacidad de ponderación usa la media de los 3 canales para
// preservar el peso escalar del código original; el color sí mezcla por
// canal). out_weight (opcional): Σ opacidades — es el valor que el preview
// legacy usa como α de la zona top ("transmit=%.2f" en la UI).
void blend_parallel(const std::vector<Slice>& slices,
                    const Material mats[4],
                    float out_rgb[3],
                    float* out_weight = nullptr);

// Opacidad de una franja: 1 - 0.1^(ratio/td), por canal. Expuesta porque
// StackFlatten y los previews la reutilizan sueltos.
void slice_opacity(const Material& m, float ratio, float out_op[3]);

// ---------------------------------------------------------------------------
// Helpers de construcción (sin I/O — la GUI les pasa los strings ya leídos)

// "#RRGGBB" / "RRGGBB" → Material.rgb. TD escalar legacy → td r=g=b.
// Strings inválidos → gris 0.5 (mismo fallback que tool_colour, Tab.cpp:5079).
Material material_from_hex(const std::string& hex_rgb, float td_scalar);
Material material_from_hex(const std::string& hex_rgb,
                           float td_r, float td_g, float td_b);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_hpp_
// NEOTKO_COLORSCI_TAG_END
