// NEOTKO_GRAVITY_TAG s362 — ver GravityLayering.hpp.

#include "GravityLayering.hpp"

#include "libslic3r/Slicing.hpp"
#include "libslic3r/NeoDebug.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace Slic3r {
namespace Gravity {

static void dbg(const std::string &msg)
{
    if (NeoDebug::enabled(NeoDebug::GRAVITY))
        NeoDebug::write(NeoDebug::GRAVITY, msg);
}

// Ajusta las últimas capas REALES (desde el índice `first_real`) para que la serie acabe en `z_top`.
// Elige entre encoger lo que hay o quitar la última y estirar: lo que menos cambie cada capa. Todo
// o nada: si alguna capa se saldría de [min, max], no toca.
static bool land_top(const SlicingParameters &sp, std::vector<coordf_t> &layers, size_t first_real,
                     double z_top)
{
    const size_t n = layers.size() / 2;
    if (n <= first_real)
        return false;

    // Candidata A: la serie tal cual. Candidata B: sin la última capa (si sólo sobresalía).
    std::vector<coordf_t> a = layers, b = layers;
    b.resize(b.size() - 2);
    const double gap_a = z_top - a.back();
    const bool   has_b = b.size() / 2 > first_real;
    const double gap_b = has_b ? z_top - b.back() : std::numeric_limits<double>::max();
    const size_t k_a   = std::min<size_t>(5, a.size() / 2 - first_real);
    const size_t k_b   = has_b ? std::min<size_t>(5, b.size() / 2 - first_real) : 1;
    const bool   b_first = has_b && std::abs(gap_b) / double(k_b) < std::abs(gap_a) / double(k_a);

    for (int pass = 0; pass < 2; ++pass) {
        const bool use_b = (pass == 0) == b_first;
        if (use_b && !has_b)
            continue;
        std::vector<coordf_t> s = use_b ? b : a;
        const size_t m   = s.size() / 2;
        const double gap = z_top - s.back();
        if (std::abs(gap) < EPSILON) {
            layers = std::move(s);
            return true;
        }
        const size_t k   = std::min<size_t>(5, m - first_real);
        const double per = gap / double(k);
        bool ok = true;
        for (size_t i = m - k; i < m && ok; ++i) {
            const double h = s[2 * i + 1] - s[2 * i] + per;
            ok = h >= sp.min_layer_height - EPSILON && h <= sp.max_layer_height + EPSILON;
        }
        if (!ok)
            continue;
        for (size_t i = m - k, j = 1; i < m; ++i, ++j) {
            s[2 * i]     += per * double(j - 1);
            s[2 * i + 1] += per * double(j);
        }
        s.back() = z_top;
        layers = std::move(s);
        return true;
    }
    return false;
}

bool land_layers_on_base(const SlicingParameters &sp, std::vector<coordf_t> &layers, bool precise_top)
{
    const double z_base = sp.neotko_floating_bottom_z;
    if (z_base <= 0. || layers.size() < 4 || layers.size() % 2 != 0)
        return false;
    // Con raft la serie no nace en la cama y el «suelo» es el raft: fuera de alcance.
    if (sp.object_print_z_min != 0. || !sp.first_object_layer_height_fixed())
        return false;

    const double z_first = layers[1]; // techo de la primera capa
    if (z_base <= z_first + EPSILON)
        return false; // toca la cama o empieza dentro de la primera capa: rejilla de siempre

    const double span = z_base - z_first;
    const double z_top = sp.object_print_z_max;
    const size_t n_old = layers.size() / 2;

    // Frontera original más cercana a la base: las capas que empiezan en ella son las «reales».
    size_t j_base = 0;
    double best   = std::numeric_limits<double>::max();
    for (size_t k = 0; k < n_old; ++k) {
        const double d = std::abs(layers[2 * k + 1] - z_base);
        if (d < best) { best = d; j_base = k; }
    }
    const double delta = z_base - layers[2 * j_base + 1];

    // Tramo vacío: n capas iguales de la primera capa a la base.
    auto fits = [&](long n) {
        if (n < 1) return false;
        const double h = span / double(n);
        return h >= sp.min_layer_height - EPSILON && h <= sp.max_layer_height + EPSILON;
    };
    long n_empty = std::max<long>(1, std::lround(span / std::max(sp.layer_height, EPSILON)));
    if (!fits(n_empty)) {
        if (fits(n_empty + 1))      ++n_empty;
        else if (fits(n_empty - 1)) --n_empty;
        else {
            std::ostringstream ss;
            ss << "LAND_BASE skipped: z_base=" << z_base << " span=" << span << " layer_height="
               << sp.layer_height << " min=" << sp.min_layer_height << " max=" << sp.max_layer_height;
            dbg(ss.str());
            return false;
        }
    }
    const double h_empty = span / double(n_empty);

    std::vector<coordf_t> out;
    out.reserve(layers.size() + 4);
    out.push_back(layers[0]);
    out.push_back(z_first);
    for (long i = 0; i < n_empty; ++i) {
        out.push_back(z_first + h_empty * double(i));
        out.push_back(i + 1 == n_empty ? z_base : z_first + h_empty * double(i + 1));
    }
    const size_t first_real = out.size() / 2;

    // Capas reales: las originales por encima de la frontera, desplazadas para nacer en la base.
    for (size_t k = j_base + 1; k < n_old; ++k) {
        out.push_back(k == j_base + 1 ? z_base : layers[2 * k] + delta);
        out.push_back(layers[2 * k + 1] + delta);
    }
    // Mismo criterio que generate_object_layers: hay capa mientras el techo no alcance la pieza.
    const double tiny   = 0.5 * sp.min_layer_height;
    double       h_last = out.size() / 2 > first_real ? out.back() - out[out.size() - 2] : sp.layer_height;
    while (out.size() / 2 > first_real + 1 && out[out.size() - 2] + tiny >= z_top)
        out.resize(out.size() - 2); // el desplazamiento dejó una capa entera por encima de la pieza
    while (out.back() + tiny < z_top) {
        const double lo = out.size() / 2 > first_real ? out.back() : z_base;
        out.push_back(lo);
        out.push_back(lo + h_last);
    }

    bool top_landed = false;
    if (precise_top)
        top_landed = land_top(sp, out, first_real, z_top);

    {
        std::ostringstream ss;
        ss << "LAND_BASE z_base=" << z_base << " z_top=" << z_top << " moved_by=" << delta
           << " empty_layers=" << n_empty << " h_empty=" << h_empty
           << " real_layers=" << (out.size() / 2 - first_real)
           << " first_real=[" << out[2 * first_real] << "," << out[2 * first_real + 1] << "]"
           << " last=[" << out[out.size() - 2] << "," << out.back() << "]"
           << " precise_top=" << (precise_top ? (top_landed ? "landed" : "not_possible") : "off");
        dbg(ss.str());
    }
    layers = std::move(out);
    return true;
}

bool land_layers_on_top(const SlicingParameters &sp, std::vector<coordf_t> &layers)
{
    if (layers.size() < 4 || layers.size() % 2 != 0)
        return false;
    // Con raft la serie no nace en la cama: fuera de alcance, igual que land_layers_on_base.
    if (sp.object_print_z_min != 0. || !sp.first_object_layer_height_fixed())
        return false;

    const double z_top    = sp.object_print_z_max;
    const double old_top  = layers.back();
    const size_t old_n    = layers.size() / 2;
    const bool   landed   = land_top(sp, layers, 1, z_top); // 1 = la primera capa no se toca
    {
        std::ostringstream ss;
        ss << "LAND_TOP z_top=" << z_top << " was=" << old_top << " layers=" << old_n << "->" << layers.size() / 2
           << " last=[" << layers[layers.size() - 2] << "," << layers.back() << "]"
           << " result=" << (landed ? "landed" : "not_possible");
        dbg(ss.str());
    }
    return landed;
}

} // namespace Gravity
} // namespace Slic3r
