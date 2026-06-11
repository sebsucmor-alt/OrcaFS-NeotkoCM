// NEOTKO_COLORSCI_TAG_START — C1 (P5)
#include "WeaveLibrary.hpp"

namespace Slic3r {
namespace ColorSci {

const std::vector<WeavePreset>& weave_presets()
{
    // Equivalencias de COLOR_PREDICTION_UNIFIED_PLAN.md §Fase C.1:
    //   Tafetán (plain)  → 1212
    //   Sarga 2/2        → 1122 + offset 1/capa (REQUIERE C.2 — hoy offset
    //                      se lista pero no se honra; UI lo muestra gris)
    //   Sarga 3/1        → 1112
    //   Satén 5          → 11112
    //   Houndstooth      → 1122 top + 2211 penu, escritura atómica (C.3)
    // Decisión consciente del plan: phase-shift y tartán NO entran (ver
    // COLOR_PREDICTION §4 — descartados/diferidos).
    static const std::vector<WeavePreset> presets = {
        { "Custom",            "",      "",     0, false,
          "Free pattern string (digits 1-4)" },
        { "Plain (tafetan)",   "1212",  "",     0, false,
          "1:1 alternation - balanced 50/50 blend" },
        { "Twill 2/2 (sarga)", "1122",  "",     1, false,
          "2:2 - diagonal requires per-layer offset (C.2, pending)" },
        { "Twill 3/1",         "1112",  "",     1, false,
          "3:1 - A-dominant; diagonal requires C.2" },
        { "Satin 5 (saten)",   "11112", "",     0, false,
          "4:1 - A-dominant smooth surface, B accent" },
        { "Houndstooth",       "1122",  "2211", 0, true,
          "Writes top+penu as a pair (atomic)" },
    };
    return presets;
}

std::string rotate_pattern(const std::string& pattern, int offset)
{
    const int n = (int)pattern.size();
    if (n == 0)
        return pattern;
    int k = offset % n;
    if (k < 0) k += n;
    if (k == 0)
        return pattern;
    return pattern.substr(k) + pattern.substr(0, k);
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_COLORSCI_TAG_END
