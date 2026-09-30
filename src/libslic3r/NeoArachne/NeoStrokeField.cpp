// NEOTKO_NEOSTROKE_TAG s338 — NeoStroke por CAMPO. Ver NeoStrokeField.hpp.
// Transcripción de `docs/TOOLS/strokes/campo/campo2.py` (plan) y `caminos.py` (colas + costura). Cada bloque
// lleva al lado el trozo del prototipo del que sale, para poder verificarlo transcribiéndolo de vuelta.
#include "NeoStrokeField.hpp"

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r { namespace NeoArachne {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

using fclk = std::chrono::steady_clock;
double ms_since(fclk::time_point a) { return std::chrono::duration<double, std::milli>(fclk::now() - a).count(); }

// ── rejilla ─────────────────────────────────────────────────────────────────
// Celda (r, c) = punto (x0 + c·cell, y1 − r·cell), igual que el ráster del prototipo (PIL con el origen
// arriba a la izquierda y la fila creciendo hacia abajo).
struct Grid {
    int    W = 0, H = 0;
    double x0 = 0., y1 = 0., cell = 0.01;
    size_t size() const { return size_t(W) * size_t(H); }
    size_t idx(int r, int c) const { return size_t(r) * size_t(W) + size_t(c); }
    Vec2d  at(double r, double c) const { return Vec2d(x0 + c * cell, y1 - r * cell); }
    bool   rc(const Vec2d& p, int& r, int& c) const
    {
        c = int(std::lround((p.x() - x0) / cell));
        r = int(std::lround((y1 - p.y()) / cell));
        return r >= 0 && r < H && c >= 0 && c < W;
    }
};

// Relleno por barrido (par-impar: contorno y agujeros juntos). = `campo.raster`.
std::vector<char> raster(const Grid& g, const ExPolygons& ex)
{
    std::vector<std::pair<Vec2d, Vec2d>> edges;
    auto add = [&](const Polygon& poly) {
        const size_t n = poly.points.size();
        for (size_t i = 0; i < n; ++i) {
            const Point& a = poly.points[i];
            const Point& b = poly.points[(i + 1) % n];
            edges.emplace_back(Vec2d(unscale<double>(a.x()), unscale<double>(a.y())),
                               Vec2d(unscale<double>(b.x()), unscale<double>(b.y())));
        }
    };
    for (const ExPolygon& e : ex) {
        add(e.contour);
        for (const Polygon& h : e.holes)
            add(h);
    }
    std::vector<char>   m(g.size(), 0);
    std::vector<double> xs;
    for (int r = 0; r < g.H; ++r) {
        const double y = g.y1 - r * g.cell;
        xs.clear();
        for (const auto& e : edges) {
            const Vec2d &a = e.first, &b = e.second;
            if ((a.y() <= y && b.y() > y) || (b.y() <= y && a.y() > y))
                xs.push_back(a.x() + (y - a.y()) * (b.x() - a.x()) / (b.y() - a.y()));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int c0 = std::max(0, int(std::ceil((xs[k] - g.x0) / g.cell)));
            const int c1 = std::min(g.W - 1, int(std::floor((xs[k + 1] - g.x0) / g.cell)));
            for (int c = c0; c <= c1; ++c)
                m[g.idx(r, c)] = 1;
        }
    }
    return m;
}

// Distancia euclídea exacta (Felzenszwalb & Huttenlocher), con el índice del rasgo más cercano.
// Rasgo = celda con `feat` a 1. Distancia en mm. = `ndi.distance_transform_edt(~feat, return_indices=True)`.
void dt1d(const std::vector<double>& f, int n, std::vector<double>& d, std::vector<int>& arg,
          std::vector<int>& v, std::vector<double>& z)
{
    const double INF = 1e30;
    v.assign(size_t(n), 0);
    z.assign(size_t(n) + 1, 0.);
    int k = 0;
    v[0] = 0;
    z[0] = -INF;
    z[1] = INF;
    for (int q = 1; q < n; ++q) {
        double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2. * q - 2. * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2. * q - 2. * v[k]);
        }
        ++k;
        v[k]     = q;
        z[k]     = s;
        z[k + 1] = INF;
    }
    k = 0;
    d.assign(size_t(n), 0.);
    arg.assign(size_t(n), 0);
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q)
            ++k;
        d[q]   = double(q - v[k]) * double(q - v[k]) + f[v[k]];
        arg[q] = v[k];
    }
}

void edt(const Grid& g, const std::vector<char>& feat, std::vector<double>& dist, std::vector<int>* nearest)
{
    const double INF = 1e18;
    std::vector<double> col(g.size());
    std::vector<int>    row_of(g.size());
    std::vector<double> f, d, z;
    std::vector<int>    arg, v;
    f.resize(size_t(std::max(g.W, g.H)));
    for (int c = 0; c < g.W; ++c) {
        for (int r = 0; r < g.H; ++r)
            f[r] = feat[g.idx(r, c)] ? 0. : INF;
        dt1d(f, g.H, d, arg, v, z);
        for (int r = 0; r < g.H; ++r) {
            col[g.idx(r, c)]    = d[r];
            row_of[g.idx(r, c)] = arg[r];
        }
    }
    dist.assign(g.size(), 0.);
    if (nearest)
        nearest->assign(g.size(), -1);
    for (int r = 0; r < g.H; ++r) {
        for (int c = 0; c < g.W; ++c)
            f[c] = col[g.idx(r, c)];
        dt1d(f, g.W, d, arg, v, z);
        for (int c = 0; c < g.W; ++c) {
            dist[g.idx(r, c)] = std::sqrt(std::max(0., d[c])) * g.cell;
            if (nearest && d[c] < INF * 0.5)
                (*nearest)[g.idx(r, c)] = int(g.idx(row_of[g.idx(r, arg[c])], arg[c]));
        }
    }
}

