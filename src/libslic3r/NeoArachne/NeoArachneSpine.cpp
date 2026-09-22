// NEOTKO_NEOARACHNE_TAG v3-spine (s323)
#include "NeoArachneSpine.hpp"

#include "../PerimeterGenerator.hpp"
#include "../PrintConfig.hpp"
#include "../ClipperUtils.hpp"
#include "../VariableWidth.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Geometry/MedialAxis.hpp"
#include "../Line.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

namespace Slic3r { namespace NeoArachne {

bool fits_in_one_line(const ExPolygon& ex, double ceiling_scaled)
{
    return offset_ex(ExPolygons{ ex }, -float(ceiling_scaled / 2.)).empty();
}

// ── Limpieza del esqueleto (fase 2) ─────────────────────────────────────────
// MedialAxis corta las líneas en cada cruce y deja, en cada punta afilada, ramitas cortas hacia
// las esquinas. Sin limpiar, esas ramitas se prolongan hasta el borde y se montan sobre la línea
// principal, y cada rama es un camino suelto.
//
// s323, lo aprendido con la lámina y la S:
//   · Las ramas de un mismo cruce NO comparten el punto exacto (el Voronoi deja el cruce partido
//     en vértices separados por micras) ⇒ los nodos se agrupan por PROXIMIDAD.
//   · Un hueco que se pellizca hasta un punto queda en DOS zonas que se tocan ahí (el centro de
//     la S) ⇒ el grafo se limpia con TODAS las zonas a la vez, no zona a zona.
//   · Una ramita siempre es estrecha (va a una esquina), así que compararla con su propio ancho casi
//     nunca la poda ⇒ se compara con el grosor del hueco en el nodo donde nace.

// Línea del esqueleto + la zona a la que pertenece cada extremo (para prolongarlo hasta SU borde).
struct SPl {
    ThickPolyline pl;
    size_t        zf = 0;   // zona del extremo inicial
    size_t        zb = 0;   // zona del extremo final
};

struct EndRef { size_t pl; bool back; };

static bool alive(const SPl& s) { return s.pl.points.size() >= 2; }
static bool is_closed_pl(const ThickPolyline& pl) { return pl.points.front() == pl.points.back(); }

static const Point& end_point(const ThickPolyline& pl, bool back) { return back ? pl.points.back() : pl.points.front(); }
static double end_width(const ThickPolyline& pl, bool back) { return back ? pl.width.back() : pl.width.front(); }

static double max_width(const ThickPolyline& pl)
{
    return pl.width.empty() ? 0. : *std::max_element(pl.width.begin(), pl.width.end());
}

static void reverse_spl(SPl& s)
{
    s.pl.reverse();
    std::swap(s.zf, s.zb);
}

// Dirección con la que la línea SALE del nodo por el extremo indicado.
static Vec2d leave_dir(const ThickPolyline& pl, bool back)
{
    const Point& n = back ? pl.points.back() : pl.points.front();
    const Point& q = back ? pl.points[pl.points.size() - 2] : pl.points[1];
    Vec2d d = (q - n).cast<double>();
    const double l = d.norm();
    return l > 0. ? Vec2d(d / l) : d;
}

// Pega `b` a continuación de `a` por su nodo común y deja `b` vacía. Si los extremos no coinciden
// exactamente (cruce partido), el hueco se cubre con un tramo puente.
static void join_at(SPl& a, bool a_back, SPl& b, bool b_back)
{
    if (!a_back) reverse_spl(a);   // `a` termina en el nodo
    if (b_back)  reverse_spl(b);   // `b` empieza en el nodo
    ThickPolyline& A = a.pl;
    ThickPolyline& B = b.pl;
    if (A.points.back() == B.points.front()) {
        A.points.insert(A.points.end(), B.points.begin() + 1, B.points.end());
        A.width.insert(A.width.end(), B.width.begin(), B.width.end());
    } else {
        const coordf_t wa = A.width.back();
        const coordf_t wb = B.width.front();
        A.points.insert(A.points.end(), B.points.begin(), B.points.end());
        A.width.push_back(wa);
        A.width.push_back(wb);
        A.width.insert(A.width.end(), B.width.begin(), B.width.end());
    }
    A.endpoints.second = B.endpoints.second;
    a.zb = b.zb;
    B.points.clear();
    B.width.clear();
}

// Nodos = grupos de extremos cercanos. `node_of[(pl, back)]` da el nodo de cada extremo.
struct Graph {
    std::vector<std::vector<EndRef>>          nodes;
    std::map<std::pair<size_t, bool>, size_t> node_of;
    size_t degree_of(size_t pl, bool back) const { return nodes[node_of.at({ pl, back })].size(); }
    // Grosor del hueco en el nodo: el mayor ancho de los extremos que llegan a él.
    double node_width(const std::vector<SPl>& pls, size_t node) const
    {
        double w = 0.;
        for (const EndRef& e : nodes[node])
            w = std::max(w, end_width(pls[e.pl].pl, e.back));
        return w;
    }
};

static Graph build_graph(const std::vector<SPl>& pls, double max_tol)
{
    std::vector<EndRef> ends;
    for (size_t i = 0; i < pls.size(); ++i) {
        if (!alive(pls[i]) || is_closed_pl(pls[i].pl))
            continue;
        ends.push_back({ i, false });
        ends.push_back({ i, true });
    }
    std::vector<size_t> parent(ends.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto root = [&](size_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
    for (size_t a = 0; a < ends.size(); ++a)
        for (size_t b = a + 1; b < ends.size(); ++b) {
            const ThickPolyline& pa = pls[ends[a].pl].pl;
            const ThickPolyline& pb = pls[ends[b].pl].pl;
            // Medio ancho de línea en ese punto (+ margen), sin pasar de medio techo.
            const double tol = std::min(0.6 * std::max(end_width(pa, ends[a].back), end_width(pb, ends[b].back))
                                            + 2. * SCALED_EPSILON, max_tol);
            if ((end_point(pa, ends[a].back) - end_point(pb, ends[b].back)).cast<double>().squaredNorm() <= tol * tol)
                parent[root(a)] = root(b);
        }
    Graph g;
    std::map<size_t, size_t> root_to_node;
    for (size_t i = 0; i < ends.size(); ++i) {
        const size_t r = root(i);
        auto it = root_to_node.find(r);
        if (it == root_to_node.end()) {
            it = root_to_node.emplace(r, g.nodes.size()).first;
            g.nodes.emplace_back();
        }
        g.nodes[it->second].push_back(ends[i]);
        g.node_of[{ ends[i].pl, ends[i].back }] = it->second;
    }
    return g;
}

static void kill(SPl& s) { s.pl.points.clear(); s.pl.width.clear(); }

static void clean_graph(std::vector<SPl>& pls, double ceiling_w, SpineStats& s)
{
    const double max_tol = 0.5 * ceiling_w;
    for (int guard = 0; guard < 100000; ++guard) {
        const Graph gr = build_graph(pls, max_tol);

        // (0) Conector del propio cruce: una línea cortita con los dos extremos en el mismo nodo.
        bool changed = false;
        for (size_t i = 0; i < pls.size() && !changed; ++i) {
            if (!alive(pls[i]) || is_closed_pl(pls[i].pl))
                continue;
            if (gr.node_of.at({ i, false }) == gr.node_of.at({ i, true }) && pls[i].pl.length() <= 2. * max_tol) {
                kill(pls[i]);
                changed = true;
            }
        }
        if (changed)
            continue;

        // (a) Poda: la ramita más corta que cuelga de un cruce con la otra punta libre y es más
        //     corta que 1.5 × el grosor del hueco en ese cruce. De una en una, para no podar todas
        //     las ramas de una Y pequeña.
        size_t best_pl = std::numeric_limits<size_t>::max();
        double best_ratio = 1.5;
        for (size_t n = 0; n < gr.nodes.size(); ++n) {
            const auto& ends = gr.nodes[n];
            if (ends.size() < 3)
                continue;
            const double w = gr.node_width(pls, n);
            if (w <= 0.)
                continue;
            for (const EndRef& e : ends) {
                if (gr.degree_of(e.pl, !e.back) != 1)
                    continue;
                const double ratio = pls[e.pl].pl.length() / w;
                if (ratio < best_ratio) { best_ratio = ratio; best_pl = e.pl; }
            }
        }
        if (best_pl != std::numeric_limits<size_t>::max()) {
            kill(pls[best_pl]);
            ++s.spurs_pruned;
            continue;
        }

        // (b) Nodo de paso (grado 2): las dos mitades son la misma línea.
        for (const auto& ends : gr.nodes) {
            if (ends.size() == 2 && ends[0].pl != ends[1].pl) {
                join_at(pls[ends[0].pl], ends[0].back, pls[ends[1].pl], ends[1].back);
                changed = true;
                break;
            }
        }
        if (changed)
            continue;

        // (c) Cruce: se encadenan las dos ramas más alineadas (las que siguen recto). Una Y de 3
        //     ramas queda en 2 líneas, como en S3D.
        for (const auto& ends : gr.nodes) {
            if (ends.size() < 3)
                continue;
            double best = 2.;
            size_t bi = 0, bj = 0;
            for (size_t i = 0; i < ends.size(); ++i)
                for (size_t j = i + 1; j < ends.size(); ++j) {
                    if (ends[i].pl == ends[j].pl)
                        continue;
                    const double d = leave_dir(pls[ends[i].pl].pl, ends[i].back).dot(leave_dir(pls[ends[j].pl].pl, ends[j].back));
                    if (d < best) { best = d; bi = i; bj = j; }
                }
            if (best < 2.) {
                join_at(pls[ends[bi].pl], ends[bi].back, pls[ends[bj].pl], ends[bj].back);
                ++s.junction_joins;
                changed = true;
                break;
            }
        }
        if (!changed)
            break;
    }
    pls.erase(std::remove_if(pls.begin(), pls.end(), [](const SPl& x) { return !alive(x); }), pls.end());

    // Bucles cerrados más pequeños que su propio ancho: son un borrón, no una línea (la `a`, s323).
    pls.erase(std::remove_if(pls.begin(), pls.end(), [&s](const SPl& x) {
                  if (!is_closed_pl(x.pl) || x.pl.length() >= M_PI * max_width(x.pl))
                      return false;
                  ++s.tiny_loops_dropped;
                  return true;
              }), pls.end());

    // Extremos libres según el grafo final (no según MedialAxis): una punta que se ha quedado sola
    // tras podar sus ramitas también debe llegar al borde; una que toca a otra línea, no.
    const Graph gr = build_graph(pls, max_tol);
    for (size_t i = 0; i < pls.size(); ++i) {
        ThickPolyline& pl = pls[i].pl;
        if (is_closed_pl(pl)) {
            pl.endpoints = { false, false };
            continue;
        }
        pl.endpoints.first  = gr.degree_of(i, false) == 1;
        pl.endpoints.second = gr.degree_of(i, true) == 1;
    }
}

// Punto del borde de `ex` (contorno o agujero) al que llega el extremo `tip` siguiendo su
// dirección (siendo `inner` el punto anterior de la línea). Es el mismo gesto que
// ExPolygon::medial_axis, pero aquel sólo prueba el contorno y aquí el hueco puede ser un
// anillo (el agujero de la `a`).
static bool boundary_hit(const ExPolygon& ex, const Point& inner, const Point& tip, double reach, Point& hit)
{
    const Vec2d a   = inner.cast<double>();
    const Vec2d b   = tip.cast<double>();
    Vec2d       dir = b - a;
    const double n  = dir.norm();
    if (n < SCALED_EPSILON)
        return false;
    dir /= n;
    const Line ray(tip, (b + dir * reach).cast<coord_t>());

    double best  = std::numeric_limits<double>::max();
    bool   found = false;
    auto test = [&](const Polygon& poly) {
        for (const Line& l : poly.lines()) {
            Point ip;
            if (line_alg::intersection(ray, l, &ip)) {
                const double d = (ip - tip).cast<double>().squaredNorm();
                if (d < best) { best = d; hit = ip; found = true; }
            }
        }
    };
    test(ex.contour);
    for (const Polygon& h : ex.holes)
        test(h);
    return found && hit != tip;
}

ExPolygons run_spine(PerimeterGenerator& g, const ExPolygons& regions, const SpineParams& p, SpineStats* stats,
                     ExPolygons* rejected, const ExPolygons* forced, ExtrusionEntitiesPtr* out)
{
    SpineStats  local;
    SpineStats& s = stats ? *stats : local;
    ExPolygons  consumed;

    const double floor_w   = scaled<double>(std::max(p.floor_mm, 0.001));
    const double ceiling_w = scaled<double>(std::max(p.ceiling_mm, p.floor_mm));
    const double min_len   = scaled<double>(std::max(p.min_length_mm, 0.));
    const double sliver_w  = scaled<double>(std::max(p.sliver_mm, 0.));
    // El esqueleto se busca hasta anchos muy pequeños para que la línea llegue a la punta; el
    // suelo se aplica después, al ancho de la línea, nunca como corte (regla 3).
    const double detect_min = std::max(double(SCALED_EPSILON) * 4., 0.25 * floor_w);
    const double detect_max = ceiling_w * 1.05;
    const double simplify   = scaled<double>(std::max(g.print_config->resolution.value, 0.005));
    const double mm2        = SCALING_FACTOR * SCALING_FACTOR;

    // 1. Esqueleto de cada zona (simplificada; se guarda para prolongar las puntas hasta su borde).
    ExPolygons       zones;
    std::vector<SPl> pls;
    std::vector<std::pair<const ExPolygon*, bool>> input;   // (zona, forzada)
    for (const ExPolygon& c : regions)
        input.push_back({ &c, false });
    if (forced)
        for (const ExPolygon& c : *forced)
            input.push_back({ &c, true });
    for (const auto& [comp_ptr, is_forced] : input) {
        const ExPolygon& comp = *comp_ptr;
        ++s.components;
        const double a = comp.area();
        if (a <= 0.)
            continue;
        if (!is_forced && !fits_in_one_line(comp, ceiling_w)) {
            ++s.wide_components;
            s.wide_area_mm2 += a * mm2;
            if (rejected) rejected->push_back(comp);
            continue;
        }
        ExPolygon ex = comp;
        ex.douglas_peucker(simplify);
        ThickPolylines tp;
        // Una zona forzada puede pasar del techo: el esqueleto se busca entero (si no, MedialAxis
        // descarta las aristas más anchas que su máximo y la línea sale cortada) y el ancho se
        // recorta al techo después.
        Geometry::MedialAxis ma(detect_min, is_forced ? ceiling_w * 4. : detect_max, ex);
        ma.build(&tp);
        tp.erase(std::remove_if(tp.begin(), tp.end(), [](const ThickPolyline& pl) { return pl.points.size() < 2; }), tp.end());
        if (tp.empty()) {
            // Sin espina útil (p. ej. un cuadradito: su esqueleto son esquinas que validate_edge
            // descarta). A relleno en vez de quedarse vacío (ancla, s323).
            if (rejected) rejected->push_back(comp);
            continue;
        }
        if (sliver_w > 0.) {
            double wmax = 0.;
            for (const ThickPolyline& pl : tp)
                wmax = std::max(wmax, max_width(pl));
            if (wmax < sliver_w) {   // grieta: el hueco nunca llega al umbral
                ++s.slivers_skipped;
                continue;
            }
        }
        const size_t z = zones.size();
        zones.push_back(std::move(ex));
        consumed.push_back(comp);
        ++s.spine_components;
        s.spine_area_mm2 += a * mm2;
        for (ThickPolyline& pl : tp)
            pls.push_back({ std::move(pl), z, z });
    }

    // 2. Limpieza con todas las zonas a la vez (un hueco pellizcado sigue siendo UNA línea).
    clean_graph(pls, ceiling_w, s);

    // 3. Puntas al borde, suelo/techo y largo mínimo. A diferencia de ExPolygon::medial_axis NO se
    //    podan las ramas por el ancho máximo encontrado (mataría los huecos pequeños que S3D sí
    //    imprime); sólo cuenta el largo mínimo.
    ThickPolylines all;
    for (SPl& sp : pls) {
        ThickPolyline& pl = sp.pl;
        // Grieta DENTRO de una zona buena (s323: el final del arco de arriba de la S, 1 mm a ancho
        // de suelo, separado de la línea principal): la regla de la zona no la ve porque la zona
        // sí llega al suelo gracias a la línea gorda. Se mira también línea a línea.
        if (sliver_w > 0. && max_width(pl) < sliver_w) {
            ++s.slivers_skipped;
            continue;
        }
        // Puntas al borde con un tramo NUEVO que se afina hasta el suelo. s323: estirar el último
        // tramo dejaba la prolongación al ancho del final de la línea (0.85 en el hueco de la S,
        // 0.7–0.9 mm de largo) metiéndose donde el hueco ya se cierra = material encima del muro.
        if (pl.endpoints.first) {
            Point tip;
            if (boundary_hit(zones[sp.zf], pl.points[1], pl.points.front(), ceiling_w, tip)) {
                const coordf_t w0 = pl.width.front();
                pl.points.insert(pl.points.begin(), tip);
                pl.width.insert(pl.width.begin(), { coordf_t(floor_w), w0 });
            }
        }
        if (pl.endpoints.second) {
            Point tip;
            if (boundary_hit(zones[sp.zb], pl.points[pl.points.size() - 2], pl.points.back(), ceiling_w, tip)) {
                const coordf_t w1 = pl.width.back();
                pl.points.push_back(tip);
                pl.width.push_back(w1);
                pl.width.push_back(coordf_t(floor_w));
            }
        }
        for (coordf_t& w : pl.width)
            w = std::clamp<coordf_t>(w, floor_w, ceiling_w);   // reglas 3 y 4
        const double len = pl.length();
        if (len < min_len) {
            ++s.dropped_short;
            continue;
        }
        s.spine_len_mm += unscale<double>(len);
        all.emplace_back(std::move(pl));
    }
    s.polylines += all.size();

    if (!all.empty()) {
        // Mismo conversor que el gap-fill de Classic: el ancho de la ThickPolyline es el hueco y
        // el volumen resultante es hueco × altura, igual que el W de S3D.
        if (out != nullptr) {
            variable_width(all, erGapFill, g.solid_infill_flow, *out);
        } else if (g.gap_fill != nullptr) {
            ExtrusionEntityCollection coll;
            variable_width(all, erGapFill, g.solid_infill_flow, coll.entities);
            g.gap_fill->append(std::move(coll.entities));
        }
    }
    return consumed;
}

}} // namespace Slic3r::NeoArachne
