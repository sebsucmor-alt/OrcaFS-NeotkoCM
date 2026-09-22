// NEOTKO_NEOSTROKE_TAG C1 (s325)
#include "NeoStrokeSkeleton.hpp"

#include "../AABBTreeLines.hpp"
#include "../ClipperUtils.hpp"
#include "../Geometry/Voronoi.hpp"
#include "../Polygon.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <utility>

namespace Slic3r { namespace NeoArachne {

// Cuantización de coordenadas al construir el grafo: 0.1 µm, igual que el Q = 1e4 (sobre mm) del
// prototipo. Dos vértices del Voronoi a menos de esto son EL MISMO nodo.
static const double NS_QUANT_MM = 1e-4;

using NodeKey = std::pair<int64_t, int64_t>;

static inline NodeKey node_key(const Vec2d& p)
{
    return { int64_t(std::llround(p.x() / NS_QUANT_MM)), int64_t(std::llround(p.y() / NS_QUANT_MM)) };
}

// Muestreo de un anillo a paso `ds`, en mm. Igual que _resample() del prototipo: n tramos iguales,
// el punto de cierre no se repite.
static void resample_ring(const Polygon& ring, double ds_mm, std::vector<Vec2d>& out)
{
    const size_t n_pts = ring.points.size();
    if (n_pts < 3)
        return;
    std::vector<double> acc(n_pts + 1, 0.);
    for (size_t i = 0; i < n_pts; ++i) {
        const Vec2d a = unscale(ring.points[i]);
        const Vec2d b = unscale(ring.points[(i + 1) % n_pts]);
        acc[i + 1]    = acc[i] + (b - a).norm();
    }
    const double total = acc.back();
    if (total <= 0.)
        return;
    const size_t n = std::max<size_t>(8, size_t(std::ceil(total / std::max(ds_mm, 1e-4))));
    size_t seg = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s = total * double(i) / double(n);
        while (seg + 1 < n_pts && acc[seg + 1] < s)
            ++seg;
        const Vec2d  a  = unscale(ring.points[seg]);
        const Vec2d  b  = unscale(ring.points[(seg + 1) % n_pts]);
        const double dl = acc[seg + 1] - acc[seg];
        const double t  = dl > 0. ? (s - acc[seg]) / dl : 0.;
        out.emplace_back(a + (b - a) * t);
    }
}

void StrokeSkeleton::link(int a, int b)
{
    if (a == b)
        return;
    auto& A = m_adj[a];
    if (std::find(A.begin(), A.end(), b) == A.end())
        A.push_back(b);
    auto& B = m_adj[b];
    if (std::find(B.begin(), B.end(), a) == B.end())
        B.push_back(a);
}

void StrokeSkeleton::unlink_node(int i)
{
    for (int nb : m_adj[i]) {
        auto& N = m_adj[nb];
        N.erase(std::remove(N.begin(), N.end(), i), N.end());
    }
    m_adj[i].clear();
    m_alive[i] = false;
}

// ── construcción ────────────────────────────────────────────────────────────
// Voronoi de un muestreo denso del contorno (exterior y agujeros). Se queda con las aristas cuyos
// DOS extremos y su punto medio caen dentro de la isla: es el mismo truco que usa Orca por dentro.
void StrokeSkeleton::build(const ExPolygon& island, double ds_mm)
{
    std::vector<Vec2d> samples;
    resample_ring(island.contour, ds_mm, samples);
    for (const Polygon& h : island.holes)
        resample_ring(h, ds_mm, samples);
    if (samples.size() < 4)
        return;

    // El Voronoi trabaja en coordenadas escaladas (enteras). Puntos repetidos fuera.
    Points sites;
    sites.reserve(samples.size());
    {
        std::map<NodeKey, char> seen;
        for (const Vec2d& p : samples)
            if (seen.emplace(node_key(p), 0).second)
                sites.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    }
    if (sites.size() < 4)
        return;

    Geometry::VoronoiDiagram vd;
    vd.construct_voronoi(sites.begin(), sites.end());

    AABBTreeLines::LinesDistancer<Linef> inside{ to_unscaled_linesf(ExPolygons{ island }) };
    auto is_inside = [&inside](const Vec2d& p) { return inside.distance_from_lines<true>(p) < 0.; };

    std::map<NodeKey, int> index;
    auto node_at = [&](const Vec2d& p) -> int {
        auto it = index.find(node_key(p));
        if (it != index.end())
            return it->second;
        const int id = int(m_pos.size());
        index.emplace(node_key(p), id);
        m_pos.emplace_back(p);
        m_adj.emplace_back();
        m_alive.push_back(true);
        return id;
    };

    for (const auto& e : vd.edges()) {
        // Sólo aristas finitas: las infinitas del Voronoi de un contorno cerrado salen de celdas
        // exteriores y no pintan nada dentro de la isla.
        if (e.vertex0() == nullptr || e.vertex1() == nullptr)
            continue;
        if (e.vertex0() > e.vertex1())   // cada arista viene dos veces (ella y su gemela)
            continue;
        const Vec2d a(e.vertex0()->x() * SCALING_FACTOR, e.vertex0()->y() * SCALING_FACTOR);
        const Vec2d b(e.vertex1()->x() * SCALING_FACTOR, e.vertex1()->y() * SCALING_FACTOR);
        if (!is_inside(Vec2d((a + b) * 0.5)))
            continue;
        if (!is_inside(a) || !is_inside(b))
            continue;
        const int ia = node_at(a);
        const int ib = node_at(b);
        link(ia, ib);
    }
}

// ── fusión de cruces partidos (trampa 2) ────────────────────────────────────
// El Voronoi parte un cruce en varios vértices separados por micras. Dos CRUCES (grado ≥ 3) unidos
// por una cadena más corta que `tol` son el mismo cruce. 🚨 Fusionar por distancia sin mirar el
// grado se come el esqueleto entero.
void StrokeSkeleton::merge_junctions(double tol_mm)
{
    if (tol_mm <= 0.)
        return;
    std::vector<int> junc;
    for (size_t i = 0; i < m_pos.size(); ++i)
        if (m_alive[i] && degree(int(i)) >= 3)
            junc.push_back(int(i));
    if (junc.size() < 2)
        return;

    std::vector<int> rep(m_pos.size());
    for (size_t i = 0; i < rep.size(); ++i)
        rep[i] = int(i);
    std::function<int(int)> root = [&](int k) {
        while (rep[k] != k) {
            rep[k] = rep[rep[k]];
            k      = rep[k];
        }
        return k;
    };

    bool merged = false;
    for (int s : junc) {
        for (int nb : std::vector<int>(m_adj[s])) {
            std::vector<int> chain{ s };
            int    prev = s, cur = nb;
            double length = (m_pos[s] - m_pos[nb]).norm();
            while (degree(cur) == 2 && length < tol_mm) {
                int nxt = (m_adj[cur][0] == prev) ? m_adj[cur][1] : m_adj[cur][0];
                chain.push_back(cur);
                length += (m_pos[cur] - m_pos[nxt]).norm();
                prev = cur;
                cur  = nxt;
            }
            if (length >= tol_mm || degree(cur) < 3 || cur == s)
                continue;
            chain.push_back(cur);
            for (size_t i = 1; i < chain.size(); ++i) {
                const int ra = root(s), rb = root(chain[i]);
                if (ra != rb) {
                    rep[rb] = ra;
                    merged  = true;
                }
            }
        }
    }
    if (!merged)
        return;

    // Cada grupo colapsa a un nodo, colocado en el centro de sus CRUCES (si los hay).
    std::map<int, std::vector<int>> groups;
    for (size_t i = 0; i < m_pos.size(); ++i)
        if (m_alive[i])
            groups[root(int(i))].push_back(int(i));
    std::vector<std::vector<int>> new_adj(m_pos.size());
    for (const auto& kv : groups) {
        std::vector<int> jj;
        for (int k : kv.second)
            if (degree(k) >= 3)
                jj.push_back(k);
        if (jj.empty())
            jj = kv.second;
        Vec2d c = Vec2d::Zero();
        for (int k : jj)
            c += m_pos[k];
        m_pos[kv.first] = c / double(jj.size());
        if (kv.second.size() > 1)
            m_stats.merged += kv.second.size() - 1;
    }
    for (size_t a = 0; a < m_pos.size(); ++a) {
        if (!m_alive[a])
            continue;
        const int ra = root(int(a));
        for (int b : m_adj[a]) {
            const int rb = root(b);
            if (ra == rb)
                continue;
            auto& A = new_adj[ra];
            if (std::find(A.begin(), A.end(), rb) == A.end())
                A.push_back(rb);
            auto& B = new_adj[rb];
            if (std::find(B.begin(), B.end(), ra) == B.end())
                B.push_back(ra);
        }
    }
    for (size_t i = 0; i < m_pos.size(); ++i) {
        if (!m_alive[i])
            continue;
        if (root(int(i)) != int(i)) {
            m_alive[i] = false;
            m_adj[i].clear();
        } else {
            m_adj[i] = std::move(new_adj[i]);
        }
    }
}

// Disco de radio r alrededor de p, en coordenadas escaladas. 32 lados, como el buffer(…, 8) de
// shapely que usa el prototipo.
static Polygon disc(const Vec2d& p, double r)
{
    Polygon out;
    out.points.reserve(32);
    for (int i = 0; i < 32; ++i) {
        const double a = 2. * PI * double(i) / 32.;
        out.points.emplace_back(scaled<coord_t>(p.x() + r * std::cos(a)), scaled<coord_t>(p.y() + r * std::sin(a)));
    }
    return out;
}

static double area_mm2(const ExPolygons& e)
{
    double a = 0.;
    for (const ExPolygon& ex : e)
        a += ex.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

// ── poda de ramitas (trampas 1 y 12) ────────────────────────────────────────
// Una rama de punta se quita si es CORTA (menos de `factor` veces el radio de su base: la esquina
// de un remate plano mide exactamente r·√2) **y además** no deja área sin cubrir. 🚨 Sólo con el
// largo, en una letra con junturas gordas — la A — el radio de la base es grande y la poda se
// lleva patas enteras de la letra (trampa 12). Y se podan TODAS las hojas de una pasada: de una en
// una, al quitar el primer bigote de un remate su base baja a grado 2 y el segundo ya no es una
// ramita (trampa 1).
void StrokeSkeleton::prune(const ExPolygon& island, double factor, double area_tol)
{
    if (factor <= 0.)
        return;
    AABBTreeLines::LinesDistancer<Linef> bnd{ to_unscaled_linesf(ExPolygons{ island }) };
    std::vector<double> rad(m_pos.size(), 0.);
    double              rad_max = 0.;
    for (size_t i = 0; i < m_pos.size(); ++i)
        if (m_alive[i]) {
            rad[i]  = std::abs(bnd.distance_from_lines<false>(m_pos[i]));
            rad_max = std::max(rad_max, rad[i]);
        }

    for (int pass = 0; pass < 20; ++pass) {
        ++m_stats.prune_passes;
        std::vector<std::vector<int>> cand;
        for (size_t leaf = 0; leaf < m_pos.size(); ++leaf) {
            if (!m_alive[leaf] || degree(int(leaf)) != 1)
                continue;
            std::vector<int> chain{ int(leaf) };
            int    prev = int(leaf), cur = m_adj[leaf][0];
            double length = (m_pos[leaf] - m_pos[cur]).norm();
            const double limit = factor * rad_max;
            while (degree(cur) == 2 && length < limit) {
                const int nxt = (m_adj[cur][0] == prev) ? m_adj[cur][1] : m_adj[cur][0];
                chain.push_back(cur);
                length += (m_pos[cur] - m_pos[nxt]).norm();
                prev = cur;
                cur  = nxt;
            }
            if (degree(cur) < 3)
                continue;   // componente suelta, o cadena demasiado larga
            if (length < factor * rad[cur])
                cand.push_back(std::move(chain));
        }
        if (cand.empty())
            break;

        // 🚨 RENDIMIENTO. `others` del prototipo es "todos los nodos menos esta cadena y los ya
        // condenados", y unir un disco por nodo con Clipper es O(nodos) por pasada: en una letra
        // son 300 discos, en una isla de 47 x 100 mm son decenas de miles, y la poda corre DOS
        // veces por isla. Era el laminado atascado al 15 % (s325).
        //
        // Se mide exactamente lo mismo sin unir el esqueleto entero: un disco lejano no puede
        // tocar los discos de la rama, así que sólo entran los nodos cuya caja, engordada por su
        // radio y por el mayor de la rama, corta la caja de la rama. El resultado es idéntico —
        // `mine.difference(others)` no cambia si se quitan discos que no intersecan `mine` — y la
        // unión pasa de N a un puñado.
        std::vector<char> doomed(m_pos.size(), 0);
        std::vector<char> is_mine(m_pos.size(), 0);
        size_t            n_doomed = 0;
        for (size_t ci = 0; ci < cand.size(); ++ci) {
            Vec2d  lo(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
            Vec2d  hi(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
            double r_chain = 0.;
            Polygons mine;
            for (int k : cand[ci]) {
                const double r = std::max(rad[k], 1e-4);
                r_chain = std::max(r_chain, r);
                lo = lo.cwiseMin(m_pos[k]);
                hi = hi.cwiseMax(m_pos[k]);
                mine.emplace_back(disc(m_pos[k], r));
                is_mine[k] = 1;
            }
            Polygons others;
            for (size_t i = 0; i < m_pos.size(); ++i) {
                if (!m_alive[i] || doomed[i])
                    continue;
                if (is_mine[i])
                    continue;                      // los nodos de ESTA rama no cuentan
                const double r    = std::max(rad[i], 1e-4);
                const double slack = r + r_chain;
                if (m_pos[i].x() < lo.x() - slack || m_pos[i].x() > hi.x() + slack ||
                    m_pos[i].y() < lo.y() - slack || m_pos[i].y() > hi.y() + slack)
                    continue;                      // ni rozándose: no puede aportar nada
                others.emplace_back(disc(m_pos[i], r));
            }
            if (!others.empty() &&
                area_mm2(diff_ex(union_ex(mine), union_ex(others))) < area_tol) {
                for (int k : cand[ci])
                    if (!doomed[k]) {
                        doomed[k] = 1;
                        ++n_doomed;
                    }
            }
            // `others` vacío = no queda nada alrededor: la rama ES el esqueleto ahí, no se poda.
            for (int k : cand[ci])
                is_mine[k] = 0;
        }
        if (n_doomed == 0)
            break;
        for (size_t i = 0; i < m_pos.size(); ++i)
            if (doomed[i]) {
                unlink_node(int(i));
                ++m_stats.pruned;
            }
    }
    // Nodos que se han quedado sin vecinos.
    for (size_t i = 0; i < m_pos.size(); ++i)
        if (m_alive[i] && m_adj[i].empty())
            m_alive[i] = false;
}

void StrokeSkeleton::compute_radii(const ExPolygon& island)
{
    AABBTreeLines::LinesDistancer<Linef> bnd{ to_unscaled_linesf(ExPolygons{ island }) };
    m_r.assign(m_pos.size(), 0.);
    for (size_t i = 0; i < m_pos.size(); ++i)
        if (m_alive[i])
            m_r[i] = std::abs(bnd.distance_from_lines<false>(m_pos[i]));
}

// ── trazos ──────────────────────────────────────────────────────────────────
// Una rama = el tramo entre dos nodos de grado != 2. Los ciclos puros (una O sin cruces) no tienen
// ningún nodo especial y se recogen aparte.
void StrokeSkeleton::extract_branches()
{
    auto make = [&](const std::vector<int>& keys, bool closed) {
        StrokeBranch b;
        b.closed = closed;
        b.pts.reserve(keys.size());
        b.r.reserve(keys.size());
        for (int k : keys) {
            b.pts.push_back(m_pos[k]);
            b.r.push_back(m_r[k]);
        }
        for (size_t i = 1; i < b.pts.size(); ++i)
            b.length_mm += (b.pts[i] - b.pts[i - 1]).norm();
        b.deg_front = degree(keys.front());
        b.deg_back  = degree(keys.back());
        m_branches.push_back(std::move(b));
    };

    std::map<std::pair<int, int>, char> seen;
    std::vector<char>                   done(m_pos.size(), 0);
    for (size_t s = 0; s < m_pos.size(); ++s) {
        if (!m_alive[s] || degree(int(s)) == 2)
            continue;
        for (int nb : std::vector<int>(m_adj[s])) {
            if (seen.count({ int(s), nb }))
                continue;
            std::vector<int> keys{ int(s) };
            int prev = int(s), cur = nb;
            seen.emplace(std::make_pair(int(s), nb), 0);
            while (true) {
                keys.push_back(cur);
                if (degree(cur) != 2)
                    break;
                const int nxt = (m_adj[cur][0] == prev) ? m_adj[cur][1] : m_adj[cur][0];
                prev = cur;
                cur  = nxt;
            }
            seen.emplace(std::make_pair(keys.back(), keys[keys.size() - 2]), 0);
            for (int k : keys)
                done[k] = 1;
            make(keys, false);
        }
    }
    for (size_t k0 = 0; k0 < m_pos.size(); ++k0) {
        if (!m_alive[k0] || done[k0] || m_adj[k0].empty())
            continue;
        std::vector<int> keys{ int(k0) };
        int prev = int(k0), cur = m_adj[k0][0];
        while (cur != int(k0)) {
            keys.push_back(cur);
            done[cur] = 1;
            int nxt = -1;
            for (int x : m_adj[cur])
                if (x != prev) {
                    nxt = x;
                    break;
                }
            if (nxt < 0)
                break;
            prev = cur;
            cur  = nxt;
        }
        done[k0] = 1;
        keys.push_back(int(k0));
        make(keys, true);
    }
}

std::vector<Vec2d> StrokeSkeleton::longest_path() const
{
    // El pedazo mayor primero: el esqueleto de un engorde sale partido en varios.
    std::vector<char> seen(m_pos.size(), 0);
    std::vector<int>  best;
    for (size_t k0 = 0; k0 < m_pos.size(); ++k0) {
        if (!m_alive[k0] || seen[k0])
            continue;
        std::vector<int> comp, stack{ int(k0) };
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            if (seen[c])
                continue;
            seen[c] = 1;
            comp.push_back(c);
            for (int nb : m_adj[c])
                if (!seen[nb])
                    stack.push_back(nb);
        }
        if (comp.size() > best.size())
            best = std::move(comp);
    }
    if (best.size() < 2)
        return {};

    auto far = [&](int src, std::vector<int>& prev) {
        std::map<int, double> dist;
        std::priority_queue<std::pair<double, int>, std::vector<std::pair<double, int>>,
                            std::greater<std::pair<double, int>>> pq;
        prev.assign(m_pos.size(), -1);
        dist[src] = 0.;
        pq.push({ 0., src });
        std::vector<char> done(m_pos.size(), 0);
        int    end = src;
        double bestd = -1.;
        while (!pq.empty()) {
            const auto [d, c] = pq.top();
            pq.pop();
            if (done[c])
                continue;
            done[c] = 1;
            if (d > bestd) {
                bestd = d;
                end   = c;
            }
            for (int nb : m_adj[c]) {
                const double nd = d + (m_pos[c] - m_pos[nb]).norm();
                auto it = dist.find(nb);
                if (it == dist.end() || nd < it->second - 1e-12) {
                    dist[nb] = nd;
                    prev[nb] = c;
                    pq.push({ nd, nb });
                }
            }
        }
        return end;
    };
    std::vector<int> prev;
    const int a = far(best.front(), prev);
    const int b = far(a, prev);
    std::vector<Vec2d> out;
    for (int c = b; c >= 0; c = prev[c])
        out.push_back(m_pos[c]);
    return out;
}

StrokeSkeleton::StrokeSkeleton(const ExPolygon& island, const SkeletonParams& p)
{
    build(island, p.ds_mm);
    prune(island, p.prune_mm, p.prune_area_mm2);
    merge_junctions(p.merge_mm);
    prune(island, p.prune_mm, p.prune_area_mm2);
    compute_radii(island);
    extract_branches();

    for (size_t i = 0; i < m_pos.size(); ++i) {
        if (!m_alive[i])
            continue;
        ++m_stats.nodes;
        const size_t d = degree(int(i));
        if (d == 1)
            ++m_stats.leaves;
        else if (d >= 3)
            ++m_stats.junctions;
    }
    m_stats.branches = m_branches.size();
}

}} // namespace Slic3r::NeoArachne
