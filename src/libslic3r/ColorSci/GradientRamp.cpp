// NEOTKO_COLORSCI_TAG_START — GD1 (P1)
#include "GradientRamp.hpp"

#include <algorithm>
#include <cmath>

#include "../LocalesUtils.hpp"   // float_to_string_decimal_point — locale-safe

namespace Slic3r {
namespace ColorSci {

GradientSpec sanitize(const GradientSpec& spec, std::vector<std::string>* warnings)
{
    GradientSpec s = spec;
    auto warn = [&](const std::string& msg) {
        if (warnings) warnings->push_back(msg);
    };

    if (s.steps < 1) { s.steps = 1; warn("steps < 1 → 1"); }
    if (s.layer_height <= 0.0) { s.layer_height = 0.2; warn("layer_height <= 0 → 0.2"); }

    const double hi = std::max(kMinSweepMM, s.layer_height - kMinSweepMM);
    auto clamp_split = [&](double v, const char* name) {
        const double cv = std::clamp(v, kMinSweepMM, hi);
        if (std::abs(cv - v) > 1e-9)
            warn(std::string(name) + " fuera de [" +
                 float_to_string_decimal_point(kMinSweepMM, 2) + ", " +
                 float_to_string_decimal_point(hi, 2) + "] → clampeado");
        return cv;
    };
    s.split_min_mm = clamp_split(s.split_min_mm, "split_min");
    s.split_max_mm = clamp_split(s.split_max_mm, "split_max");
    if (s.split_min_mm > s.split_max_mm) {
        std::swap(s.split_min_mm, s.split_max_mm);
        warn("split_min > split_max → intercambiados");
    }

    std::string pat;
    for (char c : s.penu_pattern)
        if (c >= '1' && c <= '4') pat += c;
    if (pat != s.penu_pattern)
        warn("pattern filtrado a dígitos 1-4");
    if (pat.empty()) { pat = "12"; warn("pattern vacío → \"12\""); }
    s.penu_pattern = pat;

    s.tool_a = std::clamp(s.tool_a, 0, 3);
    s.tool_b = std::clamp(s.tool_b, 0, 3);
    return s;
}

SurfacePass make_penu_colorstitch_pass(const GradientSpec& s)
{
    // port de make_penu_colormix_pass + DEFAULT_PENU_KV_EXTRAS
    // (gen_gradient_grid.py:56-99). Mismos valores, mismas claves — el kv es
    // el esquema canónico capturado de 8rectangles.3mf.
    SurfacePass p;
    p.kind       = SurfacePassKind::ColorMix;
    p.ratio      = 1.0;
    p.solid_tool = s.tool_a;                 // fallback tool — mirrors 3mf
    p.angle      = -1;
    p.fan        = -1;
    p.speed_pct  = 100;
    p.colormix.present = true;

    auto& kv = p.colormix.kv;
    kv["interlayer_colormix_pattern_penultimate"]      = s.penu_pattern;
    kv["interlayer_colormix_penu_tool_a"]              = std::to_string(s.tool_a);
    kv["interlayer_colormix_penu_tool_b"]              = std::to_string(s.tool_b);
    kv["interlayer_colormix_penu_tool_c"]              = "2";
    kv["interlayer_colormix_penu_tool_d"]              = "3";
    kv["interlayer_colormix_penu_band_count_a"]        = std::to_string(s.band_a);
    kv["interlayer_colormix_penu_band_count_b"]        = std::to_string(s.band_b);
    kv["interlayer_colormix_penu_band_count_c"]        = "0";
    kv["interlayer_colormix_penu_band_count_d"]        = "0";
    kv["interlayer_colormix_penu_pct_a"]               = std::to_string(s.pct_a);
    kv["interlayer_colormix_penu_pct_b"]               = std::to_string(s.pct_b);
    // precision -1 = representación mínima ("0.6", no "0.60") — paridad
    // byte-a-byte con el fixture Python (validado contra /tmp/gd_ref.3mf).
    kv["interlayer_colormix_penu_overlap"]             = float_to_string_decimal_point(s.overlap);
    kv["interlayer_colormix_penu_gamma"]               = std::to_string(s.gamma);
    kv["interlayer_colormix_penu_easing"]              = "0";
    kv["interlayer_colormix_penu_invert"]              = "0";
    kv["interlayer_colormix_penu_mode"]                = "0";
    kv["interlayer_colormix_penu_repetitions"]         = std::to_string(s.repetitions);
    kv["interlayer_colormix_penu_min_surface_lines"]   = "3";
    kv["interlayer_colormix_penu_angle"]               = "-1";
    return p;
}

static SurfacePass make_solid_top_pass(int tool, double ratio)
{
    // port de make_solid_top_pass (gen_gradient_grid.py:102). El Python marca
    // colormix.present=true en el pass inferior solo por mimetizar el 3mf de
    // referencia ("harmless"); aquí NO se replica — kv vacío + present=false
    // es el estado limpio y el engine no lo distingue (fallback solo aplica a
    // passes kind=ColorMix).
    SurfacePass p;
    p.kind       = SurfacePassKind::Solid;
    p.ratio      = ratio;
    p.solid_tool = std::clamp(tool, 0, 3);
    p.angle      = -1;
    p.fan        = -1;
    p.speed_pct  = 100;
    return p;
}

std::vector<GradientStep> build_ramp(const GradientSpec& spec)
{
    const GradientSpec s = sanitize(spec);

    // s120: el gradiente es un helper TOP-only — solo reparto A/B en el top.
    // YA NO se le asigna penu ColorStitch (era el origen del bug: el generador
    // se construyó partiendo de "Mixed approximation", que mete un dither de
    // los tools del patrón (0/1 absolutos) ajeno al A/B elegido → contaminaba
    // preview Y slice). Si el usuario quiere un penu para ajustar el tono, lo
    // añade él después como un plus.
    std::vector<GradientStep> out;
    out.reserve(s.steps);
    for (int i = 0; i < s.steps; ++i) {
        // port de compute_sweep (gen_gradient_grid.py:130-143)
        const double f    = s.steps < 2 ? 0.0 : (double)i / (s.steps - 1);
        const double b_mm = s.split_min_mm + f * (s.split_max_mm - s.split_min_mm);
        const double a_mm = s.split_max_mm - f * (s.split_max_mm - s.split_min_mm);

        SurfacePassStack top;
        top.enabled = true;
        top.passes.push_back(make_solid_top_pass(s.tool_b, b_mm / s.layer_height));
        top.passes.push_back(make_solid_top_pass(s.tool_a, a_mm / s.layer_height));

        GradientStep step;
        step.a_mm = a_mm;
        step.b_mm = b_mm;
        step.top  = std::move(top);
        step.penu.enabled = false;  // s120: gradiente top-only (sin penu)
        out.push_back(std::move(step));
    }
    return out;
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_COLORSCI_TAG_END
