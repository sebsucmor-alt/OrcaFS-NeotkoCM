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
                 float out_rgb[3],
                 float* out_weight)
{
    const std::vector<Slice> slices = flatten_stack(st, penu, fallback_pattern);
    if (slices.empty()) {
        if (out_weight) *out_weight = 0.f;
        return false;
    }
    blend_parallel(slices, mats, out_rgb, out_weight);
    return true;
}

bool sandwich_colour_legacy(const SurfacePassStack& top,
                            const SurfacePassStack& penu,
                            const std::string& fallback_pattern_top,
                            const std::string& fallback_pattern_penu,
                            const Material mats[4],
                            float out_rgb[3])
{
    // port de stacked_preview_color (Tab.cpp:5455-5466)
    float c_top[3], c_penu[3], w_top = 0.f;
    const bool has_top  = zone_colour(top,  false, fallback_pattern_top,  mats, c_top, &w_top);
    const bool has_penu = zone_colour(penu, true,  fallback_pattern_penu, mats, c_penu);
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

// Colapsa un pass a una capa Beer-Lambert equivalente (ver nota del .hpp
// sobre la aproximación del td en passes ColorStitch/PathBlend).
static bool pass_to_layer(const SurfacePass& p, bool penu,
                          const Material mats[4], Layer& out)
{
    const float r_pass = std::max(0.f, (float)p.ratio);
    if (r_pass < 1e-6f)
        return false;

    if (p.kind == SurfacePassKind::Solid) {
        const Material& m = mats[std::clamp(p.solid_tool, 0, 3)];
        out.rgb = m.rgb;
        out.td  = m.td;
        out.ratio = r_pass;
        return true;
    }
    if (p.kind == SurfacePassKind::None)
        return false;

    // ColorStitch / PathBlend: franjas → color paralelo + td medio ponderado.
    SurfacePassStack one;
    one.enabled = true;
    one.passes.push_back(p);
    const std::vector<Slice> slices = flatten_stack(one, penu, "");
    if (slices.empty())
        return false;

    float rgb[3];
    blend_parallel(slices, mats, rgb);
    out.rgb = { rgb[0], rgb[1], rgb[2] };

    float wsum = 0.f, td[3] = { 0.f, 0.f, 0.f };
    for (const Slice& s : slices) {
        const Material& m = mats[std::clamp(s.tool, 0, 3)];
        for (int c = 0; c < 3; ++c)
            td[c] += m.td[c] * s.ratio;
        wsum += s.ratio;
    }
    if (wsum > 1e-6f)
        for (int c = 0; c < 3; ++c)
            td[c] /= wsum;
    out.td    = { td[0], td[1], td[2] };
    out.ratio = r_pass;
    return true;
}

void sandwich_colour_stacked(const SurfacePassStack& top,
                             const SurfacePassStack& penu,
                             const Material mats[4],
                             const float bg_rgb[3],
                             float out_rgb[3],
                             float* out_transmit)
{
    std::vector<Layer> layers;
    layers.reserve(SurfacePassStack::kMaxPasses * 2);
    // penu primero (más profundo), luego top; dentro de cada zona el orden
    // del stack ya es bottom→top (passes[0] = abajo — mismo convenio que el
    // sweep del Designer: [SOLID B, SOLID A]).
    if (penu.enabled)
        for (const SurfacePass& p : penu.passes) {
            Layer l;
            if (pass_to_layer(p, true, mats, l))
                layers.push_back(l);
        }
    if (top.enabled)
        for (const SurfacePass& p : top.passes) {
            Layer l;
            if (pass_to_layer(p, false, mats, l))
                layers.push_back(l);
        }
    blend_stacked(layers, bg_rgb, out_rgb, out_transmit);
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_COLORSCI_TAG_END
