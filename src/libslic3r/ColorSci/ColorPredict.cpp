// NEOTKO_COLORSCI_TAG_START — Predict (ronda Flat/Mixed)
#include "ColorPredict.hpp"
#include "StackFlatten.hpp"   // sandwich_colour_stacked

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Slic3r {
namespace ColorSci {

// --- helpers de construcción de stacks -------------------------------------

static SurfacePass solid_pass(int tool, double ratio)
{
    SurfacePass p;
    p.kind       = SurfacePassKind::Solid;
    p.solid_tool = std::clamp(tool, 0, 3);
    p.ratio      = ratio;
    p.angle = -1; p.fan = -1; p.speed_pct = 100;
    return p;
}

// Penu ColorStitch de 2 tools — dither óptico "AB". kv canónico mínimo (el
// preview solo necesita el pattern; el resto son defaults que el engine
// también asume). Self-contained para que el painter Fase 6b lo resuelva.
static SurfacePassStack penu_dither(int a, int b)
{
    SurfacePass p;
    p.kind       = SurfacePassKind::ColorMix;
    p.ratio      = 1.0;
    p.solid_tool = a;
    p.angle = -1; p.fan = -1; p.speed_pct = 100;
    p.colormix.present = true;
    const std::string pat = std::to_string(a + 1) + std::to_string(b + 1);
    p.colormix.kv["interlayer_colormix_pattern_penultimate"] = pat;
    p.colormix.kv["interlayer_colormix_penu_tool_a"] = std::to_string(a);
    p.colormix.kv["interlayer_colormix_penu_tool_b"] = std::to_string(b);
    SurfacePassStack st;
    st.enabled = true;
    st.passes.push_back(p);
    return st;
}

static SurfacePassStack solid_top(int tool, double ratio)
{
    SurfacePassStack st;
    st.enabled = true;
    st.passes.push_back(solid_pass(tool, ratio));
    return st;
}

// --- dedup por ΔE2000 ------------------------------------------------------

static void dedup_and_sort(std::vector<ColorRecipe>& v, const PredictOptions& opt)
{
    // Greedy: ordenar por L (luminosidad) y descartar vecinos con ΔE < umbral.
    std::sort(v.begin(), v.end(), [](const ColorRecipe& a, const ColorRecipe& b) {
        return rgb_to_lab(a.rgb.data()).L < rgb_to_lab(b.rgb.data()).L;
    });
    std::vector<ColorRecipe> out;
    for (const ColorRecipe& r : v) {
        bool dup = false;
        const Lab la = rgb_to_lab(r.rgb.data());
        for (const ColorRecipe& k : out) {
            if (delta_e2000(la, rgb_to_lab(k.rgb.data())) < opt.dedup_de) { dup = true; break; }
        }
        if (!dup) out.push_back(r);
        if ((int)out.size() >= opt.max_entries) break;
    }
    v.swap(out);
}

static void predict_recipe_colour(ColorRecipe& r, const Material mats[4],
                                  const PredictOptions& opt)
{
    sandwich_colour_stacked(r.top, r.penu, mats, opt.bg_rgb, r.rgb.data());
}

// --- Paletas browse --------------------------------------------------------

std::vector<ColorRecipe> predict_flat_palette(const Material mats[4],
                                              const PredictOptions& opt)
{
    std::vector<ColorRecipe> v;
    const SurfacePassStack none_penu;   // vacío

    // 1 solid: filamento puro (ratio 1).
    for (int t = 0; t < 4; ++t) {
        ColorRecipe r;
        r.top  = solid_top(t, 1.0);
        r.penu = none_penu;
        r.desc = "T" + std::to_string(t + 1);
        predict_recipe_colour(r, mats, opt);
        v.push_back(std::move(r));
    }
    // 2 solids apilados: bottom B + top A, sweep de ratio. El top A (visible
    // arriba) ≥ min_top_visible_mm; el filler B (abajo) ≥ min_pass_mm.
    const int steps = std::max(2, opt.ratio_steps);
    const double ra_lo = opt.min_top_visible_mm / opt.layer_height;   // top ≥ 0.05
    const double ra_hi = 1.0 - opt.min_pass_mm   / opt.layer_height;  // bottom ≥ 0.04
    if (ra_hi > ra_lo)
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) {
                if (a == b) continue;
                for (int s = 0; s <= steps; ++s) {
                    const double ra = ra_lo + (ra_hi - ra_lo) * (double)s / steps;  // grosor top A
                    ColorRecipe r;
                    r.top.enabled = true;
                    r.top.passes.push_back(solid_pass(b, 1.0 - ra));  // abajo (relleno)
                    r.top.passes.push_back(solid_pass(a, ra));        // arriba (visible)
                    r.penu = none_penu;
                    r.desc = "T" + std::to_string(b + 1) + "+T" + std::to_string(a + 1);
                    predict_recipe_colour(r, mats, opt);
                    v.push_back(std::move(r));
                }
            }
    dedup_and_sort(v, opt);
    return v;
}

std::vector<ColorRecipe> predict_mixed_palette(const Material mats[4],
                                               const PredictOptions& opt)
{
    std::vector<ColorRecipe> v;
    const int steps = std::max(2, opt.ratio_steps);
    // El pass fino VISIBLE arriba ≥ min_top_visible_mm (0.05 seguro); el
    // relleno abajo ≥ min_pass_mm (0.04). Ej. capa 0.20 → fino ∈ [0.05,0.16],
    // relleno ∈ [0.04,0.15] (el caso "0.16/0.04" se cae fuera por diseño).
    const double lo = opt.min_top_visible_mm / opt.layer_height;
    const double hi = std::max(lo, 1.0 - opt.min_pass_mm / opt.layer_height);

    // Base penu dither(A,B) + TOP COMPLETO de 2 passes que suman la altura de
    // capa (Σ=1.0) — NUNCA un solo pass fino (dejaría la top surface sin cerrar,
    // capas cortadas que no conectan con lo de arriba). Espejo de la estructura
    // del gradient (GradientRamp), respetando el orden físico:
    //   passes[0] = ABAJO (junto al penu, relleno grueso "el resto")
    //   passes[1] = ARRIBA (lo que ve el ojo, fino translúcido)
    // El fino arriba deja "colar" la base dither por TD → gamut extendido,
    // pero la superficie queda completa y printable. Parejas ORDENADAS (a,b)
    // a≠b → cubre "a fino sobre b" y "b fino sobre a".
    for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b) {
            if (a == b) continue;
            const SurfacePassStack penu = penu_dither(a, b);
            for (int s = 0; s <= steps; ++s) {
                const double thin = lo + (hi - lo) * (double)s / steps;  // grosor A visible
                ColorRecipe r;
                r.top.enabled = true;
                r.top.passes.push_back(solid_pass(b, 1.0 - thin));  // [0] abajo = relleno (resto)
                r.top.passes.push_back(solid_pass(a, thin));        // [1] arriba = fino visible
                r.penu = penu;
                r.desc = "T" + std::to_string(a + 1) + std::to_string(b + 1)
                       + " base · cap T" + std::to_string(a + 1)
                       + "/T" + std::to_string(b + 1);
                predict_recipe_colour(r, mats, opt);
                v.push_back(std::move(r));
            }
        }
    dedup_and_sort(v, opt);
    return v;
}

