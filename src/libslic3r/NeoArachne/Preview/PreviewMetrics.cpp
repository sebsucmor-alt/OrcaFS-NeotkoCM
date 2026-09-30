// NEOTKO_NEOSTROKE_TAG s337 — ver la cabecera.
#include "PreviewMetrics.hpp"

#include "../../ClipperUtils.hpp"
#include "../../BoundingBox.hpp"
#include "../../Polyline.hpp"
#include "../../libslic3r.h"
#include "../../NeoDebug.hpp"   // s342f — sonda de tiempos

#include <chrono>
#include <oneapi/tbb/parallel_for.h>   // s342g — la unión de huellas por trozos
#include <cstdio>
#include <string>

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace Slic3r { namespace NeoArachne { namespace Preview {

void LayerMetrics::add(const LayerMetrics& o)
{
    interior_mm    += o.interior_mm;
    interior_ok_mm += o.interior_ok_mm;
    classic_mm     += o.classic_mm;
    starts         += o.starts;
    thin_loose_mm    += o.thin_loose_mm;
    thin_squeezed_mm += o.thin_squeezed_mm;
    tip_risk_count   += o.tip_risk_count;
    tip_risk_mm      += o.tip_risk_mm;
    seam_risk_mm     += o.seam_risk_mm;
    double_pass_mm   += o.double_pass_mm;
    excess_mm2       += o.excess_mm2;
    knot_count       += o.knot_count;
    contact_mm       += o.contact_mm;
    sustained_mm2    += o.sustained_mm2;
    vol_mm3        += o.vol_mm3;
    for (int i = 0; i < kGapKinds; ++i) {
        gap_count[i] += o.gap_count[i];
        gap_mm2[i]   += o.gap_mm2[i];
    }
}

namespace {

inline bool extrudes(const OrderedSegment& s) { return !s.is_travel && s.mm3_per_mm > 0.0; }

struct Piece {
    Polygons    polys;
    BoundingBox bb;
};

// La huella de un tramo, como la dibuja `s336_capa_huella.py`: una línea de ancho sep con remates
// redondos. Se usa el ancho de CAUDAL (lo que de verdad cae), no el nominal.
// 🚨 Tolerancia de arco de 5 µm: con la de por defecto cada remate sale con cientos de vértices y las
//    uniones de Clipper se eternizan (memoria `lesson_clipper_jtround_arc_tolerance`).
Polygons segment_footprint(const OrderedSegment& s)
{
    const double h   = s.height > 0.f ? double(s.height) : 0.2;
    const double sep = footprint_sep(double(s.path_width_flow > 0.f ? s.path_width_flow : s.path_width), h);
    if (sep <= 0.0)
        return {};
    Polyline pl;
    pl.points = { s.from, s.to };
    return offset(pl, float(scaled<double>(0.5 * sep)), ClipperLib::jtRound, scaled<double>(0.005),
                  ClipperLib::etOpenRound);
}

bool touches(const Polygons& grown, const BoundingBox& gbb, const std::vector<Piece>& pieces)
{
    for (const Piece& p : pieces) {
        if (!p.bb.overlap(gbb))
            continue;
        if (!intersection(grown, p.polys).empty())
            return true;
    }
    return false;
}

} // namespace

MetricsResult compute_layer_metrics(const PreviewResult& r, const MetricsOptions& opt)
{
    MetricsResult out;
    // s342f — sonda de tiempos por parte (canal NEOSTROKE). `ns_mark` cierra la parte anterior.
    std::vector<std::pair<const char*, double>> ns_times;
    auto ns_t0 = std::chrono::steady_clock::now();
    auto ns_mark = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        ns_times.emplace_back(name, std::chrono::duration<double, std::milli>(now - ns_t0).count());
        ns_t0 = now;
    };
    LayerMetrics& m = out.m;
    // s339 — NIVEL DE AVISO: un solo número mueve todos los umbrales. > 1 avisa antes, < 1 sólo lo claro.
    const double lvl            = std::clamp(opt.level, 0.25, 4.0);
    const double knot_mm_eff    = opt.knot_mm * (0.5 + 0.5 * lvl);
    const double knot_chain_eff = opt.knot_chain_mm * (0.5 + 0.5 * lvl);
    const double excess_thr     = 1.0 + (opt.excess_ratio - 1.0) / lvl;
    const double sustained_thr  = 1.0 + (opt.sustained_ratio - 1.0) / lvl;
    const double contact_margin = opt.closure_mm + 0.02 * (lvl - 1.0);

    // ── cifras por tramo y recorridos (s336_zonas_por_caja.py / s336_capa_recorridos.py) ──
    Run  cur;
    bool have_cur = false;
    size_t cur_first = 0, cur_last = 0;
    std::vector<std::pair<size_t, size_t>> ns_runs;   // [primero, último] de cada recorrido de NeoStroke
    // s339 — a qué recorrido (índice en `out.runs`) pertenece cada tramo: el contacto no cuenta a los vecinos inmediatos
    //    del MISMO recorrido. `close_run` mete el recorrido en `out.runs` antes de abrir el siguiente.
    std::vector<int> seg_run(r.ordered_segments.size(), -1);
    auto close_run = [&]() {
        if (!have_cur) return;
        out.runs.push_back(cur);
        if (cur.neostroke) { ++m.starts; ns_runs.emplace_back(cur_first, cur_last); }
        have_cur = false;
    };
    for (size_t si = 0; si < r.ordered_segments.size(); ++si) {
        const OrderedSegment& s = r.ordered_segments[si];
        if (!extrudes(s)) {
            // Un viaje o un patín corta el recorrido: el siguiente tramo con plástico es un arranque.
            close_run();
            continue;
        }
        const double l = unscale<double>(s.length_scaled);
        const double h = s.height > 0.f ? double(s.height) : 0.2;
        if (s.neostroke) {
            m.interior_mm += l;
            if (footprint_sep(double(s.path_width), h) >= opt.sep_threshold_mm)
                m.interior_ok_mm += l;
            m.vol_mm3 += l * double(s.path_width) * h;
        } else {
            m.classic_mm += l;
        }
        if (have_cur && cur.neostroke == s.neostroke && cur.end == s.from) {
            cur.end        = s.to;
            cur.length_mm += l;
            cur_last       = si;
            seg_run[si]    = int(out.runs.size());
        } else {
            close_run();
            cur_first = cur_last = si;
            seg_run[si] = int(out.runs.size());
            cur.start     = s.from;
            cur.end       = s.to;
            cur.neostroke = s.neostroke;
            cur.length_mm = l;
            have_cur      = true;
        }
    }
    close_run();

    if (r.input_slices.empty())
        return out;

    ns_mark("recorridos");   // s342f — sonda de tiempos
    // ── huecos (s336_huecos_junta.py) ──
    // El script pinta la capa y busca píxeles negros encerrados; aquí es exacto: el corte menos la unión
    // de todas las huellas. Lo que queda y es pequeño (< gap_max_mm2) es un hueco; lo grande es una
    // contraforma o la zona del relleno, igual que el umbral de 0.3 mm² del script.
    std::vector<Piece> classic, neo;
    Polygons           all;
    for (const OrderedSegment& s : r.ordered_segments) {
        if (!extrudes(s))
            continue;
        Piece p;
        p.polys = segment_footprint(s);
        if (p.polys.empty())
            continue;
        p.bb = get_extents(p.polys);
        all.insert(all.end(), p.polys.begin(), p.polys.end());
        (s.neostroke ? neo : classic).push_back(std::move(p));
    }
    // s342g — 2.9 de los 3.1 s de las cifras eran ESTA unión (miles de huellas de tramo de golpe). Por trozos
    // consecutivos (el orden de impresión va isla a isla, así que cada trozo es casi una letra) unidos en paralelo, y
    // luego los trozos: el mismo territorio. Sólo es medida del visor; el G-code no pasa por aquí.
    ExPolygons covered;
    {
        const size_t n_chunks = std::clamp<size_t>(all.size() / 400, 1, 32);
        std::vector<Polygons> part(n_chunks);
        const size_t per = (all.size() + n_chunks - 1) / n_chunks;
        tbb::parallel_for(size_t(0), n_chunks, [&](size_t k) {
            const size_t a = k * per, b = std::min(all.size(), a + per);
            if (a < b)
                part[k] = to_polygons(union_ex(Polygons(all.begin() + a, all.begin() + b)));
        });
        Polygons merged;
        for (Polygons& pp : part)
            append(merged, std::move(pp));
        covered = union_ex(merged);
    }

    ns_mark("union");   // s342f — sonda de tiempos
    // ── regla de vecinos ──
    // Para cada tramo fino de NeoStroke se sondea a los DOS lados, justo pasado su borde (sep/2 + neighbour_mm):
    // si los dos puntos caen dentro de la huella de algún otro cordón, está entre vecinos. Si falta uno, es una
    // línea suelta o de borde (el borde de la isla deja un lado sin nada).
    {
        std::vector<BoundingBox> cbb;
        cbb.reserve(covered.size());
        for (const ExPolygon& e : covered) cbb.push_back(get_extents(e));
        auto inside = [&](const Point& p) {
            for (size_t i = 0; i < covered.size(); ++i)
                if (cbb[i].contains(p) && covered[i].contains(p))
                    return true;
            return false;
        };
        out.seg_flags.assign(r.ordered_segments.size(), sfNone);
        // ¿Tiene cordón a los dos lados? Se sondea justo pasado su borde (sep/2 + neighbour_mm).
        auto sides = [&](const OrderedSegment& s, bool& left, bool& right) -> bool {
            const double h   = s.height > 0.f ? double(s.height) : 0.2;
            const double sep = footprint_sep(double(s.path_width), h);
            const Vec2d a = unscale(s.from), b = unscale(s.to);
            const Vec2d d = b - a;
            const double len = d.norm();
            if (len < 1e-6) return false;
            const Vec2d  nrm(-d.y() / len, d.x() / len);
            const Vec2d  mid = 0.5 * (a + b);
            const double off = 0.5 * std::max(0.0, sep) + opt.neighbour_mm;
            left  = inside(Point::new_scale(mid.x() + nrm.x() * off, mid.y() + nrm.y() * off));
            right = inside(Point::new_scale(mid.x() - nrm.x() * off, mid.y() - nrm.y() * off));
            return true;
        };
        for (size_t i = 0; i < r.ordered_segments.size(); ++i) {
            const OrderedSegment& s = r.ordered_segments[i];
            if (!extrudes(s) || !s.neostroke)
                continue;
            const double h   = s.height > 0.f ? double(s.height) : 0.2;
            const double sep = footprint_sep(double(s.path_width), h);
            if (sep >= opt.sep_threshold_mm)
                continue;
            bool left = false, right = false;
            if (!sides(s, left, right))
                continue;
            const double l = unscale<double>(s.length_scaled);
            if (left && right) { out.seg_flags[i] |= sfThinSqueezed; m.thin_squeezed_mm += l; }
            else               { out.seg_flags[i] |= sfThinLoose;    m.thin_loose_mm    += l; }
        }

        ns_mark("vecinos");   // s342f — sonda de tiempos
        // ── riesgo de punta (TEST22 Z3/Z4/Z7) ──
        // El final del muro de Classic deja la punta de la cuña a NeoStroke solo: una línea sin nada a los lados
        // que ARRANCA ahí. A 15 mm/s agarra; a 30 y 60 la boquilla tira antes de que pegue y el cuello se rompe.
        if (opt.speed_mm_s > opt.tip_speed_limit) {
            auto walk = [&](size_t first, size_t last, bool forward) {
                double acc = 0.0; bool any = false;
                for (size_t k = 0; k <= last - first; ++k) {
                    const size_t i = forward ? first + k : last - k;
                    const OrderedSegment& s = r.ordered_segments[i];
                    if (!extrudes(s)) continue;
                    acc += unscale<double>(s.length_scaled);
                    bool left = false, right = false;
                    // 🚨 Suelta del TODO: sin nada a ninguno de los dos lados, como la punta de la cuña rota. Con
                    //    «falta un lado» salían 35 por zona en vez de 11-14 y marcaba extremos pegados a un vecino que
                    //    las fotos no enseñan rotos (verificado por transliteración sobre el G-code del TEST22).
                    if (sides(s, left, right) && !left && !right) {
                        out.seg_flags[i] |= sfTipRisk;
                        m.tip_risk_mm += unscale<double>(s.length_scaled);
                        any = true;
                    }
                    if (acc >= opt.tip_zone_mm) break;
                }
                return any;
            };
            for (const auto& [first, last] : ns_runs) {
                const bool a = walk(first, last, true);
                const bool b = walk(first, last, false);
                m.tip_risk_count += size_t(a) + size_t(b);
            }
        }

        ns_mark("punta");   // s342f — sonda de tiempos
        // ── s338: nudo de puntas (TEST24, la onda) ──
        // Un ARRANQUE de NeoStroke que (1) empieza en COLA AFILADA (ancho al arrancar < `knot_thin` × el ancho que
        // alcanza en su primer medio milímetro) y (2) tiene al menos `knot_others` puntas de OTROS recorridos
        // (arranques o paradas) a menos de `knot_mm`. Se marca su primer ~0.3 mm y los tramos de esas puntas.
        // 🚨 Sin (1) y con una sola punta vecina marcaba el 43 % de los arranques del TEST24: el camino siguiente que
        //    empieza donde acabó el anterior es lo normal y no deja hoyo. Con las dos condiciones marca el 26 %, que
        //    es real: casi todos los caminos del campo arrancaban en cola. Transliteración: `campo/nudos.py`.
        ns_mark("nudo");   // s342f — sonda de tiempos
        // ── s339: racimo de ARRANQUES (TEST25, el agujero de arriba de los anillos en las 8 zonas) ──
        // Las puntas de NS (arranques y paradas) se encadenan si están a menos de `knot_chain_mm` (enlace simple). Un racimo
        // con ≥ `knot_starts` arranques deja agujero aunque los arranques sean anchos: tras el viaje la presión no ha
        // llegado a ninguno, y sus remates redondos, que en el plan se pisan (el visor lo pintaba de NARANJA, como
        // sobra), no llegan a salir. Las paradas juntas no: llegan con presión de sobra (TEST25 zona 1, (4.2,−1.7)).
        std::vector<bool> run_in_racimo(ns_runs.size(), false);
        {
            struct Tip { Point p; size_t run; bool start; };
            std::vector<Tip> tips;
            tips.reserve(2 * ns_runs.size());
            for (size_t a = 0; a < ns_runs.size(); ++a) {
                tips.push_back({ r.ordered_segments[ns_runs[a].first].from, a, true });
                tips.push_back({ r.ordered_segments[ns_runs[a].second].to, a, false });
            }
            std::vector<size_t> parent(tips.size());
            for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
            std::function<size_t(size_t)> root = [&](size_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
            const double lim2 = sqr(scaled<double>(knot_chain_eff));
            for (size_t i = 0; i < tips.size(); ++i)
                for (size_t j = i + 1; j < tips.size(); ++j)
                    if (tips[i].run != tips[j].run && (tips[i].p - tips[j].p).cast<double>().squaredNorm() < lim2)
                        parent[root(i)] = root(j);
            std::unordered_map<size_t, std::vector<size_t>> groups;
            for (size_t i = 0; i < tips.size(); ++i) groups[root(i)].push_back(i);
            auto mark_tip = [&](const Tip& t) {
                double acc = 0.;
                const size_t first = ns_runs[t.run].first, last = ns_runs[t.run].second;
                for (size_t k = 0; k <= last - first; ++k) {
                    const size_t i = t.start ? first + k : last - k;
                    if (!extrudes(r.ordered_segments[i])) continue;
                    out.seg_flags[i] |= sfKnotRisk;
                    acc += unscale<double>(r.ordered_segments[i].length_scaled);
                    if (acc >= 0.3) break;
                }
            };
            for (const auto& [g, members] : groups) {
                size_t n_starts = 0;
                for (size_t i : members) n_starts += tips[i].start ? 1 : 0;
                if (n_starts < size_t(std::max(1, opt.knot_starts)))
                    continue;
                for (size_t i : members) { mark_tip(tips[i]); run_in_racimo[tips[i].run] = true; }
                ++m.knot_count;
            }
        }

        {
            const double lim = scaled<double>(knot_mm_eff);
            auto start_is_thin = [&](size_t first, size_t last) {
                double w0 = -1., w5 = 0., acc = 0.;
                for (size_t i = first; i <= last; ++i) {
                    const OrderedSegment& s = r.ordered_segments[i];
                    if (!extrudes(s)) continue;
                    if (w0 < 0.) w0 = double(s.path_width);
                    if (acc < 0.5) w5 = std::max(w5, double(s.path_width));
                    acc += unscale<double>(s.length_scaled);
                    if (acc >= 0.5) break;
                }
                return w0 > 0. && w0 < opt.knot_thin * w5;
            };
            auto mark = [&](size_t first, size_t last, bool from_start) {
                double acc = 0.;
                for (size_t k = 0; k <= last - first; ++k) {
                    const size_t i = from_start ? first + k : last - k;
                    if (!extrudes(r.ordered_segments[i])) continue;
                    out.seg_flags[i] |= sfKnotRisk;
                    acc += unscale<double>(r.ordered_segments[i].length_scaled);
                    if (acc >= 0.3) break;
                }
            };
            for (size_t a = 0; a < ns_runs.size(); ++a) {
                if (!start_is_thin(ns_runs[a].first, ns_runs[a].second))
                    continue;
                const Point sa = r.ordered_segments[ns_runs[a].first].from;
                std::vector<std::pair<size_t, bool>> near;   // (recorrido, es su arranque)
                for (size_t b = 0; b < ns_runs.size(); ++b) {
                    if (b == a) continue;
                    if ((r.ordered_segments[ns_runs[b].first].from - sa).cast<double>().norm() < lim)
                        near.emplace_back(b, true);
                    if ((r.ordered_segments[ns_runs[b].second].to - sa).cast<double>().norm() < lim)
                        near.emplace_back(b, false);
                }
                if (near.size() < size_t(std::max(1, opt.knot_others)))
                    continue;
                for (const auto& [b, is_start] : near)
                    mark(ns_runs[b].first, ns_runs[b].second, is_start);
                mark(ns_runs[a].first, ns_runs[a].second, true);
                if (!run_in_racimo[a])   // s339 — ya contado como racimo de arranques
                    ++m.knot_count;
            }
        }

        ns_mark("racimo");   // s342f — sonda de tiempos
        // ── s339: CONTACTO con el vecino (TEST25, los surcos y la luz a contraluz de la zona 1) ──
        // Muestras cada `contact_step_mm` sobre el eje de TODO lo que extruye (NS y muro). Para cada muestra de NS se busca,
        // a cada lado, el vecino más cercano de lado (casi perpendicular, no del mismo recorrido a menos de 1,5 mm) y su
        // holgura = distancia − (sepA + sepB)/2, con la huella de CAUDAL. Riesgo si el más apretado de un lado no se pisa
        // al menos `closure_mm`. Un lado sin nadie (borde) no cuenta: eso es la línea suelta.
        // Transliteración: `campo/t25/visor.py::contacto`.
        if (opt.compute_contact) {
            struct Smp { Vec2d p; Vec2d dir; double sep; int run; double arc; size_t seg; bool ns; };
            std::vector<Smp> smp;
            std::vector<double> run_arc(out.runs.size() + 1, 0.0);
            const double step = std::max(0.01, opt.contact_step_mm);
            for (size_t i = 0; i < r.ordered_segments.size(); ++i) {
                const OrderedSegment& s = r.ordered_segments[i];
                if (!extrudes(s) || seg_run[i] < 0) continue;
                const Vec2d a = unscale(s.from), b = unscale(s.to), d = b - a;
                const double L = d.norm();
                if (L < 1e-6) continue;
                const double h  = s.height > 0.f ? double(s.height) : 0.2;
                const double sp = footprint_sep(double(s.path_width_flow > 0.f ? s.path_width_flow : s.path_width), h);
                const int n = std::max(1, int(L / step));
                double& acc = run_arc[size_t(seg_run[i])];
                for (int k = 0; k < n; ++k) {
                    const double t = (k + 0.5) / n;
                    smp.push_back({ a + t * d, d / L, sp, seg_run[i], acc + t * L, i, s.neostroke });
                }
                acc += L;
            }
            const double reach = opt.contact_reach_mm, cell = 0.25;
            std::unordered_map<long long, std::vector<size_t>> grid;
            auto key = [](long long cx, long long cy) { return cx * 1000003LL + cy; };
            for (size_t k = 0; k < smp.size(); ++k)
                grid[key((long long)std::floor(smp[k].p.x() / cell), (long long)std::floor(smp[k].p.y() / cell))].push_back(k);
            const long long nc = (long long)std::ceil(reach / cell);
            std::vector<int> seg_bad(r.ordered_segments.size(), 0), seg_all(r.ordered_segments.size(), 0);
            for (size_t k = 0; k < smp.size(); ++k) {
                const Smp& q = smp[k];
                if (!q.ns) continue;
                const Vec2d nrm(-q.dir.y(), q.dir.x());
                double best[2] = { 1e9, 1e9 };
                const long long cx = (long long)std::floor(q.p.x() / cell), cy = (long long)std::floor(q.p.y() / cell);
                for (long long dx = -nc; dx <= nc; ++dx)
                    for (long long dy = -nc; dy <= nc; ++dy) {
                        auto it = grid.find(key(cx + dx, cy + dy));
                        if (it == grid.end()) continue;
                        for (size_t j : it->second) {
                            const Smp& o = smp[j];
                            if (o.run == q.run && std::abs(o.arc - q.arc) < 1.5) continue;
                            const Vec2d v = o.p - q.p;
                            const double dist = v.norm();
                            if (dist < 1e-6 || dist > reach) continue;
                            if (std::abs(v.dot(q.dir)) > 0.5 * dist) continue;   // sólo vecinos de lado
                            const int side = v.dot(nrm) > 0.0 ? 0 : 1;
                            best[side] = std::min(best[side], dist - 0.5 * (q.sep + o.sep));
                        }
                    }
                ++seg_all[q.seg];
                if ((best[0] < 1e8 && best[0] > -contact_margin) || (best[1] < 1e8 && best[1] > -contact_margin))
                    ++seg_bad[q.seg];
            }
            for (size_t i = 0; i < r.ordered_segments.size(); ++i)
                if (seg_all[i] > 0 && 2 * seg_bad[i] >= seg_all[i]) {
                    out.seg_flags[i] |= sfContact;
                    m.contact_mm += unscale<double>(r.ordered_segments[i].length_scaled);
                }
        }

        ns_mark("contacto");   // s342f — sonda de tiempos
        // ── riesgo de raja entre cordones anchos (TEST22 Z6) ──
        {
            Polygons wide;
            for (size_t i = 0; i < r.ordered_segments.size(); ++i) {
                const OrderedSegment& s = r.ordered_segments[i];
                if (!extrudes(s) || !s.neostroke) continue;
                const double h = s.height > 0.f ? double(s.height) : 0.2;
                if (footprint_sep(double(s.path_width), h) < opt.seam_sep_mm) continue;
                Polygons fp = segment_footprint(s);
                wide.insert(wide.end(), fp.begin(), fp.end());
            }
            if (!wide.empty()) {
                const ExPolygons wu = union_ex(wide);
                std::vector<BoundingBox> wbb;
                for (const ExPolygon& e : wu) wbb.push_back(get_extents(e));
                auto in_wide = [&](const Point& p) {
                    for (size_t i = 0; i < wu.size(); ++i)
                        if (wbb[i].contains(p) && wu[i].contains(p)) return true;
                    return false;
                };
                for (size_t i = 0; i < r.ordered_segments.size(); ++i) {
                    const OrderedSegment& s = r.ordered_segments[i];
                    if (!extrudes(s) || !s.neostroke) continue;
                    const double h   = s.height > 0.f ? double(s.height) : 0.2;
                    const double sep = footprint_sep(double(s.path_width), h);
                    if (sep < opt.seam_sep_mm) continue;
                    const Vec2d a = unscale(s.from), b = unscale(s.to), d = b - a;
                    const double len = d.norm();
                    if (len < 1e-6) continue;
                    const Vec2d  nrm(-d.y() / len, d.x() / len), mid = 0.5 * (a + b);
                    const double off = 0.5 * sep + opt.neighbour_mm;
                    if (in_wide(Point::new_scale(mid.x() + nrm.x() * off, mid.y() + nrm.y() * off)) ||
                        in_wide(Point::new_scale(mid.x() - nrm.x() * off, mid.y() - nrm.y() * off))) {
                        out.seg_flags[i] |= sfSeamRisk;
                        m.seam_risk_mm += len;
                    }
                }
            }
        }

        ns_mark("raja");   // s342f — sonda de tiempos
        // ── doble pasada (TEST22 Z3: relieve de «ironing» en la punta) ──
        // El centro de un tramo cae dentro de la huella de otro puesto ANTES en esta capa (no de sus vecinos
        // inmediatos del mismo recorrido): pasa por encima de lo que ya hay. Rejilla de 1 mm para no mirar todo.
        {
            const coord_t cell = scaled<coord_t>(1.0);
            std::unordered_map<long long, std::vector<size_t>> grid;
            std::vector<Polygons> fps(r.ordered_segments.size());
            std::vector<BoundingBox> fbb(r.ordered_segments.size());
            auto key = [](coord_t cx, coord_t cy) { return (long long)(cx) * 1000003LL + (long long)(cy); };
            // s339b — el ARRANQUE ADELANTADO pasa dos veces a propósito por los primeros mm de su propio recorrido:
            //    eso no es doble pasada. Se perdona dentro del mismo recorrido en sus primeros `kLeadForgive` mm.
            constexpr double kLeadForgive = 3.0;
            std::vector<double> arc(r.ordered_segments.size(), 0.0);
            {
                int last_run = -1; double acc = 0.0;
                for (size_t i = 0; i < r.ordered_segments.size(); ++i) {
                    if (seg_run[i] != last_run) { last_run = seg_run[i]; acc = 0.0; }
                    arc[i] = acc;
                    if (extrudes(r.ordered_segments[i])) acc += unscale<double>(r.ordered_segments[i].length_scaled);
                }
            }
            for (size_t i = 0; i < r.ordered_segments.size(); ++i) {
                const OrderedSegment& s = r.ordered_segments[i];
                if (!extrudes(s)) continue;
                const Point mid((s.from.x() + s.to.x()) / 2, (s.from.y() + s.to.y()) / 2);
                if (s.neostroke) {
                    const coord_t cx = mid.x() / cell, cy = mid.y() / cell;
                    bool hit = false;
                    for (coord_t dx = -1; dx <= 1 && !hit; ++dx)
                        for (coord_t dy = -1; dy <= 1 && !hit; ++dy) {
                            auto it = grid.find(key(cx + dx, cy + dy));
                            if (it == grid.end()) continue;
                            for (size_t j : it->second) {
                                if (j + 3 > i) continue;   // sus vecinos inmediatos no cuentan
                                if (seg_run[j] >= 0 && seg_run[j] == seg_run[i] && arc[i] < kLeadForgive) continue;
                                if (!fbb[j].contains(mid)) continue;
                                for (const Polygon& pg : fps[j])
                                    if (pg.contains(mid)) { hit = true; break; }
                                if (hit) break;
                            }
                        }
                    if (hit) {
                        out.seg_flags[i] |= sfDoublePass;
                        m.double_pass_mm += unscale<double>(s.length_scaled);
                    }
                }
                fps[i] = segment_footprint(s);
                if (fps[i].empty()) continue;
                fbb[i] = get_extents(fps[i]);
                const coord_t x0 = fbb[i].min.x() / cell, x1 = fbb[i].max.x() / cell;
                const coord_t y0 = fbb[i].min.y() / cell, y1 = fbb[i].max.y() / cell;
                for (coord_t gx = x0; gx <= x1; ++gx)
                    for (coord_t gy = y0; gy <= y1; ++gy)
                        grid[key(gx, gy)].push_back(i);
            }
        }
    }
    ns_mark("doble");   // s342f — sonda de tiempos
    // ── batiburrillo: mapa de densidad de plástico de NeoStroke ──
    // Verificado por transliteración sobre el G-code del TEST22 (capa 1.08). Tres decisiones, cada una medida:
    //  · DENSIDAD, no reparto: cada celda recibe la densidad real del tramo (mm³/mm ÷ ancho) por su área. Repartir
    //    el volumen entre «las celdas que caen» daba ~18 % de exceso en TODAS las zonas (ruido de cuadrícula).
    //  · SÓLO NeoStroke sobre NeoStroke: con Classic, el solape del 20 % contra el muro (el que cierra la junta, el
    //    mejor en TEST20) salía como exceso: 78 mm² en Z1 frente a 15 en Z8 (overlap 0). Eso es a propósito.
    //  · PROMEDIO en ~0,35 mm antes del umbral: el plástico fundido se reparte; una celda suelta no es un montón.
    //  Resultado NS-sobre-NS: Z1 4,2 mm² · Z2 (extra flow 15) 4,9 · Z9 (sin giros, sin U que se pisen) 2,8.
    if (opt.compute_excess && !r.ordered_segments.empty()) {
        const double c   = std::max(0.02, opt.excess_cell_mm);
        const BoundingBox bb = get_extents(r.input_slices);
        const double x0 = unscale<double>(bb.min.x()) - 1.0, y0 = unscale<double>(bb.min.y()) - 1.0;
        const int nx = int((unscale<double>(bb.max.x()) + 1.0 - x0) / c) + 1;
        const int ny = int((unscale<double>(bb.max.y()) + 1.0 - y0) / c) + 1;
        if (nx > 0 && ny > 0 && size_t(nx) * size_t(ny) < 30000000) {
            std::vector<float> dens(size_t(nx) * size_t(ny), 0.f);   // mm³/mm², en capas: 1 = lleno justo
            for (const OrderedSegment& s : r.ordered_segments) {
                if (!extrudes(s) || !s.neostroke) continue;
                const double h  = s.height > 0.f ? double(s.height) : 0.2;
                const Vec2d a = unscale(s.from), b = unscale(s.to), d = b - a;
                const double L = d.norm();
                if (L < 1e-6) continue;
                // 🚨 s339 — el hueco es el NOMINAL (lo que el plan le reserva), no el de caudal. Con el de caudal el
                //    plástico y el ancho crecían a la vez y rho salía SIEMPRE 1: la extrusión extra en curva no sumaba
                //    nada (TEST25: el visor daba 1,3 → 1,6 mm² de zona 1 a zona 2; la foto, una media luna levantada).
                const double rad = 0.5 * std::max(0.02, footprint_sep(double(s.path_width), h));
                const float  rho = float(s.mm3_per_mm / (2.0 * rad) / h);   // 1.0 = la capa justa
                const int ix0 = std::max(0, int((std::min(a.x(), b.x()) - rad - x0) / c));
                const int ix1 = std::min(nx - 1, int((std::max(a.x(), b.x()) + rad - x0) / c));
                const int iy0 = std::max(0, int((std::min(a.y(), b.y()) - rad - y0) / c));
                const int iy1 = std::min(ny - 1, int((std::max(a.y(), b.y()) + rad - y0) / c));
                for (int iy = iy0; iy <= iy1; ++iy)
                    for (int ix = ix0; ix <= ix1; ++ix) {
                        const Vec2d p(x0 + (ix + 0.5) * c, y0 + (iy + 0.5) * c);
                        // 🚨 Sólo el RECTÁNGULO, proyección semiabierta [0, 1): los tramos de NS miden ~0,14 mm y
                        //    con remates redondos cada vértice contaba DOS veces.
                        const double t = (p - a).dot(d) / (L * L);
                        if (t < 0.0 || t >= 1.0) continue;
                        if ((a + t * d - p).norm() <= rad) dens[size_t(iy) * size_t(nx) + size_t(ix)] += rho;
                    }
            }
            // Promedio en caja de k×k celdas (≈0,35 mm) con tabla de sumas.
            const int k = std::max(1, int(std::lround(0.35 / c)) | 1), hk = k / 2;
            std::vector<double> sum(size_t(nx + 1) * size_t(ny + 1), 0.0);
            for (int iy = 0; iy < ny; ++iy)
                for (int ix = 0; ix < nx; ++ix)
                    sum[size_t(iy + 1) * size_t(nx + 1) + size_t(ix + 1)] = dens[size_t(iy) * size_t(nx) + size_t(ix)]
                        + sum[size_t(iy) * size_t(nx + 1) + size_t(ix + 1)] + sum[size_t(iy + 1) * size_t(nx + 1) + size_t(ix)]
                        - sum[size_t(iy) * size_t(nx + 1) + size_t(ix)];
            auto box = [&](int ix, int iy) {
                const int xa = std::max(0, ix - hk), xb = std::min(nx, ix + hk + 1);
                const int ya = std::max(0, iy - hk), yb = std::min(ny, iy + hk + 1);
                const double S = sum[size_t(yb) * size_t(nx + 1) + size_t(xb)] - sum[size_t(ya) * size_t(nx + 1) + size_t(xb)]
                               - sum[size_t(yb) * size_t(nx + 1) + size_t(xa)] + sum[size_t(ya) * size_t(nx + 1) + size_t(xa)];
                return S / double(k * k);
            };
            // s339 — la MONTAÑA: la misma tabla, caja de `sustained_box_mm`, umbral bajo.
            const int k2 = std::max(1, int(std::lround(opt.sustained_box_mm / c)) | 1), hk2 = k2 / 2;
            auto box2 = [&](int ix, int iy) {
                const int xa = std::max(0, ix - hk2), xb = std::min(nx, ix + hk2 + 1);
                const int ya = std::max(0, iy - hk2), yb = std::min(ny, iy + hk2 + 1);
                const double S = sum[size_t(yb) * size_t(nx + 1) + size_t(xb)] - sum[size_t(ya) * size_t(nx + 1) + size_t(xb)]
                               - sum[size_t(yb) * size_t(nx + 1) + size_t(xa)] + sum[size_t(ya) * size_t(nx + 1) + size_t(xa)];
                return S / double(k2 * k2);
            };
            out.excess_cell_mm = c;
            for (int iy = 0; iy < ny; ++iy)
                for (int ix = 0; ix < nx; ++ix) {
                    if (!(dens[size_t(iy) * size_t(nx) + size_t(ix)] > 0.f)) continue;
                    const Point pc = Point::new_scale(x0 + (ix + 0.5) * c, y0 + (iy + 0.5) * c);
                    if (box(ix, iy) > excess_thr) {
                        out.excess_cells.emplace_back(pc);
                        m.excess_mm2 += c * c;
                    } else if (box2(ix, iy) > sustained_thr) {
                        out.sustained_cells.emplace_back(pc);
                        m.sustained_mm2 += c * c;
                    }
                }
        }
    }

    if (!opt.compute_gaps)
        return out;

    const ExPolygons domain = union_ex(r.input_slices);
    // Lo que NeoStroke cede al relleno normal. Las zonas grandes son relleno de verdad (piezas anchas); las pequeñas
    // son bolsitas que el relleno macizo no llega a imprimir (TEST23): se marcan, no se descuentan.
    ExPolygons infill;
    for (const Surface& sf : r.fill_surfaces.surfaces) infill.push_back(sf.expolygon);
    infill = union_ex(infill);
    auto add_gap = [&](const ExPolygon& ex, GapKind kind, double a) {
        Gap g; g.poly = ex; g.area_mm2 = a; g.kind = kind;
        ++m.gap_count[int(kind)];
        m.gap_mm2[int(kind)] += a;
        out.gaps.push_back(std::move(g));
    };
    for (const ExPolygon& ex : infill) {
        const double a = unscale<double>(unscale<double>(ex.area()));
        if (a >= opt.gap_min_mm2 && a <= opt.pocket_max_mm2)
            add_gap(ex, GapKind::InfillPocket, a);
    }
    // 🚨 Sin tope de tamaño para los huecos: el 0,3 mm² del script de s336 estaba para no contar la CONTRAFORMA de la
    //    letra, que en su imagen también salía negra. Aquí el dominio es la isla, que ya no la incluye. Con el tope, los
    //    huecos grandes (los que más se notan) desaparecían del mapa. Lo grande que sí es relleno se descuenta aparte.
    const ExPolygons holes  = diff_ex(diff_ex(domain, to_polygons(covered)), to_polygons(infill));
    const double     grow   = scaled<double>(0.02);   // "toca" = a menos de 20 µm, lo que el píxel del script
    for (const ExPolygon& ex : holes) {
        const double a = unscale<double>(unscale<double>(ex.area()));
        if (a < opt.gap_min_mm2)
            continue;
        if (a > opt.gap_max_mm2) {
            add_gap(ex, GapKind::Hole, a);
            continue;
        }
        const Polygons    grown = offset(ex, float(grow));
        const BoundingBox gbb   = get_extents(grown);
        const bool t_cl = touches(grown, gbb, classic);
        const bool t_ns = touches(grown, gbb, neo);
        add_gap(ex, (t_cl && t_ns) ? GapKind::Joint : (t_ns ? GapKind::NeoStrokeOnly : GapKind::ClassicOnly), a);
    }
    ns_mark("montana");
    if (NeoDebug::enabled(NeoDebug::NEOSTROKE)) {   // s342f — dónde se van las cifras del visor
        std::string s = "[NS-CIFRAS]";
        char b[64];
        for (const auto& t : ns_times) { snprintf(b, sizeof(b), " %s=%.0f", t.first, t.second); s += b; }
        s += " ms";
        NeoDebug::write(NeoDebug::NEOSTROKE, s.c_str());
    }
    return out;
}

}}} // namespace Slic3r::NeoArachne::Preview
