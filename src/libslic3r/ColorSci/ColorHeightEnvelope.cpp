// NEOTKO_ALHCOLOR_TAG_START — Fase 0 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md)
#include "ColorHeightEnvelope.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace ColorSci {

double min_thickness_for_opacity_mm(const Material& m,
                                    int             channel,
                                    float           target_opacity,
                                    double          td_reference_height_mm)
{
    if (channel < 0 || channel > 2)
        return 0.0;

    const double td_ratio = double(m.td[channel]);
    // TD ~0 → material opaco en ese canal → cualquier grosor rinde el color.
    if (td_ratio < 1e-6)
        return 0.0;

    // Convertir TD (fracción de reference height) a mm reales.
    const double td_mm = td_ratio * std::max(1e-6, td_reference_height_mm);

    // Clamp de la opacidad objetivo a un rango invertible: op=1 daría grosor
    // infinito, op<=0 daría grosor 0/negativo.
    const double op = std::clamp(double(target_opacity), 0.01, 0.999);

    //   op = 1 - 0.1^(t/td)  ⇒  0.1^(t/td) = 1-op
    //   (t/td)*log10(0.1) = log10(1-op)  ⇒  t = -td * log10(1-op)
    //   = td * log10(1/(1-op))
    const double t_mm = td_mm * std::log10(1.0 / (1.0 - op));
    return std::max(0.0, t_mm);
}

ColorHeightEnvelope compute_color_height_envelope(const ColorHeightContext& ctx,
                                                  float  target_opacity,
                                                  double mix_band_upper_mm)
{
    ColorHeightEnvelope env;

    const double nozzle_lo = std::max(1e-3, ctx.nozzle_min_height_mm);
    const double nozzle_hi = std::max(nozzle_lo, ctx.nozzle_max_height_mm);

    // --- Sin color: passthrough (envelope = bounds de nozzle sin recortar) ---
    if (ctx.painted_tools.empty()) {
        env.passthrough = true;
        env.h_min = nozzle_lo;
        env.h_max = nozzle_hi;
        env.h_opt = std::clamp(ctx.td_reference_height_mm, nozzle_lo, nozzle_hi);
        return env;
    }

    // --- Suelo por fidelidad: el peor caso (grosor más exigente) entre todos
    // los tools pintados y sus 3 canales. El tool más translúcido / canal con
    // TD mayor manda el suelo. ---
    double fidelity_floor = 0.0;
    for (int tool : ctx.painted_tools) {
        if (tool < 0 || tool > 3)
            continue;
        for (int c = 0; c < 3; ++c) {
            const double t = min_thickness_for_opacity_mm(
                ctx.mats[tool], c, target_opacity, ctx.td_reference_height_mm);
            fidelity_floor = std::max(fidelity_floor, t);
        }
    }

    // --- Suelo por top-surface / sandwich: si esta zona es top y hay que
    // reservar espacio para un sandwich de N passes, la capa debe caber los N
    // passes (cada uno ≥ min_pass, el visible arriba ≥ min_top_visible). ---
    double sandwich_floor = 0.0;
    if (ctx.sandwich_passes >= 2) {
        const double per_pass = std::max(ctx.min_pass_height_mm, 1e-3);
        // (N-1) fillers a min_pass + 1 pass visible a min_top_visible.
        sandwich_floor = double(ctx.sandwich_passes - 1) * per_pass
                       + std::max(ctx.min_top_visible_mm, per_pass);
    }

    double h_min = std::max({ nozzle_lo, fidelity_floor, sandwich_floor });

    // --- Techo por resolución de patrón de mezcla: por encima de la banda de
    // color objetivo el patrón A/B ya no se representa dentro de una capa. ---
    double h_max = nozzle_hi;
    if (mix_band_upper_mm > 1e-6)
        h_max = std::min(h_max, mix_band_upper_mm);

    // --- Conflicto: fidelidad/sandwich exigen más de lo que el patrón/nozzle
    // permite. Se prioriza FIDELIDAD (el color debe leerse) y se avisa. ---
    if (h_min > h_max) {
        env.conflict = true;
        h_max = h_min;                 // abre el techo hasta el suelo
        h_max = std::min(h_max, nozzle_hi); // pero nunca por encima del nozzle
        h_min = std::min(h_min, h_max);
    }

    env.h_min = h_min;
    env.h_max = h_max;
    // Óptimo: el mayor grosor válido (menos capas, misma fidelidad). Si hubo
    // conflicto h_opt = h_max = h_min.
    env.h_opt = h_max;
    return env;
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_ALHCOLOR_TAG_END