// --- Match inverso ---------------------------------------------------------

ColorRecipe suggest_flat(const float target_rgb[3],
                         const Material mats[4],
                         const PredictOptions& opt)
{
    const Lab tlab = rgb_to_lab(target_rgb);
    ColorRecipe best;
    best.delta_e = 1e9f;
    // Reusar la enumeración flat (incluye 1 y 2 solids) — barata.
    PredictOptions wide = opt;
    wide.max_entries = 1 << 30;     // sin dedup agresivo: queremos el mejor
    wide.dedup_de    = 0.f;
    wide.ratio_steps = std::max(opt.ratio_steps, 12);
    std::vector<ColorRecipe> cand = predict_flat_palette(mats, wide);
    for (ColorRecipe& r : cand) {
        const float de = delta_e2000(tlab, rgb_to_lab(r.rgb.data()));
        if (de < best.delta_e) { best = r; best.delta_e = de; }
    }
    return best;
}

ColorRecipe suggest_mixed(const float target_rgb[3],
                          const Material mats[4],
                          const PredictOptions& opt)
{
    const Lab tlab = rgb_to_lab(target_rgb);
    ColorRecipe best;
    best.delta_e = 1e9f;
    PredictOptions wide = opt;
    wide.max_entries = 1 << 30;
    wide.dedup_de    = 0.f;
    wide.ratio_steps = std::max(opt.ratio_steps, 12);
    std::vector<ColorRecipe> cand = predict_mixed_palette(mats, wide);
    for (ColorRecipe& r : cand) {
        const float de = delta_e2000(tlab, rgb_to_lab(r.rgb.data()));
        if (de < best.delta_e) { best = r; best.delta_e = de; }
    }
    return best;
}