// Gauss por TRES medias móviles (Kutskir): coste O(celdas) sea cual sea σ. Con el núcleo directo, σ = 25 celdas
// eran ~200 M operaciones por isla. = `ndi.gaussian_filter` salvo el borde (aquí se repite el último valor).
void box_pass(const Grid& g, std::vector<double>& a, int rad, bool horiz)
{
    if (rad <= 0)
        return;
    const int n_outer = horiz ? g.H : g.W, n = horiz ? g.W : g.H;
    std::vector<double> line(static_cast<size_t>(n)), out(static_cast<size_t>(n));
    const double inv = 1. / (2 * rad + 1);
    for (int o = 0; o < n_outer; ++o) {
        for (int k = 0; k < n; ++k)
            line[size_t(k)] = a[horiz ? g.idx(o, k) : g.idx(k, o)];
        double acc = 0.;
        for (int k = -rad; k <= rad; ++k)
            acc += line[size_t(std::clamp(k, 0, n - 1))];
        for (int k = 0; k < n; ++k) {
            out[size_t(k)] = acc * inv;
            acc += line[size_t(std::clamp(k + rad + 1, 0, n - 1))] - line[size_t(std::clamp(k - rad, 0, n - 1))];
        }
        for (int k = 0; k < n; ++k)
            a[horiz ? g.idx(o, k) : g.idx(k, o)] = out[size_t(k)];
    }
}

void blur(const Grid& g, std::vector<double>& a, double sig_px)
{
    if (sig_px < 0.3)
        return;
    const int    nb     = 3;
    const double wIdeal = std::sqrt(12. * sig_px * sig_px / nb + 1.);
    int          wl     = int(std::floor(wIdeal));
    if (wl % 2 == 0)
        --wl;
    const int wu = wl + 2;
    const int m  = int(std::lround((12. * sig_px * sig_px - nb * wl * wl - 4. * nb * wl - 3. * nb) / (-4. * wl - 4.)));
    for (int i = 0; i < nb; ++i) {
        const int rad = ((i < m ? wl : wu) - 1) / 2;
        box_pass(g, a, rad, true);
        box_pass(g, a, rad, false);
    }
}

// = `campo2.smooth_masked`: media gaussiana SÓLO de lo que está dentro de la zona.
std::vector<double> smooth_masked(const Grid& g, const std::vector<double>& f, const std::vector<char>& U, double sig_px)
{
    std::vector<double> a(g.size()), b(g.size());
    for (size_t i = 0; i < g.size(); ++i) {
        a[i] = U[i] ? f[i] : 0.;
        b[i] = U[i] ? 1. : 0.;
    }
    blur(g, a, sig_px);
    blur(g, b, sig_px);
    std::vector<double> out(g.size(), kNaN);
    for (size_t i = 0; i < g.size(); ++i)
        if (U[i])
            out[i] = a[i] / std::max(b[i], 1e-9);
    return out;
}

// Curvas de nivel 0 de un campo con huecos (NaN). = `skimage.measure.find_contours(f, 0)`, en (fila, col).
std::vector<std::vector<Vec2d>> contours0(const Grid& g, const std::vector<double>& f)
{
    // arista horizontal (r,c)-(r,c+1) -> 2·idx ; vertical (r,c)-(r+1,c) -> 2·idx+1
    std::unordered_map<long long, Vec2d>              pt;
    std::unordered_map<long long, std::vector<long long>> nb;
    auto edge_pt = [&](long long key, int r0, int c0, int r1, int c1) {
        if (pt.count(key))
            return;
        const double va = f[g.idx(r0, c0)], vb = f[g.idx(r1, c1)];
        const double t  = va / (va - vb);
        pt[key] = Vec2d(r0 + t * (r1 - r0), c0 + t * (c1 - c0));   // (fila, col)
    };
    auto link = [&](long long a, long long b) {
        nb[a].push_back(b);
        nb[b].push_back(a);
    };
    for (int r = 0; r + 1 < g.H; ++r)
        for (int c = 0; c + 1 < g.W; ++c) {
            const double v0 = f[g.idx(r, c)], v1 = f[g.idx(r, c + 1)], v2 = f[g.idx(r + 1, c + 1)], v3 = f[g.idx(r + 1, c)];
            if (std::isnan(v0) || std::isnan(v1) || std::isnan(v2) || std::isnan(v3))
                continue;
            const bool p0 = v0 > 0, p1 = v1 > 0, p2 = v2 > 0, p3 = v3 > 0;
            const long long T = 2LL * (long long)g.idx(r, c), Bt = 2LL * (long long)g.idx(r + 1, c);
            const long long L = 2LL * (long long)g.idx(r, c) + 1, Rt = 2LL * (long long)g.idx(r, c + 1) + 1;
            std::vector<long long> cr;
            if (p0 != p1) { edge_pt(T, r, c, r, c + 1); cr.push_back(T); }
            if (p1 != p2) { edge_pt(Rt, r, c + 1, r + 1, c + 1); cr.push_back(Rt); }
            if (p2 != p3) { edge_pt(Bt, r + 1, c, r + 1, c + 1); cr.push_back(Bt); }
            if (p3 != p0) { edge_pt(L, r, c, r + 1, c); cr.push_back(L); }
            if (cr.size() == 2)
                link(cr[0], cr[1]);
            else if (cr.size() == 4) {
                const bool center = (v0 + v1 + v2 + v3) / 4. > 0;
                // cr = T, R, B, L
                if (p0 == center) {         // 0 y 2 del lado del centro: se aíslan 1 y 3
                    link(cr[0], cr[1]);
                    link(cr[2], cr[3]);
                } else {                    // se aíslan 0 y 2
                    link(cr[0], cr[3]);
                    link(cr[1], cr[2]);
                }
            }
        }
    std::vector<std::vector<Vec2d>> out;
    std::unordered_map<long long, char> used;
    auto walk = [&](long long s) {
        // 🚨 `.at()`, nunca `[]`: aquí se itera sobre `nb` desde fuera y un `[]` con una clave nueva lo reharía.
        std::vector<Vec2d> poly{ pt.at(s) };
        used[s] = 1;
        long long cur = s, prev = -1;
        while (true) {
            long long nx = -1;
            for (long long q : nb.at(cur))
                if (q != prev && (!used.count(q) || (q == s && poly.size() > 2))) {
                    nx = q;
                    break;
                }
            if (nx < 0)
                break;
            poly.push_back(pt.at(nx));
            if (nx == s)
                break;
            used[nx] = 1;
            prev = cur;
            cur  = nx;
        }
        out.push_back(std::move(poly));
    };
    for (auto& kv : nb)
        if (kv.second.size() == 1 && !used.count(kv.first))
            walk(kv.first);
    for (auto& kv : nb)
        if (!used.count(kv.first))
            walk(kv.first);
    for (auto& poly : out)
        for (Vec2d& q : poly)
            q = g.at(q.x(), q.y());
    return out;
}

