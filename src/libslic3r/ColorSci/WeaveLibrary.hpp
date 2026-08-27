// NEOTKO_COLORSCI_TAG_START — C1 (P5 del plan COLORSTITCH_STUDIO_PLAN.md)
// WeaveLibrary — ligamentos textiles como generadores de pattern strings
// ColorStitch. Implementa C.1 + C.3 del COLOR_PREDICTION_UNIFIED_PLAN.md
// (dropdown de tafetán/sarga/satén/houndstooth); la sarga REAL (offset por
// capa, C.2) requiere tocar el dispatcher de ColorStitch — hasta
// entonces `offset_per_layer` se expone pero la UI debe mostrarlo
// deshabilitado/informativo (regla del plan: no prometer sarga real antes
// de C.2).
//
// Convención de patterns: dígitos '1'..'4' = tools A..D del pass (mapeados
// por interlayer_colormix_*_tool_{a..d}); cada dígito = una línea del dither.
// La analogía textil: pattern top a 0° = urdimbre, penu a 90° = trama
// (TEXTILE PATTERNS, superseded en _reference/).
#ifndef slic3r_ColorSci_WeaveLibrary_hpp_
#define slic3r_ColorSci_WeaveLibrary_hpp_

#include <string>
#include <vector>

namespace Slic3r {
namespace ColorSci {

struct WeavePreset {
    // name SIN traducir — la UI lo pasa por _L() (los .po se generan del
    // literal; mantener estable).
    const char* name;
    const char* pattern_top;       // string para la zona top
    const char* pattern_penu;      // string para la zona penu ("" = igual que top)
    int         offset_per_layer;  // desplazamiento del pattern por capa (C.2;
                                   // 0 = fijo — ÚNICO valor honrado hoy)
    bool        paired;            // true → escribir top y penu ATÓMICAMENTE
                                   // (houndstooth, C.3); false → el usuario
                                   // elige a qué zona aplicar
    const char* note;              // tooltip corto para la UI (sin traducir)
};

// Orden estable — la UI los lista tal cual. Índice 0 = Custom (sin efecto,
// deja el campo de texto libre como hasta ahora).
const std::vector<WeavePreset>& weave_presets();

// Aplica `offset` rotaciones a la izquierda al pattern (helper para C.2 y
// para el preview de sarga cuando exista). "1122" offset 1 → "1221".
std::string rotate_pattern(const std::string& pattern, int offset);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_WeaveLibrary_hpp_
// NEOTKO_COLORSCI_TAG_END
