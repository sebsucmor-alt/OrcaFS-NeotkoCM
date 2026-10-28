// NEOTKO_COLORSCI_TAG_START — P0 (Fase A) + GD2
#include "StackFlatten.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r {
namespace ColorSci {

static std::string pass_pattern(const SurfacePass& p, bool penu,
                                const std::string& fallback_pattern)
{
    // a) clave corta del editor per-pass (la que blend_preview_zone mira hoy)
    auto it = p.colorstitch.kv.find("pattern");
    if (it != p.colorstitch.kv.end() && !it->second.empty())
        return it->second;
    // b) claves largas de stacks self-contained (Designer / bake Plan1)
    const char* k = penu ? "interlayer_colormix_pattern_penultimate"
                         : "interlayer_colormix_pattern_top";
    it = p.colorstitch.kv.find(k);
    if (it != p.colorstitch.kv.end() && !it->second.empty())
        return it->second;
    // c) fallback del caller (config viva del diálogo); "" si autocontenido
    return fallback_pattern;
}

std::string pass_pattern_of(const SurfacePass& p, bool penu) { return pass_pattern(p, penu, ""); }

std::vector<Slice> flatten_stack(const SurfacePassStack& st,
                                 bool penu,
                                 const std::string& fallback_pattern)
{
    std::vector<Slice> out;
    if (!st.enabled || st.passes.empty())
        return out;

    for (const SurfacePass& p : st.passes) {
        const float r_pass = std::max(0.f, (float)p.ratio);
        if (r_pass < 1e-6f)
            continue;
        switch (p.kind) {
        case SurfacePassKind::Solid:
            out.push_back({ std::clamp(p.solid_tool, 0, 3), r_pass });
            break;
        case SurfacePassKind::ColorStitch: {
            const std::string pat = pass_pattern(p, penu, fallback_pattern);
            if (pat.empty())
                break;
            // port Tab.cpp:5400-5407 — un slice por tool, peso por frecuencia
            std::map<int, int> cnt;
            int total = 0;
            for (char c : pat) {
                const int t = (int)c - '1';
                if (t >= 0 && t < 4) { cnt[t]++; total++; }
            }
            if (total == 0)
                break;
            for (const auto& [t, n] : cnt)
                out.push_back({ t, r_pass * (float)n / (float)total });
            break;
        }
        case SurfacePassKind::PathBlend: {
            // port Tab.cpp:5410-5421 — 50/50 bottom/top del blob
            int t_bot = 0, t_top = 0;
            auto it = p.pathblend.kv.find("blob");
            if (it != p.pathblend.kv.end() && !it->second.empty()) {
                const auto pb = PathBlendPassConfig::from_blob_json(it->second);
                t_bot = pb.tool_bottom;
                t_top = pb.tool_top;
            }
            out.push_back({ std::clamp(t_bot, 0, 3), r_pass * 0.5f });
            out.push_back({ std::clamp(t_top, 0, 3), r_pass * 0.5f });
            break;
        }
        case SurfacePassKind::None:
            break;
        }
    }
    return out;
}

bool zone_colour(const SurfacePassStack& st,
                 bool penu,
                 const std::string& fallback_pattern,
                 const Material mats[4],
                 float layer_mm,
                 float out_rgb[3],
                 float* out_weight)
{
    const std::vector<Slice> slices = flatten_stack(st, penu, fallback_pattern);
    if (slices.empty()) {
        if (out_weight) *out_weight = 0.f;
        return false;
    }
    blend_parallel(slices, mats, layer_mm, out_rgb, out_weight);
    return true;
}

bool sandwich_colour_legacy(const SurfacePassStack& top,
                            const SurfacePassStack& penu,
                            const std::string& fallback_pattern_top,
                            const std::string& fallback_pattern_penu,
                            const Material mats[4],
                            float layer_mm,
                            float out_rgb[3])
{
    // port de stacked_preview_color (Tab.cpp:5455-5466)
    float c_top[3], c_penu[3], w_top = 0.f;
    const bool has_top  = zone_colour(top,  false, fallback_pattern_top,  mats, layer_mm, c_top, &w_top);
    const bool has_penu = zone_colour(penu, true,  fallback_pattern_penu, mats, layer_mm, c_penu);
    if (!has_top && !has_penu)
        return false;
    if (!has_top)  { c_top[0]  = c_top[1]  = c_top[2]  = 0.f; w_top = 0.f; }
    if (!has_penu) { c_penu[0] = c_penu[1] = c_penu[2] = 0.f; }
    const float a  = std::clamp(w_top, 0.f, 1.f);
    const float ia = 1.f - a;
    for (int c = 0; c < 3; ++c)
        out_rgb[c] = c_top[c] * a + c_penu[c] * ia;
    return true;
}

// s350 — MODELO LÍNEA A LÍNEA. Antes una pasada ColorStitch/PathBlend se colapsaba a UNA capa con
// el color medio y el TD MEDIO (pass_to_layer): la transparencia no es lineal, así que una línea
// opaca + una translúcida no se portan como dos «medio translúcidas» (ej.: 50/50 negro TD 0.3 mm
// + blanco TD 3 mm sobre rojo daba (0.88,0.12,0.12); lo real, línea a línea, (0.61,0.07,0.07)).
// Ahora cada línea de la pasada se compone con su propio filamento y TD.
//
// Beer-Lambert es AFÍN en lo de debajo: acc = E + T·acc_debajo, con E = c·(1−t) y T = t (lineal,
// por canal). Las líneas de pasadas distintas se cruzan en ángulo ⇒ independientes ⇒ la MEDIA
// exacta sobre todas las combinaciones de líneas sale apilando, por pasada, la E media y la T
// media de sus líneas (la esperanza de un producto de independientes = producto de esperanzas).
// Mismo coste que antes y exacto bajo esa hipótesis (el tour, sandwich-builder, asume líneas
// ALINEADAS entre pasadas; coincide con esto cuando sólo hay una pasada rayada).
namespace {
struct Eff { float E[3] = { 0.f, 0.f, 0.f }; float T[3] = { 1.f, 1.f, 1.f }; };

// Una capa maciza de `ratio` (fracción de la altura de capa) del material m.
Eff eff_solid(const Material& m, float ratio)
{
    Eff e;
    for (int c = 0; c < 3; ++c) {
        const float lc = srgb_to_linear(m.rgb[c]);
        if (ratio <= 0.f) { e.E[c] = 0.f; e.T[c] = 1.f; continue; }
        if (m.td[c] < 1e-6f) { e.E[c] = lc; e.T[c] = 0.f; continue; }   // opaco
        const float t = std::pow(0.1f, ratio / m.td[c]);
        e.E[c] = lc * (1.f - t);
        e.T[c] = t;
    }
    return e;
}
// `up` encima de `down`.
Eff eff_over(const Eff& up, const Eff& down)
{
    Eff e;
    for (int c = 0; c < 3; ++c) {
        e.E[c] = up.E[c] + up.T[c] * down.E[c];
        e.T[c] = up.T[c] * down.T[c];
    }
    return e;
}
void eff_acc(Eff& sum, const Eff& e, float w)
{
    for (int c = 0; c < 3; ++c) { sum.E[c] += w * e.E[c]; sum.T[c] += w * e.T[c]; }
}
float ease(int mode, float t)
{
    switch (mode) {
    case 1:  return t * t;
    case 2:  return 1.f - (1.f - t) * (1.f - t);
    case 3:  return t < 0.5f ? 2.f * t * t : 1.f - 2.f * (1.f - t) * (1.f - t);
    default: return t;
    }
}

bool pass_to_eff(const SurfacePass& p, bool penu, const Material mats[4], float layer_mm, Eff& out)
{
    // Grosor en mm (el TD va en mm): fracción de capa × altura de capa.
    const float r = std::max(0.f, (float)p.ratio) * layer_mm;
    if (r < 1e-6f) return false;
    switch (p.kind) {
    case SurfacePassKind::Solid:
        out = eff_solid(mats[std::clamp(p.solid_tool, 0, 3)], r);
        return true;
    case SurfacePassKind::ColorStitch: {
        // Cada línea es de UN filamento y ocupa toda la altura de la pasada; peso = su frecuencia
        // en el patrón real de la receta.
        const std::string pat = pass_pattern(p, penu, "");
        int cnt[4] = { 0, 0, 0, 0 }, total = 0;
        for (char ch : pat) { const int t = (int)ch - '1'; if (t >= 0 && t < 4) { ++cnt[t]; ++total; } }
        if (total == 0) return false;
        Eff sum; for (int c = 0; c < 3; ++c) { sum.E[c] = 0.f; sum.T[c] = 0.f; }
        for (int t = 0; t < 4; ++t)
            if (cnt[t]) eff_acc(sum, eff_solid(mats[t], r), float(cnt[t]) / float(total));
        out = sum;
        return true;
    }
    case SurfacePassKind::PathBlend: {
        // Rampa: en la posición t la línea lleva tool_bottom con altura h = r·ease(t) y, en Full,
        // tool_top encima con el resto (r − h). Media sobre 16 posiciones. (Aproximación: la
        // rampa va de 0 a la pasada entera; floor_mm / mid_end_mm se ignoran — están en mm y aquí
        // las alturas van en fracción de capa.)
        PathBlendPassConfig pb;
        auto it = p.pathblend.kv.find("blob");
        if (it != p.pathblend.kv.end() && !it->second.empty()) pb = PathBlendPassConfig::from_blob_json(it->second);
        const bool  full = pb.mode == PathBlendPassConfig::Mode::Full && pb.tool_top >= 0;
        const Material& mb = mats[std::clamp(pb.tool_bottom, 0, 3)];
        const Material& mt = mats[std::clamp(full ? pb.tool_top : 0, 0, 3)];
        constexpr int K = 16;
        Eff sum; for (int c = 0; c < 3; ++c) { sum.E[c] = 0.f; sum.T[c] = 0.f; }
        for (int k = 0; k < K; ++k) {
            const float h = r * ease(pb.ease_mode, (float(k) + 0.5f) / float(K));
            const Eff bot = eff_solid(mb, h);
            eff_acc(sum, full ? eff_over(eff_solid(mt, r - h), bot) : bot, 1.f / float(K));
        }
        out = sum;
        return true;
    }
    case SurfacePassKind::None:
        break;
    }
    return false;
}
} // namespace

void sandwich_colour_stacked(const SurfacePassStack& top,
                             const SurfacePassStack& penu,
                             const Material mats[4],
                             const float bg_rgb[3],
                             double layer_mm,
                             float out_rgb[3],
                             float* out_transmit)
{
    const float lmm = float(std::max(0.01, layer_mm));
    // penu primero (más profundo), luego top; dentro de cada zona el orden del stack ya es
    // bottom→top (passes[0] = abajo — mismo convenio que el sweep del Designer).
    float acc[3], transmit[3] = { 1.f, 1.f, 1.f };
    for (int c = 0; c < 3; ++c) acc[c] = srgb_to_linear(bg_rgb[c]);
    auto apply = [&](const SurfacePassStack& st, bool is_penu) {
        if (!st.enabled) return;
        for (const SurfacePass& p : st.passes) {
            Eff e;
            if (!pass_to_eff(p, is_penu, mats, lmm, e)) continue;
            for (int c = 0; c < 3; ++c) {
                acc[c]      = e.E[c] + e.T[c] * acc[c];
                transmit[c] = transmit[c] * e.T[c];
            }
        }
    };
    apply(penu, true);
    apply(top, false);
    for (int c = 0; c < 3; ++c) out_rgb[c] = std::clamp(linear_to_srgb(acc[c]), 0.f, 1.f);
    if (out_transmit) *out_transmit = (transmit[0] + transmit[1] + transmit[2]) / 3.f;
}

SurfacePassStack reversed_stack(const SurfacePassStack& st)
{
    SurfacePassStack r = st;
    std::reverse(r.passes.begin(), r.passes.end());
    return r;
}

void sandwich_colour_bottom(const SurfacePassStack& bottom, const Material mats[4],
                            const float bg_rgb[3], double layer_mm, float out_rgb[3])
{
    sandwich_colour_stacked(reversed_stack(bottom), SurfacePassStack{}, mats, bg_rgb, layer_mm, out_rgb);
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_COLORSCI_TAG_END
