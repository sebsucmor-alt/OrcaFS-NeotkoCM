// Receta por PROFUNDIDAD del Sticker (Depth recipe), F1 + F2. Ver DepthStack.hpp.
#include "DepthStack.hpp"

#include "ColorSci.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <functional>

namespace Slic3r {
namespace DepthStack {

std::array<float, 3> td_per_channel(float td_mm, const std::array<float, 3>& rgb)
{
    // TD por canal, la MISMA cuenta que RealView Ray (RealViewShaders.swift, «TD con color»):
    // cada canal se apaga según lo que el color no refleja; la media sigue cumpliendo el TD.
    if (td_mm <= 0.f)
        return { 0.f, 0.f, 0.f };
    float k[3];
    for (int c = 0; c < 3; ++c)
        k[c] = -std::log(std::max(ColorSci::srgb_to_linear(rgb[c]), 0.02f)) + 0.05f;
    const float mk = (k[0] + k[1] + k[2]) / 3.f;
    std::array<float, 3> out;
    for (int c = 0; c < 3; ++c)
        out[c] = td_mm / (0.15f + 0.85f * k[c] / mk);
    return out;
}

static ColorSci::Layer make_layer(const Filament& f, float thickness_mm)
{
    ColorSci::Layer l;
    l.rgb   = f.rgb;
    l.td    = td_per_channel(f.td_mm, f.rgb);
    l.ratio = thickness_mm;
    return l;
}

std::array<float, 3> predict(const std::vector<Block>& blocks,
                             const std::vector<Filament>& colours,
                             const Filament& base,
                             float layer_mm)
{
    // Capas de abajo arriba, una por capa real.
    std::vector<const Filament*> layers;
    for (const Block& b : blocks) {
        if (b.layers <= 0)
            continue;
        auto it = std::find_if(colours.begin(), colours.end(), [&b](const Filament& f) { return f.tool == b.tool; });
        if (it == colours.end())
            continue;
        for (int i = 0; i < b.layers; ++i)
            layers.push_back(&*it);
    }
    // 5-nov-2026 — misma cuenta que RealView Ray (RealViewShaders.swift), de arriba abajo: cada capa deja pasar
    // tc = 10^(−grosor/TD·w) por canal y además ESPARCE (difusión según el TD: entera hasta ~1,5 mm, baja hasta
    // ~4,5 mm, mínimo 0,12) — lo esparcido sale con el color de la capa. Sin difusión todo lo apilado salía oscuro
    // y 5 mm de sólido no se distinguían de nada; con ella el plástico devuelve su color, como en la pieza.
    float c[3] = { 0.f, 0.f, 0.f }, paso[3] = { 1.f, 1.f, 1.f };
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const Filament& f = **it;
        if (f.td_mm <= 0.f) {   // opaca: tapa lo de debajo
            for (int k = 0; k < 3; ++k) { c[k] += paso[k] * ColorSci::srgb_to_linear(f.rgb[k]); paso[k] = 0.f; }
            break;
        }
        const std::array<float, 3> td = td_per_channel(f.td_mm, f.rgb);
        const float D = std::max(std::clamp((4.5f - f.td_mm) / 3.f, 0.f, 1.f), 0.12f);
        const float esparce = 1.f - std::exp(-layer_mm * D * 5.f);
        for (int k = 0; k < 3; ++k) {
            const float tc = std::pow(0.1f, layer_mm / std::max(td[k], 1e-6f));
            c[k]    += paso[k] * (esparce + (1.f - esparce) * (1.f - tc)) * ColorSci::srgb_to_linear(f.rgb[k]);
            paso[k] *= (1.f - esparce) * tc;
        }
    }
    // La base, opaca, con lo que llegue.
    for (int k = 0; k < 3; ++k)
        c[k] += paso[k] * ColorSci::srgb_to_linear(base.rgb[k]);
    return { ColorSci::linear_to_srgb(c[0]), ColorSci::linear_to_srgb(c[1]), ColorSci::linear_to_srgb(c[2]) };
}

namespace {
// Una capa de grosor d: reflexión y transmisión por canal (lineal), Kubelka-Munk.
struct KMLayer { float R[3] = { 0, 0, 0 }, T[3] = { 1, 1, 1 }; bool opaque = false; float Ro[3] = { 0, 0, 0 }; };

static void km_rt(float ks, float u, float& R, float& T)   // ks = K/S, u = S·d
{
    const float a = 1.f + ks;
    const float b = std::sqrt(std::max(a * a - 1.f, 0.f));
    if (b < 1e-4f) { R = u / (1.f + u); T = 1.f / (1.f + u); return; }
    const float bu = std::min(b * u, 40.f);
    const float sh = std::sinh(bu), ch = std::cosh(bu), den = a * sh + b * ch;
    R = sh / den;
    T = b / den;
}

static KMLayer km_layer(const Filament& f, float layer_mm, float force)
{
    KMLayer L;
    if (f.td_mm <= 0.f) {
        L.opaque = true;
        for (int c = 0; c < 3; ++c) L.Ro[c] = ColorSci::srgb_to_linear(f.rgb[c]);
        return L;
    }
    const KMMedium m = km_medium(f.rgb, f.td_mm, force);
    for (int c = 0; c < 3; ++c) km_rt(m.ks[c], m.S * layer_mm, L.R[c], L.T[c]);
    return L;
}

} // namespace

KMMedium km_medium(const std::array<float, 3>& rgb, float td_mm, float force)
{
    KMMedium m;
    float ks_min = 1e9f;
    for (int c = 0; c < 3; ++c) {
        const float r = std::clamp(ColorSci::srgb_to_linear(rgb[c]), 0.01f, 0.98f);
        m.ks[c] = (1.f - r) * (1.f - r) / (2.f * r) * std::max(force, 0.f);
        const float a = 1.f + m.ks[c];
        m.rinf[c] = a - std::sqrt(std::max(a * a - 1.f, 0.f));
        ks_min = std::min(ks_min, m.ks[c]);
    }
    if (td_mm <= 1e-6f)
        return m;
    // S·TD tal que el canal más claro (el de menos K/S) deje pasar el 10 % a través del TD.
    float lo = 1e-3f, hi = 1e4f;
    for (int it = 0; it < 50; ++it) {
        const float mid = std::sqrt(lo * hi);
        float R, T;
        km_rt(ks_min, mid, R, T);
        (T > 0.1f ? lo : hi) = mid;
    }
    m.S = std::sqrt(lo * hi) / td_mm;
    return m;
}

namespace {
static std::array<float, 3> km_stack(const std::vector<Block>& blocks, const std::vector<int>& idx,
                                     const std::vector<KMLayer>& layers, const Filament& base)
{
    // De abajo arriba: lo de debajo (base opaca) visto a través de cada capa.
    float Rb[3];
    for (int c = 0; c < 3; ++c) Rb[c] = ColorSci::srgb_to_linear(base.rgb[c]);
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (idx[i] < 0 || blocks[i].layers <= 0)
            continue;
        const KMLayer& L = layers[size_t(idx[i])];
        for (int n = 0; n < blocks[i].layers; ++n)
            for (int c = 0; c < 3; ++c)
                Rb[c] = L.opaque ? L.Ro[c] : L.R[c] + L.T[c] * L.T[c] * Rb[c] / std::max(1.f - L.R[c] * Rb[c], 1e-6f);
    }
    return { ColorSci::linear_to_srgb(Rb[0]), ColorSci::linear_to_srgb(Rb[1]), ColorSci::linear_to_srgb(Rb[2]) };
}

static std::vector<int> km_index(const std::vector<Block>& blocks, const std::vector<Filament>& colours)
{
    std::vector<int> idx;
    for (const Block& b : blocks) {
        auto it = std::find_if(colours.begin(), colours.end(), [&b](const Filament& f) { return f.tool == b.tool; });
        idx.push_back(it == colours.end() ? -1 : int(it - colours.begin()));
    }
    return idx;
}
} // namespace