double polylen(const std::vector<Vec2d>& p)
{
    double l = 0.;
    for (size_t i = 1; i < p.size(); ++i)
        l += (p[i] - p[i - 1]).norm();
    return l;
}

// Nº de cordones de una sección de ancho W. = la fórmula de `campo2.plan`.
double n_of(double W, const FieldParams& p)
{
    double n = std::round(W / p.tgt);
    n        = std::max(n, std::ceil(W / p.wmax - 1e-6));
    n        = std::min(n, std::max(1., std::floor(W / p.wmin + 1e-6)));
    return std::max(n, 1.);
}

// Huella de unos carriles (su SEPARACIÓN) en la rejilla: cápsula por segmento. = `medida2.pinta`.
void stamp(const Grid& g, const std::vector<FieldLane>& lanes, std::vector<char>& cov)
{
    for (const FieldLane& L : lanes) {
        const size_t n = L.pts.size();
        if (n < 2)
            continue;
        const size_t m = L.closed ? n : n - 1;
        for (size_t i = 0; i < m; ++i) {
            const Vec2d  a = L.pts[i], b = L.pts[(i + 1) % n];
            const double hw = 0.5 * std::max(L.w[i], g.cell);
            int r0, c0, r1, c1;
            g.rc(Vec2d(std::min(a.x(), b.x()) - hw, std::max(a.y(), b.y()) + hw), r0, c0);
            g.rc(Vec2d(std::max(a.x(), b.x()) + hw, std::min(a.y(), b.y()) - hw), r1, c1);
            const Vec2d  ab = b - a;
            const double l2 = ab.squaredNorm();
            for (int r = std::max(0, r0); r <= std::min(g.H - 1, r1); ++r)
                for (int c = std::max(0, c0); c <= std::min(g.W - 1, c1); ++c) {
                    const Vec2d  q = g.at(r, c);
                    const double t = l2 > 0. ? std::clamp((q - a).dot(ab) / l2, 0., 1.) : 0.;
                    if ((a + ab * t - q).norm() <= hw)
                        cov[g.idx(r, c)] = 1;
                }
        }
    }
}

// = skimage.morphology.medial_axis: se quitan píxeles en orden de distancia al borde (y, a igual distancia, de
// más a menos esquinados) mientras no rompan la conectividad ni sean una punta. Desempate fijo (hash), no aleatorio.
std::vector<char> medial_axis(const Grid& g, const std::vector<char>& U, const std::vector<double>& d)
{
    // 🚨 s338 (auditoría) — las capas se laminan EN PARALELO (TBB): la tabla se construye UNA vez con un static
    //    local inicializado por lambda, que C++11 garantiza sin carreras. Rellenarla "si está vacía" era una carrera.
    struct Tables { std::vector<char> table, corner; };
    static const Tables T = [] {
        Tables t;
        t.table.assign(512, 0);
        t.corner.assign(512, 0);
        auto comps = [](int idx) {   // componentes 8-conexas del 3x3
            int seen = 0, n = 0;
            for (int s0 = 0; s0 < 9; ++s0) {
                if (!(idx & (1 << s0)) || (seen & (1 << s0)))
                    continue;
                ++n;
                int st[9], top = 0;
                st[top++] = s0;
                seen |= 1 << s0;
                while (top) {
                    const int q = st[--top], r = q / 3, c = q % 3;
                    for (int dr = -1; dr <= 1; ++dr)
                        for (int dc = -1; dc <= 1; ++dc) {
                            const int rr = r + dr, cc = c + dc;
                            if (rr < 0 || rr > 2 || cc < 0 || cc > 2)
                                continue;
                            const int t2 = rr * 3 + cc;
                            if ((idx & (1 << t2)) && !(seen & (1 << t2))) {
                                seen |= 1 << t2;
                                st[top++] = t2;
                            }
                        }
                }
            }
            return n;
        };
        for (int idx = 0; idx < 512; ++idx) {
            int cnt = 0;
            for (int b = 0; b < 9; ++b)
                cnt += (idx >> b) & 1;
            const bool center = idx & 16;
            t.table[size_t(idx)]  = center && (comps(idx) != comps(idx & ~16) || cnt < 3);
            t.corner[size_t(idx)] = char(9 - cnt);
        }
        return t;
    }();
    const std::vector<char>& table  = T.table;
    const std::vector<char>& corner = T.corner;
    auto code = [&](const std::vector<char>& m, int r, int c) {
        int idx = 0, bit = 0;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc, ++bit) {
                const int rr = r + dr, cc = c + dc;
                if (rr >= 0 && rr < g.H && cc >= 0 && cc < g.W && m[g.idx(rr, cc)])
                    idx |= 1 << bit;
            }
        return idx;
    };
    struct Px { double d; int corner; uint32_t tie; int r, c; };
    std::vector<Px> order;
    for (int r = 0; r < g.H; ++r)
        for (int c = 0; c < g.W; ++c)
            if (U[g.idx(r, c)]) {
                uint32_t h = uint32_t(g.idx(r, c)) * 2654435761u;
                h ^= h >> 15;
                order.push_back({ d[g.idx(r, c)], corner[size_t(code(U, r, c))], h, r, c });
            }
    std::sort(order.begin(), order.end(), [](const Px& a, const Px& b) {
        if (a.d != b.d) return a.d < b.d;
        if (a.corner != b.corner) return a.corner < b.corner;
        return a.tie < b.tie;
    });
    std::vector<char> res = U;
    for (const Px& q : order)
        res[g.idx(q.r, q.c)] = table[size_t(code(res, q.r, q.c))];
    return res;
}