// --- Dispatcher (PR.1) -----------------------------------------------------

std::vector<ColorRecipe> build_palette(PaletteKind kind,
                                       const Material mats[4],
                                       const PredictOptions& opt,
                                       const GradientSpec* ramp)
{
    switch (kind) {
    case PaletteKind::Flat:
        return predict_flat_palette(mats, opt);
    case PaletteKind::Mixed:
        return predict_mixed_palette(mats, opt);
    case PaletteKind::GradientRamp: {
        std::vector<ColorRecipe> out;
        if (!ramp) return out;
        // Misma matemática que el modo Gradient manual del Designer: cada
        // GradientStep de build_ramp se envuelve en un ColorRecipe homogéneo
        // (rgb = composición física apilada). build_ramp es la ÚNICA fuente
        // del cálculo de la rampa — no se duplica aquí.
        const std::vector<GradientStep> steps = build_ramp(*ramp);
        out.reserve(steps.size());
        for (const GradientStep& g : steps) {
            ColorRecipe r;
            r.top  = g.top;
            r.penu = g.penu;
            sandwich_colour_stacked(g.top, g.penu, mats, opt.bg_rgb, r.rgb.data());
            char buf[48];
            std::snprintf(buf, sizeof(buf), "A %.2f / B %.2f mm", g.a_mm, g.b_mm);
            r.desc = buf;
            out.push_back(std::move(r));
        }
        return out;
    }
    }
    return {};
}

// --- MixedFilament Object mode (NEOTKO_MIXEDFIL_SANDWICH_TAG) ---------------

ColorRecipe build_mixed_filament_recipe(const MixedFilament& mf,
                                        size_t num_physical,
                                        const Material mats[4],
                                        const PredictOptions& opt)
{
    // Target colour: TD-aware side-by-side blend of component_a/component_b by
    // mix_b_percent (the user's configured intent), NOT the naive RGB blend
    // that compute_mixed_filament_display_color() uses for the UI swatch list.
    const int mix_b = std::clamp(mf.mix_b_percent, 0, 100);
    const int a = std::clamp<int>((int)mf.component_a - 1, 0, 3);
    const int b = std::clamp<int>((int)mf.component_b - 1, 0, 3);
    (void)num_physical;   // component ids already validated by the caller
    std::vector<Slice> slices;
    slices.push_back({ a, (100 - mix_b) / 100.f });
    slices.push_back({ b, mix_b / 100.f });
    float target_rgb[3];
    blend_parallel(slices, mats, target_rgb);

    ColorRecipe best = suggest_flat(target_rgb, mats, opt);

    // suggest_flat only ever returns 1-2 solid passes; pad to 3 by splitting the
    // bottom-most pass in place (same tool, halved ratio each half) — exact under
    // Beer-Lambert (T(r1)*T(r2) == T(r1+r2) for the same material), so this never
    // changes the predicted colour, it only satisfies "up to 3 solid passes".
    while (best.top.passes.size() < 3 && !best.top.passes.empty()) {
        SurfacePass& bottom = best.top.passes.front();
        const double half = bottom.ratio / 2.0;
        SurfacePass extra = bottom;
        bottom.ratio = half;
        extra.ratio  = half;
        best.top.passes.insert(best.top.passes.begin(), extra);
    }
    best.top.enabled = true;
    best.top.perimeter_override = true;
    // Top and penultimate must look identical for this mode.
    best.penu = best.top;
    return best;
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_COLORSCI_TAG_END