std::array<float, 3> predict_lupa(const std::vector<Block>& blocks,
                                  const std::vector<Filament>& colours,
                                  const Filament& base,
                                  float layer_mm,
                                  float force)
{
    std::vector<KMLayer> layers;
    for (const Filament& f : colours) layers.push_back(km_layer(f, layer_mm, force));
    return km_stack(blocks, km_index(blocks, colours), layers, base);
}

static float de2000(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    const float fa[3] = { a[0], a[1], a[2] };
    const float fb[3] = { b[0], b[1], b[2] };
    return ColorSci::delta_e2000(ColorSci::rgb_to_lab(fa), ColorSci::rgb_to_lab(fb));
}

Result solve(const std::array<float, 3>& target,
             const std::vector<Filament>& colours,
             const Filament& base,
             const Options& opt)
{
    Result best_any;    // menor ΔE (a igualdad, menos capas)
    Result best_stop;   // menos capas con ΔE ≤ stop_de
    bool   have_stop = false;

    const int k = int(colours.size());
    std::vector<KMLayer> km_layers;   // la Lupa: cada filamento a una capa, calculado una vez
    if (opt.lupa)
        for (const Filament& f : colours) km_layers.push_back(km_layer(f, opt.layer_mm, opt.lupa_force));
    const int max_layers = (opt.layer_mm > 0.f) ? int(std::floor(opt.max_depth_mm / opt.layer_mm + 1e-4f)) : 0;

    std::vector<Result> best_by_depth(size_t(std::max(0, max_layers)) + 1);   // índice = nº de capas
    auto consider = [&](const std::vector<Block>& blocks, int n_layers) {
        Result r;
        r.blocks   = blocks;
        r.rgb      = opt.lupa ? km_stack(blocks, km_index(blocks, colours), km_layers, base)
                               : predict(blocks, colours, base, opt.layer_mm);
        r.delta_e  = de2000(r.rgb, target);
        r.depth_mm = float(n_layers) * opt.layer_mm;
        if (size_t(n_layers) < best_by_depth.size() && r.delta_e < best_by_depth[size_t(n_layers)].delta_e)
            best_by_depth[size_t(n_layers)] = r;
        if (r.delta_e < best_any.delta_e - 1e-6f
            || (std::abs(r.delta_e - best_any.delta_e) <= 1e-6f && r.depth_mm < best_any.depth_mm))
            best_any = r;
        if (r.delta_e <= opt.stop_de
            && (!have_stop || r.depth_mm < best_stop.depth_mm - 1e-6f
                || (std::abs(r.depth_mm - best_stop.depth_mm) <= 1e-6f && r.delta_e < best_stop.delta_e))) {
            best_stop = r;
            have_stop = true;
        }
    };

    consider({}, 0);   // la base sola

    // 6-nov-2026 — antes recorría (max_layers+1)^k combinaciones y descartaba las que se pasaban: con 10 mm
    // (50 capas) y 4 filamentos eran ~160 millones de predicciones en el hilo de la interfaz (la app «spinning»).
    // Ahora: por profundidad CRECIENTE, solo los repartos que suman exactamente n capas, y se para al acabar la
    // primera profundidad que ya llega (ΔE ≤ stop_de). Mismo resultado: gana la menor profundidad que llega.
    std::vector<int> order(k);
    std::iota(order.begin(), order.end(), 0);
    std::vector<int> counts(k, 0);
    std::function<void(int, int, int)> split = [&](int j, int left, int n) {
        if (j == k - 1) {
            counts[j] = left;
            std::vector<Block> blocks;
            for (int q = 0; q < k; ++q)
                if (counts[order[q]] > 0)
                    blocks.push_back({ colours[order[q]].tool, counts[order[q]] });
            consider(blocks, n);
            return;
        }
        for (int c = 0; c <= left; ++c) { counts[j] = c; split(j + 1, left - c, n); }
    };
    // 7-nov-2026 — y si NO llega (MF inalcanzable, ΔE 14) seguía hasta el máximo: con 10 mm, 4 filamentos y todos
    // los órdenes eran millones de predicciones al abrir el 3mf. Ahora para cuando 6 capas seguidas no mejoran la
    // mejor en ≥ 0,25 ΔE: el freno de abajo se queda igualmente con la más fina a menos de 1 ΔE de la mejor.
    float best_seen = 1e9f;
    int   last_gain = 0;
    for (int n = 1; k > 0 && n <= max_layers; ++n) {
        std::iota(order.begin(), order.end(), 0);
        do {
            split(0, n, n);
        } while (!opt.fixed_order && std::next_permutation(order.begin(), order.end()));
        if (have_stop)
            break;
        if (best_any.delta_e < best_seen - 0.25f) { best_seen = best_any.delta_e; last_gain = n; }
        else if (n - last_gain >= 6)
            break;
    }

    // 6-nov-2026 — FRENO: si nada llega a stop_de, no gastar toda la profundidad (y filamentos raros) por mejorar
    // décimas. Se queda la receta MÁS FINA a menos de 1 ΔE de la mejor. Hace falta repasar las profundidades,
    // así que se guardan las mejores por profundidad mientras se recorren.
    Result out = have_stop ? best_stop : best_any;
    if (!have_stop) {
        for (const Result& r : best_by_depth)
            if (r.delta_e <= best_any.delta_e + 1.f) { out = r; break; }
    }
    if (k < 3)
        out.warning = "fewer than 3 colour filaments";
    return out;
}

} // namespace DepthStack
} // namespace Slic3r