// = campo.trazar: píxeles del eje -> cadenas, cortadas en puntas y cruces. `deg` = vecinos 8-conexos.
std::vector<std::vector<int>> trace(const Grid& g, const std::vector<char>& S, std::vector<int>& deg)
{
    deg.assign(g.size(), 0);
    auto nb = [&](int p, int* out) {
        const int r = p / g.W, c = p % g.W;
        int n = 0;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc) {
                if (!dr && !dc)
                    continue;
                const int rr = r + dr, cc = c + dc;
                if (rr >= 0 && rr < g.H && cc >= 0 && cc < g.W && S[g.idx(rr, cc)])
                    out[n++] = int(g.idx(rr, cc));
            }
        return n;
    };
    std::vector<int> pix;
    for (size_t i = 0; i < g.size(); ++i)
        if (S[i])
            pix.push_back(int(i));
    int buf[8];
    for (int p : pix)
        deg[size_t(p)] = nb(p, buf);
    auto is_node = [&](int p) { return deg[size_t(p)] != 2; };
    std::unordered_map<unsigned long long, char> seen;
    auto ek = [](int a, int b) {
        const unsigned long long x = (unsigned long long)std::min(a, b), y = (unsigned long long)std::max(a, b);
        return (x << 32) | y;
    };
    std::vector<std::vector<int>> out;
    auto walk = [&](int p0, int p1) {
        std::vector<int> ch{ p0, p1 };
        seen[ek(p0, p1)] = 1;
        while (!is_node(ch.back())) {
            int nbv[8];
            const int n = nb(ch.back(), nbv);
            int nx = -1;
            for (int k = 0; k < n; ++k)
                if (!seen.count(ek(ch.back(), nbv[k])) && nbv[k] != ch[ch.size() - 2]) {
                    nx = nbv[k];
                    break;
                }
            if (nx < 0)
                break;
            seen[ek(ch.back(), nx)] = 1;
            ch.push_back(nx);
            if (ch.back() == ch.front())
                break;
        }
        out.push_back(std::move(ch));
    };
    for (int p : pix)
        if (is_node(p)) {
            int nbv[8];
            const int n = nb(p, nbv);
            for (int k = 0; k < n; ++k)
                if (!seen.count(ek(p, nbv[k])))
                    walk(p, nbv[k]);
        }
    for (int p : pix) {
        int nbv[8];
        const int n = nb(p, nbv);
        for (int k = 0; k < n; ++k)
            if (!seen.count(ek(p, nbv[k])))
                walk(p, nbv[k]);
    }
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
std::vector<FieldLane> field_lanes(const ExPolygon& island, const FieldParams& p, FieldStats& st)
{
    std::vector<FieldLane> lanes;
    // 🚨 s338 — primero la frontera: sección más ancha que *Widest shape handled* (medida como el motor viejo, sobre
    //    el hueco útil, ya sin el muro) = pieza. Sin esto las piezas anchas de la placa entraban enteras en el campo.
    // 🏁 s340 — en MODO BANDA la frontera no aplica: la banda ya deja fino lo que el campo ve, y el centro se lo
    //    queda el relleno. Sin saltarla, justo las piezas anchas (para las que existe el modo) se iban enteras al viejo.
    const bool band_mode = p.band > 0.;
    if (!band_mode && !offset_ex(island, -float(scaled<double>(p.outer_w + 0.5 * p.max_stroke_w))).empty()) {
        st.skipped = true;
        return lanes;
    }
    // zona útil = la isla menos la banda del muro de Classic
    ExPolygons Uex = offset_ex(island, -float(scaled<double>(p.outer_w)));
    // 🏁 s340 — MODO BANDA: fuera el núcleo que queda a más de `band` del muro. Un trazo más fino que 2 × band no
    //    tiene núcleo y sale igual que en auto; sólo cambian las zonas anchas. Borde del núcleo redondo (offset
    //    redondo de Clipper), para que el relleno case con una línea suave.
    if (band_mode) {
        ExPolygons core = offset_ex(island, -float(scaled<double>(p.outer_w + p.band)));
        // 🚨 s340 (TEST30, la A) — sólo es relleno un núcleo ANCHO: si le cabe un disco del ancho de la banda
        //    (mín. 0.8 mm). Una tira fina (la diagonal de la A) el relleno la rechaza por no caberle una línea
        //    y NeoStroke ya no la tenía: quedaba HUECO, y su borde dentado sembraba colas (164 centrales, 162
        //    colas en esa isla). Apertura morfológica: las tiras finas vuelven a NeoStroke como en auto.
        if (!core.empty()) {
            const float r = float(scaled<double>(std::max(0.5 * p.band, 0.4)));
            core = offset_ex(offset_ex(core, -r), r);
        }
        if (!core.empty())
            Uex = diff_ex(Uex, core);
    }
    if (Uex.empty())
        return lanes;
    BoundingBox bb = get_extents(island);
    Grid g;
    g.cell = p.cell;
    const double wmm = unscale<double>(bb.max.x() - bb.min.x()) + 0.6, hmm = unscale<double>(bb.max.y() - bb.min.y()) + 0.6;
    if (wmm * hmm / (g.cell * g.cell) > double(p.max_cells)) {
        g.cell = std::sqrt(wmm * hmm / double(p.max_cells));
        if (g.cell > 0.02) {    // demasiado gruesa para cordones de 0.25: que lo haga el planificador viejo
            st.skipped = true;
            return lanes;
        }
    }
    g.x0 = unscale<double>(bb.min.x()) - 0.3;
    g.y1 = unscale<double>(bb.max.y()) + 0.3;
    g.W  = int(std::ceil(wmm / g.cell));
    g.H  = int(std::ceil(hmm / g.cell));
    st.cells += g.size();
    const double R = 1. / g.cell;   // celdas por mm

    auto t_ph = fclk::now();
    // U = raster(zona útil) ; d = edt(U)
    const std::vector<char> U = raster(g, Uex);
    std::vector<char>       notU(g.size());
    for (size_t i = 0; i < g.size(); ++i)
        notU[i] = !U[i];
    std::vector<double> d;
    edt(g, notU, d, nullptr);

    // ── esqueleto: eje sobre la rejilla (= skimage `medial_axis`) + poda PODA/PUNTA (= campo.podar) ──
    // 🚨 s338 — NO el StrokeSkeleton (Voronoi): probado en el banco, con contraformas (la B) su eje salía pegado
    //    al borde del hueco y los huecos se multiplicaban por 40. El prototipo que ganó usa el eje del ráster.
    std::vector<char> skm = medial_axis(g, U, d);
    const double Rmm = g.cell;   // mm por celda
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<int> degv;
        const std::vector<std::vector<int>> ch = trace(g, skm, degv);
        bool cut = false;
        for (const auto& c : ch) {
            const int a0 = c.front(), b0 = c.back();
            const int da = degv[size_t(a0)], db = degv[size_t(b0)];
            if (!((da == 1 && db >= 3) || (db == 1 && da >= 3)))
                continue;
            const int j    = da == 1 ? b0 : a0;
            const int leaf = da == 1 ? a0 : b0;
            // 🚨 sólo ramitas que MUEREN en una esquina (el radio se apaga): un palo cortado no se poda
            if (double(c.size()) * Rmm < p.prune * d[size_t(j)] && d[size_t(leaf)] < p.punta * d[size_t(j)]) {
                for (int q : c)
                    if (q != j)
                        skm[size_t(q)] = 0;
                ++st.pruned;
                cut = true;
            }
        }
        if (!cut)
            break;
    }
    std::vector<int> degv;
    const std::vector<std::vector<int>> chain = trace(g, skm, degv);
    st.t_eje += ms_since(t_ph);
    t_ph = fclk::now();
    if (chain.empty())
        return lanes;

    // e = edt(~sk) ; Wl = 2(d+e) ; Ws = smooth_masked(Wl, U, sig_w)
    std::vector<double> e;
    std::vector<int>    near_sk;
    edt(g, skm, e, &near_sk);
    std::vector<double> Wl(g.size(), 0.);
    for (size_t i = 0; i < g.size(); ++i)
        Wl[i] = 2. * (d[i] + e[i]);
    const std::vector<double> Ws = smooth_masked(g, Wl, U, p.sig_w * R);

    // ── HISTÉRESIS: nº de cordones sobre el eje, por tramos (= el bloque HYST de campo2) ──
    std::vector<double> nm(g.size(), 0.);
    for (const auto& cs : chain) {
        const int L = int(cs.size());
        if (L < 3)
            continue;
        std::vector<double> v(static_cast<size_t>(L)), Wm(static_cast<size_t>(L));
        for (int t = 0; t < L; ++t) {
            Wm[t] = 2. * d[size_t(cs[t])];
            v[t]  = n_of(Wm[t], p);
        }
        if (p.hyst > 0.)
            for (int it = 0; it < 8; ++it) {
                bool ch = false;
                int  a  = 0;
                while (a < L) {
                    int b = a;
                    while (b + 1 < L && v[b + 1] == v[a])
                        ++b;
                    if (double(b - a + 1) * g.cell < p.hyst && (a > 0 || b < L - 1)) {
                        std::vector<double> cands;
                        if (a > 0) cands.push_back(v[a - 1]);
                        if (b < L - 1) cands.push_back(v[b + 1]);
                        std::sort(cands.begin(), cands.end());
                        double wmx = 0.;
                        for (int t = a; t <= b; ++t)
                            wmx = std::max(wmx, Wm[t]);
                        for (double to : cands)
                            if (wmx / to <= p.wmax * p.tol_max) {
                                if (to != v[a]) {
                                    for (int t = a; t <= b; ++t)
                                        v[t] = to;
                                    ch = true;
                                }
                                break;
                            }
                    }
                    a = b + 1;
                }
                if (!ch)
                    break;
            }
        for (int t = 0; t < L; ++t)
            nm[size_t(cs[t])] = v[t];
    }
    std::vector<double> n(g.size(), 1.);
    for (size_t i = 0; i < g.size(); ++i)
        if (U[i])
            n[i] = (near_sk[i] >= 0 && nm[size_t(near_sk[i])] > 0.) ? nm[size_t(near_sk[i])] : n_of(Ws[i], p);
    // nf = smooth_masked(n) ; >= 1 ; meseta entera fuera de las rampas
    std::vector<double> nf = smooth_masked(g, n, U, p.sig_n * R);
    const double B = p.snap;
    for (size_t i = 0; i < g.size(); ++i)
        if (U[i]) {
            const double v  = std::max(nf[i], 1.);
            const double fl = std::floor(v), t = v - fl;
            nf[i] = fl + std::clamp((t - B) / std::max(1e-6, 1. - 2. * B), 0., 1.);
        }

    // pesos: P pares completos + central wc + par parcial wn  (r = nf − 2P)
    std::vector<double> Pp(g.size(), 0.), wn(g.size(), 0.);
    double Pmax = 0.;
    for (size_t i = 0; i < g.size(); ++i)
        if (U[i]) {
            Pp[i]         = std::floor(nf[i] / 2.);
            const double r = nf[i] - 2. * Pp[i];
            wn[i]         = r < 1. ? 0. : r - 1.;
            Pmax          = std::max(Pmax, Pp[i]);
        }

    st.t_campo += ms_since(t_ph);
    t_ph = fclk::now();
    // ── carriles de los pares: curvas de nivel d = d_j ──
    std::vector<double> fj(g.size()), wj(g.size());
    for (int j = 1; j <= int(Pmax) + 1; ++j) {
        bool any = false;
        for (size_t i = 0; i < g.size(); ++i) {
            fj[i] = kNaN;
            wj[i] = kNaN;
            if (!U[i])
                continue;
            const double om = j <= Pp[i] ? 1. : (j == Pp[i] + 1 ? wn[i] : 0.);
            if (om <= 0.02)
                continue;
            const double before = std::min(Pp[i], double(j - 1));
            const double dj     = Ws[i] * (before + om / 2.) / nf[i];
            fj[i] = d[i] - dj;
            wj[i] = Ws[i] * om / nf[i];
            any   = true;
        }
        if (!any)
            continue;
        for (std::vector<Vec2d>& c : contours0(g, fj)) {
            // 🚨 s338 (auditoría) — un punto cuyo ancho no está definido se DESCARTA, y si cae en mitad del carril
            //    los dos trozos quedarían unidos por una recta que se EXTRUIRÍA. Se parte el carril ahí.
            const bool ring = c.size() > 3 && (c.front() - c.back()).norm() < 1e-9;
            std::vector<FieldLane> pieces(1);
            bool broke = false;
            for (const Vec2d& q : c) {
                int r, cc;
                double w = kNaN;
                if (g.rc(q, r, cc))
                    w = wj[g.idx(r, cc)];
                if (std::isnan(w)) {
                    if (!pieces.back().pts.empty()) {
                        pieces.emplace_back();
                        broke = true;
                    }
                    continue;
                }
                pieces.back().pts.push_back(q);
                pieces.back().w.push_back(w);
            }
            for (FieldLane& L : pieces) {
                if (L.pts.size() < 3 || polylen(L.pts) < 0.3)
                    continue;
                L.closed = ring && !broke;
                if (L.closed) {   // sin el punto repetido: la costura cierra el anillo por índice
                    L.pts.pop_back();
                    L.w.pop_back();
                }
                lanes.push_back(std::move(L));
                ++st.pair_lanes;
            }
        }
    }

    // ── carril central: el eje, con lo que queda LIBRE entre él y los pares ──
    // 🚨 s338 (TEST23b) — lo libre se MIDE, no se deduce. Con la fórmula 2·(d − profundidad de los pares) el central
    //    daba por hecho dónde caían los pares, y en las rampas (donde cambia el nº de cordones) no caen ahí: en los
    //    anillos excéntricos del compact-E pisaba al par de al lado (batiburrillo 1 → 16 mm²). Ahora: huella de los
    //    pares en la rejilla y, en cada punto del eje, 2 × la distancia a ella. No puede pisar por construcción.
    //    Banco: compact-E 6.15 → 2.43 mm² de batiburrillo, Gyp Sea 1.13 → 0.31, PLAGE 1.91 → 1.06; huecos iguales.
    std::vector<char> pair_cov(g.size(), 0);
    stamp(g, lanes, pair_cov);
    std::vector<double> free_d;
    edt(g, pair_cov, free_d, nullptr);
    for (size_t ci = 0; ci < chain.size(); ++ci) {
        const auto& cs = chain[ci];
        std::vector<double> cw(cs.size());
        // 🚨 s340 (BASE-EFFECT-NEW-X02) — acotado también por la zona útil (d = edt(notU)): en un trazo fino SIN pares
        //    `pair_cov` está vacío, free_d sale infinito y el central salía a `wmax` pisando el muro de Classic
        //    (73 % del inner encima del outer, 13.6 mm³ frente a 8.6 de Classic en la misma capa).
        for (size_t t = 0; t < cs.size(); ++t) {
            const size_t c = size_t(cs[t]);
            cw[t] = std::clamp(2. * std::min(free_d[c], d[c]), 0., p.wmax);
        }
        size_t a = 0;
        while (a < cs.size()) {
            while (a < cs.size() && cw[a] <= 0.02)
                ++a;
            size_t b = a;
            while (b < cs.size() && cw[b] > 0.02)
                ++b;
            if (b - a >= 4) {
                FieldLane L;
                for (size_t t = a; t < b; t += 3) {
                    const int id = cs[t];
                    L.pts.push_back(g.at(id / g.W, id % g.W));
                    L.w.push_back(cw[t]);
                }
                if ((b - 1 - a) % 3) {
                    const int id = cs[b - 1];
                    L.pts.push_back(g.at(id / g.W, id % g.W));
                    L.w.push_back(cw[b - 1]);
                }
                L.closed = cs.front() == cs.back() && a == 0 && b == cs.size();
                if (L.closed && L.pts.size() > 2 && (L.pts.front() - L.pts.back()).norm() < 1.5 * g.cell) {
                    L.pts.pop_back();
                    L.w.pop_back();
                }
                if (polylen(L.pts) >= 0.2) {
                    lanes.push_back(std::move(L));
                    ++st.center_lanes;
                }
            }
            a = b;
        }
    }
    st.t_carriles += ms_since(t_ph);
    return lanes;
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<FieldLane> field_resample(const std::vector<FieldLane>& lanes, double step)
{
    std::vector<FieldLane> out;
    out.reserve(lanes.size());
    for (const FieldLane& L : lanes) {
        const size_t n = L.pts.size();
        if (n < 2 || step <= 0.) {
            out.push_back(L);
            continue;
        }
        // polilínea a recorrer (el anillo se cierra para medirlo entero)
        std::vector<Vec2d>  P = L.pts;
        std::vector<double> W = L.w;
        if (L.closed) {
            P.push_back(L.pts.front());
            W.push_back(L.w.front());
        }
        std::vector<double> s(P.size(), 0.);
        for (size_t i = 1; i < P.size(); ++i)
            s[i] = s[i - 1] + (P[i] - P[i - 1]).norm();
        const double total = s.back();
        const int    m     = std::max(L.closed ? 3 : 1, int(std::round(total / step)));
        FieldLane q;
        q.closed = L.closed;
        size_t seg = 0;
        const int last = L.closed ? m - 1 : m;   // en un anillo el último punto sería el primero otra vez
        for (int k = 0; k <= last; ++k) {
            const double t = total * double(k) / m;
            while (seg + 2 < P.size() && s[seg + 1] < t)
                ++seg;
            const double len = s[seg + 1] - s[seg];
            const double u   = len > 1e-12 ? std::clamp((t - s[seg]) / len, 0., 1.) : 0.;
            q.pts.push_back(P[seg] + (P[seg + 1] - P[seg]) * u);
            q.w.push_back(W[seg] + (W[seg + 1] - W[seg]) * u);
        }
        out.push_back(std::move(q));
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// = `caminos.recortar_colas`
std::vector<FieldLane> field_trim_tails(const std::vector<FieldLane>& lanes, double cmin, FieldStats& st)
{
    std::vector<FieldLane> out;
    for (const FieldLane& L : lanes) {
        if (L.pts.size() < 2)
            continue;
        if (L.closed) {
            if (*std::max_element(L.w.begin(), L.w.end()) >= cmin)
                out.push_back(L);
            continue;
        }
        size_t a = 0, b = L.pts.size() - 1;
        while (a < b && L.w[a] < cmin)
            ++a;
        while (b > a && L.w[b] < cmin)
            --b;
        if (a > 0 || b + 1 < L.pts.size())
            ++st.tails_cut;
        if (b <= a)
            continue;
        FieldLane q;
        q.pts.assign(L.pts.begin() + a, L.pts.begin() + b + 1);
        q.w.assign(L.w.begin() + a, L.w.begin() + b + 1);
        if (polylen(q.pts) < 0.25)
            continue;
        out.push_back(std::move(q));
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// = `caminos.construir`: empalmes + grafo + Euler. Devuelve los tramos continuos.
std::vector<FieldLane> field_stitch(const ExPolygon& island, const std::vector<FieldLane>& lanes, const FieldParams& p,
                                    FieldStats& st)
{
    std::vector<FieldLane> tramos;
    if (lanes.empty())
        return tramos;
    const auto t_st = fclk::now();
    // rejilla para "va por dentro" y "pisa lo ya puesto"
    Grid g;
    g.cell = 0.01;
    BoundingBox bb = get_extents(island);
    const double wmm = unscale<double>(bb.max.x() - bb.min.x()) + 0.6, hmm = unscale<double>(bb.max.y() - bb.min.y()) + 0.6;
    if (wmm * hmm / (g.cell * g.cell) > double(p.max_cells))
        g.cell = std::sqrt(wmm * hmm / double(p.max_cells));
    g.x0 = unscale<double>(bb.min.x()) - 0.3;
    g.y1 = unscale<double>(bb.max.y()) + 0.3;
    g.W  = int(std::ceil(wmm / g.cell));
    g.H  = int(std::ceil(hmm / g.cell));
    const std::vector<char> letra = raster(g, offset_ex(island, -float(scaled<double>(0.05))));
    std::vector<char> cov(g.size(), 0);
    stamp(g, lanes, cov);
    auto sample = [&](const std::vector<char>& M, const Vec2d& a, const Vec2d& b, std::vector<char>& out) {
        const int n = std::max(2, int((b - a).norm() / 0.01));
        out.resize(size_t(n) + 1);
        for (int i = 0; i <= n; ++i) {
            int r, c;
            out[size_t(i)] = g.rc(a + (b - a) * (double(i) / n), r, c) ? M[g.idx(r, c)] : 0;
        }
        return n;
    };

    // ── empalmes ──
    struct P2 { int lane, k; };
    std::vector<Vec2d> allp;
    std::vector<P2>    own;
    for (int i = 0; i < int(lanes.size()); ++i)
        for (int k = 0; k < int(lanes[i].pts.size()); ++k) {
            allp.push_back(lanes[i].pts[size_t(k)]);
            own.push_back({ i, k });
        }
    struct Cand { int pr; double dd; int i, idx, j, k; double ww; };
    std::vector<Cand> cand;
    std::vector<char> s1, s2;
    for (int i = 0; i < int(lanes.size()); ++i) {
        const FieldLane& L = lanes[size_t(i)];
        if (L.closed)
            continue;
        for (int idx : { 0, int(L.pts.size()) - 1 }) {
            const Vec2d  a   = L.pts[size_t(idx)];
            const double w   = L.w[size_t(idx)];
            const double tol = std::max(0.15, p.junta_k * std::max(w, 0.2));
            for (size_t q = 0; q < allp.size(); ++q) {
                const int j = own[q].lane, k = own[q].k;
                if (j == i)
                    continue;
                const double dd = (allp[q] - a).norm();
                if (dd > tol)
                    continue;
                const Vec2d  b  = allp[q];
                const double ww = std::max(0.05, std::min(w, lanes[size_t(j)].w[size_t(k)]));
                if (dd > 1e-6) {
                    sample(letra, a, b, s1);
                    if (std::find(s1.begin(), s1.end(), 0) != s1.end())
                        continue;
                    const int n = sample(cov, a, b, s2);
                    int on = 0, cnt = 0;
                    for (int t = 0; t <= n; ++t) {
                        const double tt = t * dd / n;
                        if (tt > ww / 2 && tt < dd - ww / 2) {
                            ++cnt;
                            on += s2[size_t(t)];
                        }
                    }
                    if (cnt > 0 && ww * dd * double(on) / cnt > p.doble)
                        continue;
                }
                const bool es_ext = !lanes[size_t(j)].closed && (k == 0 || k == int(lanes[size_t(j)].pts.size()) - 1);
                cand.push_back({ es_ext ? 0 : 1, dd, i, idx, j, k, ww });
            }
        }
    }
    std::stable_sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.pr != b.pr ? a.pr < b.pr : a.dd < b.dd; });
    std::map<std::pair<int, int>, char> usados;
    std::vector<std::vector<int>>        splits(lanes.size());
    struct Con { int i, idx, j, k; double ww; };
    std::vector<Con> conect;
    for (const Cand& c : cand) {
        if (usados.count({ c.i, c.idx }))
            continue;
        const bool es_ext = !lanes[size_t(c.j)].closed && (c.k == 0 || c.k == int(lanes[size_t(c.j)].pts.size()) - 1);
        if (es_ext && usados.count({ c.j, c.k }))
            continue;
        usados[{ c.i, c.idx }] = 1;
        if (es_ext)
            usados[{ c.j, c.k }] = 1;
        else {
            splits[size_t(c.j)].push_back(c.k);
            ++st.splices;
        }
        conect.push_back({ c.i, c.idx, c.j, c.k, c.ww });
    }
    st.joints += conect.size();

    // ── grafo ──
    std::map<std::pair<int, int>, int> node;
    auto nid = [&](int lane, int k) {
        auto it = node.find({ lane, k });
        if (it != node.end())
            return it->second;
        const int id = int(node.size());
        node[{ lane, k }] = id;
        return id;
    };
    struct Edge { int u, v; std::vector<Vec2d> pts; std::vector<double> w; bool virt; };
    std::vector<Edge> E;
    for (int i = 0; i < int(lanes.size()); ++i) {
        const FieldLane& L = lanes[size_t(i)];
        const int n = int(L.pts.size());
        std::vector<int> s = splits[size_t(i)];
        std::sort(s.begin(), s.end());
        s.erase(std::unique(s.begin(), s.end()), s.end());
        if (L.closed) {
            if (s.empty())
                s.push_back(0);
            for (size_t t = 0; t < s.size(); ++t) {
                const int a = s[t], b = t + 1 < s.size() ? s[t + 1] : s[0] + n;
                Edge ed{ nid(i, a % n), nid(i, b % n), {}, {}, false };
                for (int q = a; q <= b; ++q) {
                    ed.pts.push_back(L.pts[size_t(q % n)]);
                    ed.w.push_back(L.w[size_t(q % n)]);
                }
                E.push_back(std::move(ed));
            }
        } else {
            std::vector<int> cuts = s;
            cuts.push_back(0);
            cuts.push_back(n - 1);
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            for (size_t t = 0; t + 1 < cuts.size(); ++t) {
                Edge ed{ nid(i, cuts[t]), nid(i, cuts[t + 1]), {}, {}, false };
                ed.pts.assign(L.pts.begin() + cuts[t], L.pts.begin() + cuts[t + 1] + 1);
                ed.w.assign(L.w.begin() + cuts[t], L.w.begin() + cuts[t + 1] + 1);
                E.push_back(std::move(ed));
            }
        }
    }
    for (const Con& c : conect) {
        const int n  = int(lanes[size_t(c.j)].pts.size());
        const int kj = c.k % n;
        E.push_back({ nid(c.i, c.idx), nid(c.j, kj),
                      { lanes[size_t(c.i)].pts[size_t(c.idx)], lanes[size_t(c.j)].pts[size_t(kj)] },
                      { c.ww, c.ww }, false });
    }
    const int NN = int(node.size());
    std::vector<Vec2d> pos(static_cast<size_t>(NN));
    for (const auto& kv : node) {
        const FieldLane& L = lanes[size_t(kv.first.first)];
        pos[size_t(kv.second)] = L.pts[size_t(kv.first.second % int(L.pts.size()))];
    }
    std::vector<int> par(static_cast<size_t>(NN));
    for (int v = 0; v < NN; ++v)
        par[size_t(v)] = v;
    std::function<int(int)> f = [&](int z) {
        while (par[size_t(z)] != z) {
            par[size_t(z)] = par[size_t(par[size_t(z)])];
            z              = par[size_t(z)];
        }
        return z;
    };
    std::vector<int> deg(size_t(NN), 0);
    for (const Edge& ed : E) {
        par[size_t(f(ed.u))] = f(ed.v);
        ++deg[size_t(ed.u)];
        ++deg[size_t(ed.v)];
    }
    std::map<int, std::vector<int>> comps;
    for (int v = 0; v < NN; ++v)
        if (deg[size_t(v)] > 0)
            comps[f(v)].push_back(v);

    // ── Euler por componente: impares emparejados con virtuales, Hierholzer, cortar por las virtuales ──
    for (auto& kv : comps) {
        const std::vector<int>& c = kv.second;
        std::vector<char> inC(size_t(NN), 0);
        for (int v : c)
            inC[size_t(v)] = 1;
        std::vector<int> ce;
        for (int ei = 0; ei < int(E.size()); ++ei)
            if (inC[size_t(E[size_t(ei)].u)])
                ce.push_back(ei);
        std::vector<int> odd;
        for (int v : c)
            if (deg[size_t(v)] % 2)
                odd.push_back(v);
        struct Pr { double d; int a, b; };
        std::vector<Pr> prs;
        for (size_t x = 0; x < odd.size(); ++x)
            for (size_t y = x + 1; y < odd.size(); ++y)
                prs.push_back({ (pos[size_t(odd[x])] - pos[size_t(odd[y])]).norm(), odd[x], odd[y] });
        std::stable_sort(prs.begin(), prs.end(), [](const Pr& a, const Pr& b) { return a.d < b.d; });
        std::vector<char> free_(size_t(NN), 0);
        for (int v : odd)
            free_[size_t(v)] = 1;
        std::vector<Edge> local;
        for (int ei : ce)
            local.push_back(E[size_t(ei)]);
        for (const Pr& pr : prs)
            if (free_[size_t(pr.a)] && free_[size_t(pr.b)]) {
                free_[size_t(pr.a)] = free_[size_t(pr.b)] = 0;
                local.push_back({ pr.a, pr.b, { pos[size_t(pr.a)], pos[size_t(pr.b)] }, { 0., 0. }, true });
            }
        std::map<int, std::vector<std::pair<int, int>>> adj;   // nudo -> (arista, otro nudo)
        for (int ei = 0; ei < int(local.size()); ++ei) {
            adj[local[size_t(ei)].u].push_back({ ei, local[size_t(ei)].v });
            adj[local[size_t(ei)].v].push_back({ ei, local[size_t(ei)].u });
        }
        std::vector<char> used(local.size(), 0);
        const int start = odd.empty() ? c.front() : odd.front();
        struct St { int v, ei; bool rev; };
        std::vector<St> stack{ { start, -1, false } };
        std::vector<std::pair<int, bool>> seq;
        while (!stack.empty()) {
            const int v = stack.back().v;
            auto& av = adj[v];
            while (!av.empty() && used[size_t(av.back().first)])
                av.pop_back();
            if (!av.empty()) {
                const auto e2 = av.back();
                av.pop_back();
                used[size_t(e2.first)] = 1;
                stack.push_back({ e2.second, e2.first, local[size_t(e2.first)].u != v });
            } else {
                if (stack.back().ei >= 0)
                    seq.push_back({ stack.back().ei, stack.back().rev });
                stack.pop_back();
            }
        }
        std::reverse(seq.begin(), seq.end());
        bool has_virt = false;
        for (const auto& s : seq)
            if (local[size_t(s.first)].virt)
                has_virt = true;
        if (has_virt) {   // empezar justo después de una virtual
            while (!local[size_t(seq.front().first)].virt)
                std::rotate(seq.begin(), seq.begin() + 1, seq.end());
            std::rotate(seq.begin(), seq.begin() + 1, seq.end());
        }
        FieldLane tr;
        bool      open = false;
        for (const auto& s : seq) {
            const Edge& ed = local[size_t(s.first)];
            if (ed.virt) {
                if (open && tr.pts.size() >= 2)
                    tramos.push_back(std::move(tr));
                tr   = FieldLane{};
                open = false;
                continue;
            }
            std::vector<Vec2d>  gp = ed.pts;
            std::vector<double> gw = ed.w;
            if (s.second) {
                std::reverse(gp.begin(), gp.end());
                std::reverse(gw.begin(), gw.end());
            }
            if (!open) {
                tr.pts = gp;
                tr.w   = gw;
                open   = true;
            } else {
                tr.pts.insert(tr.pts.end(), gp.begin() + 1, gp.end());
                tr.w.insert(tr.w.end(), gw.begin() + 1, gw.end());
            }
        }
        if (open && tr.pts.size() >= 2)
            tramos.push_back(std::move(tr));
    }
    st.tramos += tramos.size();
    st.t_costura += ms_since(t_st);
    return tramos;
}

}} // namespace Slic3r::NeoArachne
