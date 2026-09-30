// NEOTKO_NEOSTROKE_TAG C1-C5 (s325)
// Port de `docs/TOOLS/strokes/strokes.py` (fase P, s324). El prototipo manda: cuando algo no
// cuadre, el roto es esto. Los comentarios 🚨 marcan las trampas de la §6 de la fase P, que son
// las que costaron sesiones; no tocarlas sin releerlas.
#include "NeoStroke.hpp"
#include "NeoStrokeSkeleton.hpp"
#include "NeoStrokeField.hpp"   // s338 — el planificador por campo
#include "NeoStrokeLink.hpp"
#include "NeoStrokeIslands.hpp"   // s342 — ajustes por isla
#include "NeoArachnePlan.hpp"   // set_no_spiral_lift_recursive

#include "../NeoDebug.hpp"
#include "../PerimeterGenerator.hpp"
#include "../AABBTreeLines.hpp"
#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../PrintConfig.hpp"
#include "../SurfaceCollection.hpp"
#include "../BoundingBox.hpp"
#include "../VariableWidth.hpp"
#include "../ShortestPath.hpp"
#include "../libslic3r.h"

#include <oneapi/tbb/blocked_range.h>   // s342e — islas en paralelo
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <cstdio>
#include <cstdlib>   // s330 — std::getenv de los dos mandos de sesión
#include <map>
#include <utility>
#include <numeric>
#include <queue>
#include <string>
#include <vector>

namespace Slic3r { namespace NeoArachne {

// ── el ancla de C6 ──────────────────────────────────────────────────────────
static LinkPlanner s_link_planner = nullptr;
const LinkPlanner& link_planner() { return s_link_planner; }
void set_link_planner(LinkPlanner p) { s_link_planner = std::move(p); }

// ── parámetros ──────────────────────────────────────────────────────────────
// Los valores son los que la fase P dejó buenos (§4 del pre-plan). Siguen a fuego: los mandos
// `neostroke_*` se añaden cuando esto imprima y haya algo que regular.
struct NsParams {
    double outer_w        = 0.42;   // banda que se come el muro exterior Classic
    double bead_min       = 0.25;   // mm: el cordón más fino que el cabezal saca DE VERDAD
    double nozzle         = 0.40;   // el cabezal DE VERDAD; sólo para lo físico (el patín)
    // NEOTKO_NEOSTROKE_TAG s331c — la REFERENCIA de todos los porcentajes de NeoStroke: suelo,
    // techo, cordón mínimo de un detalle, tope duro y el arranque de la rampa de caudal. Por
    // defecto es el ancho de línea del relleno macizo, que es el flujo que NeoStroke usa para
    // emitir, y con un cabezal de 0.40 y `internal_solid_infill_line_width` de 0.40 sale el mismo
    // número que antes: nada cambia hoy, y deja de estar atado al cabezal para mañana.
    // 🚨 Lo que se compara contra ella es la SEPARACIÓN del cordón, no su huella impresa: la
    // huella lleva además los h(1−π/4). O sea, 100 % = un cordón que ocupa lo que una línea de
    // relleno. Para referencia: el tope de S3D (0.525 medidos) son el 131 % de 0.40.
    double w_ref          = 0.40;
    double floor_w        = 0.05;   // suelo del ancho dentro de un camino continuo (12 % del cabezal)
    double ceiling_w      = 0.80;   // techo: es lo que decide k (200 % del cabezal)
    double sliver         = 0.05;   // un hueco que nunca llega a esto no es hueco, es grieta
    double min_len        = 0.40;
    double step           = 0.15;   // muestreo a lo largo del trazo
    int    smooth         = 3;      // ventana de suavizado del eje, en muestras
    double k_quantile     = 0.98;   // percentil del ancho para decidir k
    double join_tol       = 1.10;   // distancia máxima para coser dos extremos
    double join_turn      = 75.0;   // giro máximo admitido en una costura, en grados
    double uturn_tol      = 1.30;   // distancia máxima de un giro en U entre líneas vecinas
    double uturn_cost     = 0.30;   // penalización del giro en U frente a seguir recto
    double detail_overlap = 0.35;   // cuánto puede pisar un relleno ensanchado al cabezal
    double cont_len       = 2.00;   // 🚨 a 3.0 se caen trazos de verdad (trampa 11)
    // NEOTKO_NEOSTROKE_TAG C5b (s325) — ganchos de esquina. Dos puertas se abren a la vez:
    //   · la poda del esqueleto baja, para que las ramitas de esquina existan;
    //   · y a una línea que nace en un CRUCE se le perdona el largo mínimo, porque la costura la
    //     va a enganchar a un camino largo. El largo hay que juzgarlo sobre el camino FINAL, no
    //     sobre la línea planificada (trampa 8). Si al final se queda sola, ya la juzga la regla
    //     de caudal.
    // Medido en la H de BEACH HOUSES (s325): sus ocho ramitas de esquina tienen hueco útil de
    // hasta 0.57 mm — más que el cabezal — y se caían las ocho por medir 0.30-0.45 mm.
    bool   corner_hooks   = false;
    // NEOTKO_NEOSTROKE_TAG s331b — RAMPA DE CAUDAL. 0 = apagada. Sale de `neostroke_curve_overlap`
    // (mando de perfil, POR OBJETO) y la pueden pisar los env var. Ver `flow_ramp()`.
    // 🚨 Es de CAUDAL, no de ancho. En s331 subia el ancho, y el ancho es a la vez la huella y el
    // volumen, asi que un cordon que ya estaba en el tope se salia de el: medido en el TEST11, la
    // fila de texto daba ;WIDTH 0.793 / 0.827 / 0.866 en las columnas 0 / 7 / 15 %, o sea que el
    // propio mando se llevaba el cordon a 2.2 veces el cabezal. Ahora el ancho lo manda el plan y
    // lo limita `max_bead`, y esto solo mete mas plastico en el mismo sitio.
    double ovl_pct        = 0.0;    // fraccion: 0.15 = +15 % de caudal en el pico de la rampa
    double ovl_w1         = 1.25;   // fin de la rampa de ancho, en cabezales; empieza en 1.0
    double ovl_turn0      = 4.0;    // grados por mm donde empieza a contar como curva
    double ovl_turn1      = 15.0;   // grados por mm donde ya es curva del todo
    double ovl_span       = 1.0;    // sobre cuantos mm se mide el giro, NO muestra a muestra
    double ovl_straight   = 0.30;   // fraccion de la rampa que se aplica en RECTO (1 = toda)
    // s336h — los cinco `ovl_*` de forma de la rampa ya NO son mandos: fijos en los valores que se
    // imprimieron siempre (TEST13-TEST20). Los env var `ORCA_NS_OVL_*` siguen pudiendo pisarlos.
    // s336 — `cap_join` (s331d) RETIRADO: los giros continuos extruyen la U entera.
    bool   skate          = false;  // C6 — patinar entre caminos por encima de lo ya puesto
    double skate_detour   = 5.0;    // largo máximo del patín / salto recto (el factor de S3D)
    double hook_prune    = 0.8;    // poda del esqueleto con los ganchos encendidos (1.5 sin ellos)
    // NEOTKO_NEOSTROKE_TAG s331b — la frontera de "esto ya no es un trazo" es un ANCHO, no una
    // cuenta de lineas. Con la cuenta, bajar el techo tiraba formas enteras al residuo: el anillo
    // de 4.39 mm de hueco pedia k=11 a techo 0.400 y se iba al relleno (1.6 mm² de huecos medidos
    // en el TEST11), y a techo 0.600 pedia k=8 y salia limpio. Lo ancho que es una forma no
    // depende del techo, asi que el limite tampoco puede depender de el.
    double max_stroke_w   = 5.0;    // mm de hueco util: mas ancho que esto es una pieza, no un trazo
    double band           = 0.0;    // s340 — MODO BANDA: mm junto al muro que cubre NeoStroke; 0 = auto (todo)
    int    k_hard         = 40;     // tope de cordura de k, para que no se dispare nunca
    // NEOTKO_NEOSTROKE_TAG s332 — DESAPILAR los cortes de flujo en Z.
    // 🔑 El TEST14 cerró CUATRO frentes de golpe: cambiar la ordenación de la costura (4 posiciones
    //    × `seam_gap` desactivado), apagar el patín, `cap_join` al 100 % y subir el suelo al 85 %
    //    NO mueven el agujero de la B. Ninguno. Porque el defecto no está en cómo se planifica UNA
    //    capa: está en que todas las capas son LA MISMA.
    // 🚨 Medido en el TEST13, la B de la columna 5: las cuatro capas dan exactamente 12 arranques y
    //    11 cortes de flujo, y los cortes caen en el MISMO XY (100 % / 73 % / 82 % de coincidencia
    //    entre capas). 44 cortes en sólo 26 posiciones distintas, y SIETE posiciones se repiten en
    //    tres capas o más — las siete en los tres cruces del esqueleto de la B. Eso no es una
    //    muesca de superficie, es un CANAL, y encima el error físico de extrusión (la presión que
    //    tarda en recuperarse tras cada corte) se acumula siempre en la misma vertical.
    // 🔑 Es la misma razón por la que el relleno sólido gira el ángulo capa a capa, y por la que a
    //    Neotko el "Infill Solid con perímetro 1" le daba mejor resultado en letras difíciles: el
    //    error de una capa no se hereda.
    bool   layer_jitter   = true;   // el ancla de arranque ORBITA la isla segun el indice de capa
    int    residual_passes = 3;     // 🚨 la primera línea de un engorde deja una media luna al lado
    // NEOTKO_NEOSTROKE_TAG s329 — lo medido en el G-code del TEST08 (§11e de docs/NEOSTROKE.md).
    // 🚨 El techo decide k y NADA MÁS. Recortar además el ancho del cordón al techo rompe el
    // reparto: la POSICIÓN de las k líneas abre el hueco entero (±W/2) y el ancho se quedaba en
    // `techo`, así que por encima del percentil que eligió k quedaba una raja de W/k − techo. En
    // el TEST08 eso era hasta 0.39 mm² por barra de 3.20 x 12 y salía igual con los tres techos.
    // 🚨 s331b — el tope duro del cordon es ABSOLUTO (`neostroke_max_bead_pct`, 150 % del cabezal)
    // y ya no "1.25 veces el techo". Y SOLO puede subir k, nunca recortar el ancho de las k lineas:
    // recortarlo es exactamente la trampa de s329. Donde si recorta es en las puntas, los detalles
    // y el residuo, que no reparten nada y ahi un bulto es un bulto.
    double max_bead       = 0.60;   // mm: tope duro del ancho de CUALQUIER cordon (150 % de 0.40)
    // 🚨 El cordón más fino que la máquina sabe hacer NO es el diámetro del cabezal: una boquilla
    // de 0.4 saca 0.25 (el material, ya pegado, se estira). Con el cabezal a pelo como suelo, un
    // detalle de 0.2 mm se engordaba a 0.4, pisaba de más y se DESCARTABA: el hueco se quedaba.
    double detail_min     = 0.24;   // suelo de un detalle / de un relleno del residuo
    // Cuánto puede pisar una línea de una rama a lo ya planificado por otra rama antes de sobrar.
    double branch_overlap = 0.65;
    // NEOTKO_NEOSTROKE_TAG s336 (2_47) — tres mejoras de camino, cada una con su interruptor para
    // poder imprimir la misma placa con y sin. Las tres en false = el G-code de 2_46, byte a byte:
    // cada una entra por una rama propia y el camino viejo queda intacto.
    bool   cont_turns     = false;  // la vuelta en U se extruye como un arco, sin cortar el flujo
    // NEOTKO_NEOSTROKE_TAG s339 — TEST25 (fotos contra G-code, `docs/TOOLS/strokes/campo/t25/`). Las tres a 0 / false =
    // el G-code de hoy.
    double lane_ovl        = 0.0;   // fracción de caudal uniforme en cada carril (solape lateral fijo)
    bool   end_at_junctions = false;// en un racimo de puntas los tramos ACABAN; arrancan por el extremo libre
    double junction_mm     = 0.60;  // radio del racimo (el del visor, `knot_chain_mm`)
    // s339b (TEST26) — ARRANQUE ADELANTADO: el recorrido empieza `lead_in` mm DENTRO de su propio carril y vuelve al
    // arranque real antes de seguir. Lo flojo del arranque (presión que no ha llegado tras el viaje) cae donde el
    // cordón pasa otra vez enseguida. Sustituye al refuerzo de caudal (`start_boost`), que en el TEST26 hizo el hueco
    // de la G en las tres zonas que lo llevaban (5, 6, 7) y en ninguna de las demás.
    double lead_in         = 0.0;   // mm
    double lead_in_flow    = 0.5;   // caudal de la ida adelantada (la vuelta encima va a caudal normal)
    bool   var_k          = false;  // k a lo largo del trazo, sin bajar de `bead_min` (v2, s336b)
    // s336b — `offset_lines` RETIRADO: en plástico no se vio y en el banco eran micras.
    // s336i — `bead_overlap` (solape entre cordones anchos, s336d) RETIRADO: en el TEST21 las zonas 6 (con)
    //    y 7 (sin) salieron igual, las dos sobre-extruidas en curvas. El problema de los cordones anchos es
    //    de CURVATURA, no de solape entre vecinos.
};

enum class Kind { Outer, Stroke, Tip, Fill, Join, Skate };

struct NsLine {
    std::vector<Vec2d>  pts;
    std::vector<double> w;
    Kind                kind = Kind::Stroke;
    int                 sid  = -1;   // trazo al que pertenece
    int                 idx  = 0;    // cuál de las k líneas es (1..k), de un borde al otro
    int                 k    = 1;
    int                 e0   = 0;    // grado del cruce en cada punta (1 = punta libre, 0 = corte)
    int                 e1   = 0;
    // NEOTKO_NEOSTROKE_TAG s330 — ese extremo muere en un NODO DE TAPA (ver `plan_island`): lo
    // que se acaba ahí es el ESQUELETO, no el trazo. Para `extend_tips` cuenta como punta libre
    // aunque el grado sea 3. 🚨 Va en pareja con e0/e1 y `reverse()` tiene que girarlo también.
    bool                cap0 = false;
    bool                cap1 = false;
    // NEOTKO_NEOSTROKE_TAG s338 — sentido FIJADO: el orden de la isla no puede darle la vuelta (ver `wide_start`).
    bool                fixed_dir = false;

    double length() const
    {
        double l = 0.;
        for (size_t i = 1; i < pts.size(); ++i)
            l += (pts[i] - pts[i - 1]).norm();
        return l;
    }
    void reverse()
    {
        std::reverse(pts.begin(), pts.end());
        std::reverse(w.begin(), w.end());
        std::swap(e0, e1);
        std::swap(cap0, cap1);
    }
};

using NsLines = std::vector<NsLine>;
using NsPath  = std::vector<NsLine>;   // un camino continuo, ya ordenado

// ── utilidades ──────────────────────────────────────────────────────────────
static double poly_len(const std::vector<Vec2d>& p)
{
    double l = 0.;
    for (size_t i = 1; i < p.size(); ++i)
        l += (p[i] - p[i - 1]).norm();
    return l;
}

static std::vector<Vec2d> resample(const std::vector<Vec2d>& pts, double step)
{
    const double total = poly_len(pts);
    if (pts.size() < 2 || total <= 0.)
        return pts;
    const int n = std::max(1, int(std::lround(total / step)));
    std::vector<double> acc(pts.size(), 0.);
    for (size_t i = 1; i < pts.size(); ++i)
        acc[i] = acc[i - 1] + (pts[i] - pts[i - 1]).norm();
    std::vector<Vec2d> out;
    out.reserve(n + 1);
    size_t seg = 0;
    for (int i = 0; i <= n; ++i) {
        const double s = total * double(i) / double(n);
        while (seg + 2 < pts.size() && acc[seg + 1] < s)
            ++seg;
        const double dl = acc[seg + 1] - acc[seg];
        const double t  = dl > 0. ? std::clamp((s - acc[seg]) / dl, 0., 1.) : 0.;
        out.emplace_back(pts[seg] + (pts[seg + 1] - pts[seg]) * t);
    }
    return out;
}

static std::vector<Vec2d> smooth(const std::vector<Vec2d>& pts, int win, bool closed)
{
    if (win < 2 || pts.size() < 3)
        return pts;
    const int n = int(pts.size());
    const int h = win / 2;
    std::vector<Vec2d> out(n);
    for (int i = 0; i < n; ++i) {
        Vec2d s = Vec2d::Zero();
        int   c = 0;
        for (int d = -h; d <= h; ++d) {
            const int j = closed ? ((i + d) % n + n) % n : std::clamp(i + d, 0, n - 1);
            s += pts[j];
            ++c;
        }
        out[i] = s / double(c);
    }
    if (!closed) {   // las puntas no se mueven
        out.front() = pts.front();
        out.back()  = pts.back();
    }
    return out;
}

static std::vector<Vec2d> normals(const std::vector<Vec2d>& pts, bool closed)
{
    const int n = int(pts.size());
    std::vector<Vec2d> out(n);
    for (int i = 0; i < n; ++i) {
        const Vec2d a = closed ? pts[((i - 1) % n + n) % n] : pts[std::max(0, i - 1)];
        const Vec2d b = closed ? pts[(i + 1) % n]           : pts[std::min(n - 1, i + 1)];
        Vec2d d = b - a;
        const double l = d.norm();
        if (l > 0.)
            d /= l;
        else
            d = Vec2d(1., 0.);
        out[i] = Vec2d(-d.y(), d.x());
    }
    return out;
}

static double quantile(std::vector<double> v, double q)
{
    if (v.empty())
        return 0.;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, size_t(q * double(v.size() - 1)));
    return v[i];
}

// Tramos seguidos de `flags` en true. Como en el prototipo, un tramo de una sola muestra no vale.
static std::vector<std::pair<int, int>> runs(const std::vector<char>& flags)
{
    std::vector<std::pair<int, int>> out;
    int start = -1;
    for (int i = 0; i < int(flags.size()); ++i) {
        if (flags[i] && start < 0)
            start = i;
        else if (!flags[i] && start >= 0) {
            if (i - 1 > start)
                out.emplace_back(start, i - 1);
            start = -1;
        }
    }
    if (start >= 0 && int(flags.size()) - 1 > start)
        out.emplace_back(start, int(flags.size()) - 1);
    return out;
}

static Polyline to_polyline(const std::vector<Vec2d>& pts)
{
    Polyline pl;
    pl.points.reserve(pts.size());
    for (const Vec2d& p : pts)
        pl.points.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    return pl;
}

// Superficie que cubre una línea: la polilínea engordada con su ancho, tramo a tramo.
// 🚨 Remate REDONDO, no plano (trampa 9): con remate plano, en cada codo queda una muesca falsa
// entre un segmento y el siguiente y el mapa de huecos se llena de grietas que no existen — 63 en
// una O que no tiene ninguna. Y el relleno del residuo se pone a perseguirlas.
static Polygons line_poly(const std::vector<Vec2d>& pts, const std::vector<double>& w)
{
    Polygons out;
    for (size_t i = 1; i < pts.size(); ++i) {
        const double w0 = w[i - 1], w1 = w[i];
        if (w0 <= 0. || w1 <= 0. || (pts[i] - pts[i - 1]).norm() < 1e-9)
            continue;
        Polyline seg = to_polyline({ pts[i - 1], pts[i] });
        // 🚨 El 4º parámetro con jtRound es la TOLERANCIA DE ARCO en unidades escaladas, no un
        // límite de inglete (s326). Con 3. eran 3 nm: cada remate redondo salía con ~500 vértices,
        // miles de segmentos por isla, y TODAS las uniones de Clipper de NeoStroke cargaban con
        // eso (el marco de 47x100 mm: 2 min por capa). 5 µm deja ~12 vértices por círculo.
        append(out, offset(seg, float(scaled<double>((w0 + w1) / 4.)), ClipperLib::jtRound, scaled<double>(0.005),
                           ClipperLib::etOpenRound));
    }
    return out;
}

static Polygons line_poly(const NsLine& l) { return line_poly(l.pts, l.w); }

// 🚨 Material puesto, guardado POLÍGONO A POLÍGONO con su caja (s326). Unir con Clipper TODO lo
// puesto a cada paso escalaba fatal en una isla grande (el marco de 47x100 mm: 2 min por capa).
// Cualquier consulta es sobre una zona pequeña — un detalle, la ventana de un salto — así que se
// unen sólo los polígonos cuya caja la toca. Un polígono cuya caja no toca la zona no puede
// cambiar su intersección con ella: el resultado es EL MISMO que uniendo todo.
// 🚨 Se guarda por GRUPOS, no por polígono suelto: un agujero que entrara en la unión sin su
// contorno se daría por relleno. Un ExPolygon (contorno + agujeros) es un grupo; cada pieza de
// `line_poly` es un offset redondo de un segmento, siempre sin agujeros, y va sola.
struct CoverIndex {
    std::vector<Polygons>    groups;
    std::vector<BoundingBox> boxes;
    void add_positive(const Polygons& ps) {           // sólo polígonos SIN agujeros (line_poly)
        for (const Polygon& p : ps) {
            boxes.emplace_back(get_extents(p));
            groups.push_back(Polygons{ p });
        }
    }
    void add(const ExPolygons& exs) {
        for (const ExPolygon& ex : exs) {
            boxes.emplace_back(get_extents(ex.contour));
            groups.push_back(to_polygons(ex));
        }
    }
    bool empty() const { return groups.empty(); }
    ExPolygons near(const BoundingBox& zone) const {
        Polygons sel;
        for (size_t i = 0; i < groups.size(); ++i)
            if (boxes[i].overlap(zone))
                append(sel, groups[i]);
        return union_ex(sel);
    }
};

// Sonda de coste de dentro de plan y costura (s326), en ms; se vuelca en la línea [NS-T].
// s331 — sonda de la curva de overlap: cuantos tramos pasaron por ella, en cuantos llego a tocar
// algun cordon, y cuanto material se ha metido de mas en total. Sin esto solo podemos suponer,
// y ya hemos supuesto mal una vez.
static thread_local size_t ns_ov_runs = 0, ns_ov_touched = 0;
static thread_local size_t ns_cuts = 0;            // s336 — cortes de flujo DENTRO de un camino
static thread_local double ns_ov_vol_in = 0., ns_ov_vol_out = 0.;
static thread_local double ns_t_skel = 0., ns_t_strokes = 0., ns_t_tips = 0., ns_t_residual = 0.,
                           ns_t_details = 0., ns_t_stitch_only = 0., ns_t_flow = 0.;
static double ns_since(std::chrono::steady_clock::time_point a)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

static double area_mm2(const ExPolygons& e)
{
    double a = 0.;
    for (const ExPolygon& ex : e)
        a += ex.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

// Distancia al borde de una zona, en mm. Es el `boundary.distance(Point)` del prototipo.
struct Boundary {
    AABBTreeLines::LinesDistancer<Linef> d;
    explicit Boundary(const ExPolygon& ex) : d(to_unscaled_linesf(ExPolygons{ ex })) {}
    double operator()(const Vec2d& p) const { return std::abs(d.distance_from_lines<false>(p)); }
};

// ¿El segmento a-b va entero por dentro de `region`?
static bool inside(const ExPolygons& region, const Vec2d& a, const Vec2d& b)
{
    if (region.empty())
        return false;
    return diff_pl(Polylines{ to_polyline({ a, b }) }, region).empty();
}

// ── C2 + C3: las k líneas de un trazo, y sus puntas ─────────────────────────
struct StrokeStats {
    size_t strokes = 0, slivers = 0, fills = 0, too_wide = 0, dropped_detail = 0;
    size_t repainted = 0;   // s329 — líneas de una rama que caían encima de otra rama
    size_t caps      = 0;   // s330 — extremos de trazo alargados por ser un nodo de TAPA
    size_t thin = 0, thin_of = 0;   // s336 — muestras de trazo con ancho < bead_min, de cuántas
    std::map<int, size_t> k_hist;
};

// NEOTKO_NEOSTROKE_TAG s330 — las dos mitades se pueden apagar por separado sin recompilar, para
// poder laminar el MISMO 3mf de las cuatro maneras y comparar. Presencia = apagado, como los
// canales de NeoDebug. `ORCA_NS_NO_TAPA` quita el remate de tapa, `ORCA_NS_NO_MURO` vuelve a
// contar el muro dentro del solape del filtro de detalle.
static bool ns_cap_ends()
{
    static const bool off = (std::getenv("ORCA_NS_NO_TAPA") != nullptr);
    return !off;
}
static bool ns_wall_apart()
{
    static const bool off = (std::getenv("ORCA_NS_NO_MURO") != nullptr);
    return !off;
}
// Lectura de una env var numérica (la usa `ns_conv_tol`). El suavizado de caudal de s330 que
// colgaba de aquí (`ORCA_NS_SUAVE*`) se retiró en s331.
static double ns_env_num(const char* k, double def)
{
    const char* v = std::getenv(k);
    if (v == nullptr || *v == '\0')
        return def;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    return (end == v || !std::isfinite(d)) ? def : d;
}
// 🚨 Los que mandan son los mandos de perfil `neostroke_curve_overlap` y `neostroke_overlap_*`,
// que van POR OBJETO y por eso dejan meter varios valores en la misma placa. Los env var solo
// PISAN, y solo el que este puesto; puestos, se aplican a todo por igual y se cargan la placa de
// comparacion. 🚨 Se resuelven UNA vez por proceso: cambiarlos con Orca abierto no hace nada.
static bool ns_env_get(const char* k, double lo, double hi, double& out)
{
    const char* v = std::getenv(k);
    if (v == nullptr || *v == '\0')
        return false;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end == v || !std::isfinite(d))
        return false;
    out = std::max(lo, std::min(hi, d));
    return true;
}
struct NsOvlEnv {
    bool   has_pct = false, has_w1 = false, has_g0 = false, has_g1 = false, has_span = false,
           has_str = false, has_bead = false, has_sw = false, has_wref = false,
           has_turns = false, has_vark = false;   // s336
    double pct = 0., w1 = 1.25, g0 = 4., g1 = 15., span = 1., str = 1., bead = 0.60, sw = 5.0,
           wref = 0.40, turns = 0., vark = 0.;
    NsOvlEnv()
    {
        has_pct  = ns_env_get("ORCA_NS_OVL",      0.0,  50.0, pct);
        has_w1   = ns_env_get("ORCA_NS_OVL_W1",   1.01,  4.0, w1);
        has_g0   = ns_env_get("ORCA_NS_OVL_G0",   0.0,  180., g0);
        has_g1   = ns_env_get("ORCA_NS_OVL_G1",   0.1,  360., g1);
        has_span = ns_env_get("ORCA_NS_OVL_SPAN", 0.2,   10., span);
        has_str  = ns_env_get("ORCA_NS_OVL_STR",  0.0,   1.0, str);    // fracción en recto
        has_bead = ns_env_get("ORCA_NS_BEAD",     0.05,  2.0, bead);   // mm, tope duro del cordón
        has_sw   = ns_env_get("ORCA_NS_STROKE_W", 0.5,  30.0, sw);     // mm, ancho máximo de trazo
        has_wref = ns_env_get("ORCA_NS_WREF",     0.05,  5.0, wref);   // mm, referencia del 100 %
        // s336 — 0/1, pisan la casilla del perfil para un A/B sin tocar el 3mf
        has_turns = ns_env_get("ORCA_NS_CONT_TURNS",   0.0, 1.0, turns);
        has_vark  = ns_env_get("ORCA_NS_VAR_K",        0.0, 1.0, vark);
    }
};
static const NsOvlEnv& ns_ovl_env() { static const NsOvlEnv e; return e; }
// NEOTKO_NEOSTROKE_TAG s330 — la tolerancia con la que `thick_polyline_to_multi_path` trocea un
// tramo de ancho variable. Importa más de lo que parece: ese conversor emite cada trozo con
// `fmax(a_width, b_width)`, el MÁXIMO de sus dos anchos, nunca la media, así que siempre echa de
// más, y de más cuanto más varíe el ancho. Medido en el TEST10, una fila de texto: la variación
// total del perfil cae de 8.79 a 2.36 mm con el suavizado y el material emitido cae 0.409 mm²,
// contra los 0.441 que predice ese exceso. O sea que SIN suavizar se estaba echando un 2.2 % de
// más, y eso tapaba huecos y falseaba la comparación entre columnas.
// Barrido en el prototipo, exceso / puntos del camino, tolerancia 0.05 → 0.02 → 0.01:
//     W de 1.7   1.44 % / 992   →  0.93 % / 1243  →  0.51 % / 1866
//     ese        1.27 % / 108   →  0.59 % /   83  →  0.33 % /  123
//     barra 3.20 0.17 % / 574   →  0.07 % /  658  →  0.04 % /  784
// 🚨 No baja a cero por fino que se hile: los tramos que ya tienen Δw menor que la tolerancia no
// se parten nunca y su exceso se queda. Para eso habría que cambiar el `fmax` por la media, y eso
// vive en `VariableWidth.cpp`, que lo comparte el gap-fill de Orca.
// 0.02 es el punto elegido: se lleva un tercio del exceso por un 25 % más de puntos. A 0.01 los
// segmentos de una letra bajan a ~0.05 mm, que a 60 mm/s son 1250 órdenes por segundo y ahí ya se
// entra en el terreno donde la placa se atraganta.
static double ns_conv_tol() { static const double t = std::max(0.002, std::min(0.10, ns_env_num("ORCA_NS_TOL", 0.02))); return t; }

// ── s336 (2_47): carriles — k variable v2 (s336b) ─────────────────────────────
// NEOTKO_NEOSTROKE_TAG s336b — la rama de `plan_stroke` con `var_k`, en trazos ABIERTOS. Con el mando
// apagado, o en un ciclo, no se llama y sale el bucle de siempre.
//
// Hasta 2_46 `k` se decidía una vez por rama (percentil 98) y donde el trazo se estrechaba `W/k` bajaba
// a pelos de 0.07 que la boquilla no saca (s332: el 42 %). Aquí hay `k` CARRILES de borde a borde y
// cada muestra decide cuántos lleva encendidos.
//
// 🔑 Lo que enseñó el TEST18 impreso (v1), y por qué la v2 es así:
//   1. La v1 CORTABA el carril en cuanto su ancho bajaba de `bead_min`, con peso aún por repartir.
//      Los vecinos saltaban a ocupar el hueco y el remate redondo del carril cortado dejaba una MUESCA:
//      las filas de puntitos a lo largo de las formas largas (zonas F, G, H). Ahora el carril que se
//      apaga NO se corta: sigue hasta que su peso llega a cero, afinándose en cuña mientras sus
//      vecinos lo absorben. La suma de anchos es `W` en cada muestra y las posiciones son continuas,
//      así que no queda hueco en ningún punto de la transición.
//   2. La v1 cambiaba `k` cada vez que el ancho cruzaba un múltiplo del TECHO. Ahora sólo se quitan
//      carriles cuando hace FALTA: la `k` del trazo se mantiene mientras `W/k ≥ 0.9·bead_min`, y la
//      histéresis es proporcional a la forma (3 veces el ancho del trazo, mínimo 1.5 mm).
//   3. Los ANILLOS (ciclos) salían en churro: el ancho cambia a lo largo de toda la vuelta y cada
//      cambio abría o cerraba un carril. Un ciclo va con la `k` de siempre (ver la llamada).
//   4. La E de BEACH: con la regla «el menor número de cordones que respete `bead_min`», un palo de
//      0.55 salía con UN cordón gordo que arrancaba y paraba dentro del palo, y en plástico abría hueco.
//      A (2_46) ponía dos de 0.27 y salía bien. El 0.9 de arriba es justo eso: dos cordones de al menos
//      ~0.25, lo que una 0.4 saca de verdad (s332), antes que uno gordo.
// ⚠️ Cuando un carril del centro se apaga, sus vecinos dejan de ser `idx±1` y esa vuelta en U se
//    pierde; queda la costura normal. Pendiente.
static void plan_lanes(const std::vector<Vec2d>& pts, const std::vector<double>& W,
                       const std::vector<Vec2d>& nrm, const std::vector<double>& s,
                       const std::vector<std::pair<int, int>>& tramos, int k,
                       int sid, const size_t deg[2], bool cap_front, bool cap_back,
                       const NsParams& P, StrokeStats& st, NsLines& out)
{
    (void)st;
    const int    last  = int(pts.size()) - 1;
    const double b_min = 0.9 * P.bead_min;   // ver el punto 4 de arriba
    // 🚨 La histéresis se mide con el ancho TÍPICO (la mediana), no con el máximo: en una cuña el
    //    máximo es el extremo gordo, y 3 veces eso se tragaba media cuña (banco s336b: 3 líneas y 54 %).
    const double wmed = quantile(W, 0.5);
    const double hyst = std::max(1.5, 3. * wmed);          // punto 2: proporcional a la forma
    const double ramp = std::max(2. * P.ceiling_w, 1.0);   // largo de la cuña de un carril que se apaga
    // El orden en que se apagan los carriles: del centro hacia fuera; en empate, el de índice menor.
    std::vector<int> drop(k);
    std::iota(drop.begin(), drop.end(), 0);
    std::stable_sort(drop.begin(), drop.end(), [k](int a, int b) {
        return std::abs(2 * a - (k - 1)) < std::abs(2 * b - (k - 1));
    });
    for (const auto& r : tramos) {
        std::vector<int> seg;
        for (int i = r.first; i <= r.second; ++i)
            seg.push_back(i);
        const int m  = int(seg.size());
        const int e0 = r.first  == 0    ? int(deg[0]) : 0;
        const int e1 = r.second == last ? int(deg[1]) : 0;

        // Cuántos carriles hacen FALTA en cada muestra: los `k` del trazo mientras quepan.
        std::vector<int> n(m, k);
        for (int a = 0; a < m; ++a) {
            const double w = W[seg[a]];
            if (w / double(k) < b_min)
                n[a] = std::clamp(int(std::floor(w / b_min + 1e-9)), 1, k);
        }
        // Histéresis: un tramo de `n` constante más corto que `hyst` se funde con el vecino de MENOS
        // carriles (más ancho, nunca más fino).
        for (int pass = 0; pass < 8; ++pass) {
            bool changed = false;
            int a = 0;
            while (a < m) {
                int b = a;
                while (b + 1 < m && n[b + 1] == n[a])
                    ++b;
                const double len = s[seg[b]] - s[seg[a]];
                const bool has_l = a > 0, has_r = b + 1 < m;
                if (len < hyst && (has_l || has_r)) {
                    int to = has_l && has_r ? std::min(n[a - 1], n[b + 1]) : (has_l ? n[a - 1] : n[b + 1]);
                    to = std::min(to, n[a]);
                    // 🚨 Fundir nunca puede pedir un cordón más gordo que el tope duro: se recortaría y
                    //    abriría huecos a lo largo del tramo. Si no cabe, el tramo se queda como está.
                    double wrun = 0.;
                    for (int c = a; c <= b; ++c)
                        wrun = std::max(wrun, W[seg[c]]);
                    if (wrun / double(to) > P.max_bead)
                        to = n[a];
                    if (to != n[a]) {
                        for (int c = a; c <= b; ++c)
                            n[c] = to;
                        changed = true;
                    }
                }
                a = b + 1;
            }
            if (!changed)
                break;
        }
        // Peso de cada carril: 1 encendido, 0 apagado, y rampa de `ramp` mm de arco entre los dos
        // (media móvil del escalón por longitud de arco = rampa lineal).
        std::vector<std::vector<double>> wt(k, std::vector<double>(m, 1.));
        {
            const double half = 0.5 * ramp;
            for (int lane = 0; lane < k; ++lane) {
                const int rank = int(std::find(drop.begin(), drop.end(), lane) - drop.begin());
                std::vector<double> step(m);
                for (int a = 0; a < m; ++a)
                    step[a] = rank < k - n[a] ? 0. : 1.;
                int lo = 0, hi = 0;
                for (int a = 0; a < m; ++a) {
                    while (lo < a && s[seg[a]] - s[seg[lo]] > half)
                        ++lo;
                    if (hi < a)
                        hi = a;
                    while (hi + 1 < m && s[seg[hi + 1]] - s[seg[a]] <= half)
                        ++hi;
                    double acc = 0.;
                    for (int c = lo; c <= hi; ++c)
                        acc += step[c];
                    wt[lane][a] = acc / double(hi - lo + 1);
                }
            }
        }
        // Los carriles. Un carril sólo se parte donde su peso es CERO (punto 1).
        for (int lane = 0; lane < k; ++lane) {
            NsLine L;
            bool   open_front = true;
            auto flush = [&](bool at_end) {
                if (L.pts.size() >= 2) {
                    L.kind = Kind::Stroke; L.sid = sid; L.idx = lane + 1; L.k = k;
                    L.e0 = open_front ? e0 : 0;
                    L.e1 = at_end ? e1 : 0;
                    L.cap0 = open_front && cap_front && r.first == 0;
                    L.cap1 = at_end && cap_back && r.second == last;
                    const double min_len = (P.corner_hooks && (L.e0 >= 3 || L.e1 >= 3)) ? P.step : P.min_len;
                    if (L.length() >= min_len)
                        out.push_back(std::move(L));
                }
                L = NsLine{};
                open_front = false;
            };
            for (int a = 0; a < m; ++a) {
                const int j = seg[a];
                double sum = 0., before = 0.;
                for (int c = 0; c < k; ++c) {
                    sum += wt[c][a];
                    if (c < lane)
                        before += wt[c][a];
                }
                const double my = wt[lane][a];
                if (my <= 1e-9 || sum <= 1e-12) {
                    flush(false);
                    continue;
                }
                const double f = 2. * (before + 0.5 * my) / sum - 1.;   // de -1 (un borde) a +1
                const double w = W[j] * my / sum;
                L.pts.push_back(pts[j] + nrm[j] * (f * W[j] / 2.));
                L.w.push_back(std::min(P.max_bead, std::max(P.floor_w, w)));
            }
            flush(true);
        }
    }
}

static NsLines plan_stroke(const StrokeBranch& br, const Boundary& bnd, const NsParams& P,
                           StrokeStats& st, bool cap_front = false, bool cap_back = false)
{
    const bool closed = br.closed;
    std::vector<Vec2d> pts = resample(br.pts, P.step);
    if (closed && pts.size() > 3)
        pts.pop_back();
    pts = smooth(pts, P.smooth, closed);
    if (pts.size() < 2)
        return {};

    // Ancho útil local: lo que queda del hueco tras los dos muros exteriores.
    std::vector<double> W(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        W[i] = std::max(0., 2. * (bnd(pts[i]) - P.outer_w));
    const std::vector<Vec2d> nrm = normals(pts, closed);

    // Arco acumulado: el ensanchamiento del cruce no decide k. Las muestras que caen dentro del
    // radio del cruce se quedan fuera del percentil.
    std::vector<double> s(pts.size(), 0.);
    for (size_t i = 1; i < pts.size(); ++i)
        s[i] = s[i - 1] + (pts[i] - pts[i - 1]).norm();
    const double total = s.back();
    std::vector<char> mask(pts.size(), 1);
    // 🚨 Una RAMITA (hoja → cruce) vive ENTERA dentro de la sombra del cruce: enmascararla deja
    // sólo su punta, que es justo la parte donde no hay hueco, y el trazo entero se juzga grieta y
    // se tira. Es la tercera puerta que mataba los ganchos de esquina, y la más escondida: las
    // otras dos (la poda y el largo mínimo) se ven, ésta no.
    const bool spur = (br.deg_front == 1 && br.deg_back >= 3) || (br.deg_back == 1 && br.deg_front >= 3);
    if (!closed && !(P.corner_hooks && spur)) {
        const std::pair<int, size_t> ends[2] = { { 0, br.deg_front }, { -1, br.deg_back } };
        for (const auto& e : ends) {
            if (e.second < 3)
                continue;
            const Vec2d& q = (e.first == 0) ? pts.front() : pts.back();
            const double rr = bnd(q);
            for (size_t i = 0; i < pts.size(); ++i) {
                const double d = (e.first == 0) ? s[i] : total - s[i];
                if (d < rr)
                    mask[i] = 0;
            }
        }
    }
    std::vector<double> core;
    for (size_t i = 0; i < pts.size(); ++i)
        if (mask[i])
            core.push_back(W[i]);
    if (core.empty())
        core = W;
    const double wmax = quantile(core, P.k_quantile);
    if (wmax < P.sliver) {
        ++st.slivers;
        return {};
    }
    // NeoStroke es un motor de LETRAS. Un trazo más ancho que `max_stroke_w` es una zona ancha de
    // una pieza cualquiera: ahí no se planifica nada y se la queda el relleno de siempre. Es la
    // frontera del motor, y se dice en voz alta en vez de degradarse en silencio.
    if (wmax > P.max_stroke_w) {
        ++st.too_wide;
        return {};
    }
    // 🚨 s331b — el techo decide k, y el tope duro del cordón SÓLO PUEDE SUBIRLO. Recortar además
    // el ancho de las k líneas es la trampa de s329: la posición abre el hueco entero y el ancho
    // se queda corto, así que por encima del percentil queda una raja de W/k − tope.
    const double wcore = *std::max_element(core.begin(), core.end());
    int k = std::max(1, int(std::ceil(wmax / P.ceiling_w - 1e-9)));
    k     = std::max(k, int(std::ceil(wcore / P.max_bead - 1e-9)));
    k     = std::max(1, std::min(P.k_hard, k));
    ++st.k_hist[k];
    ++st.strokes;

    NsLines out;
    const int sid  = int(st.strokes);
    const int last = int(pts.size()) - 1;
    const size_t deg[2] = { br.deg_front, br.deg_back };

    // El tramo donde caben las k líneas: el hueco da al menos k · suelo.
    std::vector<char> ok(pts.size());
    bool all_ok = true;
    for (size_t i = 0; i < pts.size(); ++i) {
        // s336 — con `var_k` el número de líneas baja con el hueco, así que el tramo vale donde
        // quepa UN cordón real; lo demás lo deciden los carriles de abajo.
        ok[i] = (P.var_k && !closed) ? char(W[i] >= 0.9 * P.bead_min) : char(W[i] >= k * P.floor_w);
        all_ok = all_ok && ok[i];
    }
    // 🚨 Un ciclo TAMBIÉN se parte donde no hay sitio. El prototipo daba el ciclo entero por
    // bueno («un ciclo no tiene puntas»), y con letras nunca se notó; en un anillo excéntrico el
    // lado fino no tiene hueco ninguno — los dos muros ya se solapan ahí — y seguir el ciclo
    // significa extruir un pelo encima de material puesto. Donde el hueco no da ni para el
    // suelo, no se imprime; la costura vuelve a unir los arcos si se tocan.
    const std::vector<std::pair<int,int>> tramos = runs(ok);
    const bool cycle = closed && all_ok;
    // s336b — los ciclos (anillos) van SIEMPRE con la k de siempre: ver el punto 3 de `plan_lanes`.
    if (P.var_k && !closed) {
        plan_lanes(pts, W, nrm, s, tramos, k, sid, deg, cap_front, cap_back, P, st, out);
    } else
    for (const auto& r : tramos) {
        std::vector<int> seg;
        for (int i = r.first; i <= r.second; ++i)
            seg.push_back(i);
        if (cycle)
            seg.push_back(r.first);
        const int e0 = cycle ? 0 : (r.first  == 0    ? int(deg[0]) : 0);
        const int e1 = cycle ? 0 : (r.second == last ? int(deg[1]) : 0);
        const int kr = k;
        for (int i = 1; i <= kr; ++i) {
            const double f = double(2 * i - 1) / double(kr) - 1.;   // de -1 (un borde) a +1 (el otro)
            NsLine L;
            L.kind = Kind::Stroke; L.sid = sid; L.idx = i; L.k = kr; L.e0 = e0; L.e1 = e1;
            L.cap0 = !cycle && cap_front && r.first  == 0;
            L.cap1 = !cycle && cap_back  && r.second == last;
            for (int j : seg) {
                const double half = W[j] / 2.;
                L.pts.emplace_back(pts[j] + nrm[j] * (f * half));
                // 🚨 s329 — el ancho reparte el MISMO W[j] que la posición. Ver `max_bead`.
                L.w.push_back(std::min(P.max_bead, std::max(P.floor_w, W[j] / double(k))));
            }
            const double min_len = (P.corner_hooks && (e0 >= 3 || e1 >= 3)) ? P.step : P.min_len;
            if (L.length() >= min_len)
                out.push_back(std::move(L));
        }
    }
    for (const NsLine& L : out)            // s336 — la sonda `finos=`: sólo mira, no cambia nada
        for (double w : L.w) {
            ++st.thin_of;
            if (w < P.bead_min - 1e-9)
                ++st.thin;
        }
    if (cycle)
        return out;

    // C3 — puntas: donde las k no caben sigue UNA línea por el eje (la espina de la v3).
    std::vector<char> tips(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        tips[i] = char(!ok[i] && W[i] >= P.sliver);
    for (const auto& r : runs(tips)) {
        std::vector<int> idx;
        if (r.first > 0)
            idx.push_back(r.first - 1);     // una muestra de solape con el tramo de k líneas
        for (int i = r.first; i <= r.second; ++i)
            idx.push_back(i);
        if (r.second < last)
            idx.push_back(r.second + 1);
        NsLine L;
        L.kind = Kind::Tip; L.sid = sid; L.idx = (k + 1) / 2; L.k = k;
        L.e0 = (r.first  == 0    ? int(deg[0]) : 0);
        L.e1 = (r.second == last ? int(deg[1]) : 0);
        L.cap0 = cap_front && r.first  == 0;
        L.cap1 = cap_back  && r.second == last;
        for (int j : idx) {
            L.pts.push_back(pts[j]);
            L.w.push_back(std::min(P.max_bead, std::max(P.floor_w, W[j])));
        }
        const double min_len = (P.corner_hooks && (L.e0 >= 3 || L.e1 >= 3)) ? P.step : P.min_len;
        if (L.length() >= min_len)
            out.push_back(std::move(L));
    }
    return out;
}

// C3 — alargar hasta el borde útil las líneas que mueren en una punta libre. El esqueleto acaba
// un radio antes del final real del trazo (la poda se comió las ramitas de esquina), así que sin
// esto la punta de cada letra se queda sin rellenar.
//
// NEOTKO_NEOSTROKE_TAG s329 — la RAMPA de la punta (idea de Neotko, dibujo de s329). Tres cosas
// que estaban mal y que se pagaban justo donde más se ve, en el remate:
//   🚨 el relleno iba a ancho CONSTANTE (`L.w.insert(..., grown.size(), w)`) hacia una zona que se
//      estrecha: eso es sobre-extruir en todas las puntas, y es parte del pegote del extremo gordo
//      de la cuña del TEST08. Ahora el ancho SIGUE al hueco local, `2·(bnd − muro)`, y sólo puede
//      bajar: es la rampa del dibujo, con el suelo abajo en vez de cero (un cordón de 0.02 no
//      existe: `mm3_per_mm` sale del ancho).
//   🚨 el tope eran 60 pasos de 0.15 = 9 mm, que no es una punta, es medio trazo. Ahora el tope lo
//      pone el radio de la punta: la poda se come ~un radio, así que con 2.5 sobra.
//   🚨 el criterio de parada miraba UN punto (`nxt + d·w/2`) sin el ancho del cordón. Con el ancho
//      siguiendo al hueco el lateral queda cubierto por construcción, y el remate redondo se
//      comprueba delante.
static void extend_tips(NsLines& lines, const ExPolygons& R, const NsParams& P,
                        const Boundary& bnd, double outer_w, size_t* st_caps = nullptr)
{
    if (R.empty())
        return;
    auto contains = [&R](const Vec2d& p) {
        const Point q(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
        for (const ExPolygon& ex : R)
            if (ex.contains(q))
                return true;
        return false;
    };
    for (NsLine& L : lines) {
        for (int at_end = 0; at_end < 2; ++at_end) {
            // s330 — o es una punta libre de verdad (grado 1), o es un NODO DE TAPA: el
            // tronco de un remate plano muere en un cruce de grado 3 contra las dos ramitas de
            // esquina, y por eso se paraba en seco un radio antes del borde.
            const bool free_tip = (at_end ? L.e1   : L.e0  ) == 1;
            const bool cap_tip  = (at_end ? L.cap1 : L.cap0) && ns_cap_ends();
            if (!(free_tip || cap_tip) || L.pts.size() < 2)
                continue;
            const Vec2d a = at_end ? L.pts[L.pts.size() - 2] : L.pts[1];
            const Vec2d b = at_end ? L.pts.back()            : L.pts.front();
            Vec2d d = b - a;
            const double n = d.norm();
            if (n < 1e-9)
                continue;
            d /= n;
            const double w0    = at_end ? L.w.back() : L.w.front();
            const int    steps = std::max(2, std::min(60, int(std::ceil(2.5 * bnd(b) / P.step))));
            std::vector<Vec2d>  grown;
            std::vector<double> grown_w;
            Vec2d  cur = b;
            double w   = w0;
            for (int i = 0; i < steps; ++i) {
                const Vec2d  nxt = cur + d * P.step;
                const double gap = 2. * (bnd(nxt) - outer_w);
                if (gap < P.floor_w - 1e-9)
                    break;                                  // ahí ya no cabe ni el suelo
                const double wn = std::min(w, gap);         // el ancho sólo puede BAJAR
                if (!contains(nxt + d * (wn / 2.)))
                    break;                                  // el remate redondo se saldría
                grown.push_back(nxt);
                grown_w.push_back(wn);
                cur = nxt;
                w   = wn;
            }
            if (grown.empty())
                continue;
            if (cap_tip && !free_tip && st_caps != nullptr)
                ++*st_caps;
            if (at_end) {
                L.pts.insert(L.pts.end(), grown.begin(), grown.end());
                L.w.insert(L.w.end(), grown_w.begin(), grown_w.end());
            } else {
                std::reverse(grown.begin(), grown.end());
                std::reverse(grown_w.begin(), grown_w.end());
                L.pts.insert(L.pts.begin(), grown.begin(), grown.end());
                L.w.insert(L.w.begin(), grown_w.begin(), grown_w.end());
            }
        }
    }
}

// ── C5: el residuo ──────────────────────────────────────────────────────────
// Lo que los trazos no cubren se planifica OTRA VEZ con el mismo motor, pero sin muro. Son las
// cuñas de las serifas y los engordes donde dos trazos se funden. Una cuña sale con k=1 y un
// engorde con k=2 o 3, que es justo lo que hace falta: una línea sola en un engorde deja el borde
// sin tocar.
// 🚨 `covered` ENTRA YA HECHO y el llamador lo va acumulando. Rehacerlo aquí es unir un polígono
// por cada segmento de cada línea (una isla de 47 x 100 mm son decenas de miles) y hacerlo tres
// veces, una por vuelta del residuo: era parte del laminado atascado al 15 % (s325).
// 🚨 Y tiene que incluir lo que tapa el MURO EXTERIOR. El prototipo planifica él mismo el muro, así
// que la resta le salía sola; aquí lo pone Classic y hay que traerlo a mano, o el residuo ve la
// banda del muro como un hueco y la vuelve a rellenar: doble pasada de perímetro.
static NsLines fill_residual(const ExPolygon& island, const ExPolygons& covered,
                             const NsParams& P, const SkeletonParams& skp, StrokeStats& st)
{
    const ExPolygons rest = diff_ex(ExPolygons{ island }, covered);
    if (rest.empty())
        return {};

    NsParams inner = P;
    inner.outer_w = 0.;
    inner.min_len = std::min(P.min_len, 0.25);
    inner.smooth  = 2;
    SkeletonParams inner_skp = skp;
    inner_skp.prune_mm = std::min(skp.prune_mm, 0.8);
    inner_skp.ds_mm    = std::min(0.04, skp.ds_mm);

    // Grieta: ni el relleno la llenaría. 🚨 Un solo offset para TODO el resto, no uno por pieza
    // (s326): en el marco de 47x100 mm quedan miles de astillas entre trazos. Encoger piezas
    // separadas es encoger cada una, así que una pieza sobrevive si algún trozo encogido cae dentro
    // de ella: el mismo filtro que antes.
    const ExPolygons eroded = offset_ex(rest, -float(scaled<double>(P.sliver / 2.)));
    std::vector<BoundingBox> eroded_bb;
    eroded_bb.reserve(eroded.size());
    for (const ExPolygon& e : eroded)
        eroded_bb.emplace_back(get_extents(e.contour));
    const auto survives = [&](const ExPolygon& g) {
        const BoundingBox gb = get_extents(g.contour);
        for (size_t k = 0; k < eroded.size(); ++k)
            if (gb.contains(eroded_bb[k].min) && gb.contains(eroded_bb[k].max)
                && g.contains(eroded[k].contour.points.front()))
                return true;
        return false;
    };

    NsLines out;
    for (const ExPolygon& g : rest) {
        if (g.area() * SCALING_FACTOR * SCALING_FACTOR < 0.02)
            continue;
        if (!survives(g))
            continue;
        const StrokeSkeleton sk(g, inner_skp);
        const Boundary bnd(g);
        NsLines got;
        StrokeStats sub;
        for (const StrokeBranch& br : sk.branches()) {
            if (br.length_mm < inner.min_len)
                continue;
            NsLines l = plan_stroke(br, bnd, inner, sub);
            got.insert(got.end(), std::make_move_iterator(l.begin()), std::make_move_iterator(l.end()));
        }
        if (got.empty()) {   // engorde sin ramas útiles: una línea por el eje y basta
            const std::vector<Vec2d> axis = sk.longest_path();
            if (axis.size() >= 2) {
                NsLine L;
                L.kind = Kind::Fill; L.sid = -1; L.idx = 1; L.k = 1; L.e0 = 1; L.e1 = 1;
                L.pts = smooth(resample(axis, P.step), 2, false);
                for (const Vec2d& q : L.pts)
                    L.w.push_back(std::min(P.max_bead, std::max(P.floor_w, 2. * bnd(q))));
                got.push_back(std::move(L));
            }
        }
        for (NsLine& L : got) {
            L.kind = Kind::Fill;
            L.sid  = -1;
        }
        extend_tips(got, ExPolygons{ g }, P, bnd, 0.);   // el residuo va sin muro
        for (NsLine& L : got)
            if (L.length() >= std::min(P.min_len, 0.25)) {
                out.push_back(std::move(L));
                ++st.fills;
            }
    }
    return out;
}

// ── el motor de una isla ────────────────────────────────────────────────────
// El muro exterior NO sale de aquí: lo pone Classic, igual que en la v3. Aquí sólo el interior.
// ── s338: CIERRE DE HUECOS, mirando TODOS a la vez ─────────────────────────────
// NEOTKO_NEOSTROKE_TAG s338 — el hueco "porque justo no me queda espacio" (idea de Neotko).
// Cada trazo decide su k y su ancho mirando SÓLO su pasillo; lo que queda entre dos pasillos (una
// junta de palo y panza, una esquina interior) no es de nadie y el residuo lo juzga trozo a trozo
// con un umbral. Un trozo de 0.22 mm no llega a detalle y se queda: el hueco aparece o no según
// la geometría cruce el umbral por décimas (la P de PLAGE en el TEST23: 0.26 en tres capas, 0.21
// en otra); la sonda decía «CABE el cordon=0.000» con 0.1 mm² de hueco en la P porque su umbral es un cordón
// entero. 🚨 (s338, corregido en el TEST23d) `line_poly(w)` SÍ es la huella real: `w` es la separación y el
// conversor suma h(1−π/4) él solo. Al principio se restaba ese margen aquí y se veían rendijas falsas.
// Esta pasada va AL FINAL del plan: saca los huecos con la huella real, y cada muestra de línea
// que mira a un hueco por su normal se ensancha hacia él (y su eje se desplaza hacia ese lado),
// con el tope `max_bead`. Si al otro lado del hueco hay otra línea, cada una se lleva la mitad.
// Prototipo en Python sobre el G-code del TEST23: columna hotel 261 huecos / 5.48 mm² → 54 /
// 1.07 mm² con +1.4 % de material; Gyp Sea 14 / 0.198 → 3 / 0.056 con +0.4 %.
// Va OCULTO como parte del motor. `ORCA_NS_NO_GAPCLOSE` (presencia) lo apaga para comparar.
static thread_local size_t ns_gap_n0 = 0, ns_gap_n1 = 0, ns_gap_moved = 0;
static thread_local double ns_gap_a0 = 0., ns_gap_a1 = 0., ns_gap_mat0 = 0., ns_gap_mat1 = 0.;
static thread_local bool   ns_gap_dbg = false;

static bool ns_gap_close_on()
{
    static const bool on = std::getenv("ORCA_NS_NO_GAPCLOSE") == nullptr;
    return on;
}

static double lines_material(const NsLines& lines)
{
    double m = 0.;
    for (const NsLine& l : lines)
        for (size_t i = 1; i < l.pts.size(); ++i)
            m += 0.5 * (l.w[i - 1] + l.w[i]) * (l.pts[i] - l.pts[i - 1]).norm();
    return m;
}

// Huella REAL: el ancho menos h(1−π/4).
static ExPolygons real_cover(const NsLines& lines, double marg)
{
    Polygons cov;
    for (const NsLine& l : lines) {
        std::vector<double> w(l.w.size());
        for (size_t i = 0; i < l.w.size(); ++i)
            w[i] = std::max(0.01, l.w[i] - marg);
        append(cov, line_poly(l.pts, w));
    }
    return union_ex(cov);
}

// Huecos que no llegan a un cordón entero. Los anchos (donde cabe `max_bead`) no son de esto: son
// relleno o bolsita.
static ExPolygons narrow_gaps(const ExPolygon& island, const Polygons& outer_cov, const ExPolygons& cov_lines,
                              const NsParams& P, double& area, size_t& n)
{
    Polygons all = to_polygons(cov_lines);
    append(all, outer_cov);
    const ExPolygons gap = diff_ex(ExPolygons{ island }, union_ex(all));
    ExPolygons out;
    area = 0.;
    n    = 0;
    const float half_cap = float(scaled<double>(P.max_bead / 2.));
    for (const ExPolygon& e : gap) {
        const double a = e.area() * SCALING_FACTOR * SCALING_FACTOR;
        if (a < 0.004)
            continue;   // 0.06 x 0.06: ruido de Clipper, no un hueco
        // 🚨 s338 (TEST23c) — una RENDIJA de menos de 0.06 mm entre dos cordones no es un hueco: la cierra el aplastado
        //    del cordón (y la huella lleva 0.043 de margen). Con el campo salían miles a lo largo de los carriles (se
        //    colocan en una rejilla de 0.01 y la huella es un polígono exacto): largas, pasaban el filtro de área, y el
        //    remate ensanchaba cada carril que tenía una al lado. Medido en el log: +7.9 % de material de media, hasta
        //    +34.6 %, y el batiburrillo de los anillos del compact-E. El hueco de verdad (la P de PLAGE) medía 0.26.
        if (offset_ex(e, -float(scaled<double>(0.03))).empty())
            continue;
        if (!offset_ex(e, -half_cap).empty())
            continue;
        area += a;
        ++n;
        out.push_back(e);
    }
    return out;
}

static void close_gaps(NsLines& lines, const ExPolygon& island, const Polygons& outer_cov, const NsParams& P)
{
    if (!ns_gap_close_on() || lines.empty())
        return;
    // 🚨 s338 (TEST23d) — `w` de una NsLine ES la separación: el conversor (`VariableWidth.cpp:178`) le suma él
    //    h(1−π/4) al escribir el `;WIDTH`. La huella de `line_poly(w)` es la de verdad; no hay margen que restar.
    //    Restándolo (como se hizo al principio), el remate veía rendijas de 0.043 entre TODOS los carriles.
    const double marg     = 0.;
    const double reach    = 0.45;   // hasta dónde, desde el borde real de la línea, se mira
    const double probe    = 0.02;
    const double entry    = 0.06;   // el hueco tiene que empezar pegado al borde, no más allá
    const double smooth_r = 0.30;   // el ensanche se reparte a lo largo de la línea
    const double extra    = 0.015;  // un pelo de solape para que el borde no quede a tope
    if (ns_gap_dbg)
        ns_gap_mat0 += lines_material(lines);
    const auto in_any = [](const ExPolygons& ps, const std::vector<BoundingBox>& bb, const Point& q) {
        for (size_t k = 0; k < ps.size(); ++k)
            if (bb[k].contains(q) && ps[k].contains(q))
                return true;
        return false;
    };
    for (int it = 0; it < 4; ++it) {
        const ExPolygons cov = real_cover(lines, marg);
        double a;
        size_t n;
        const ExPolygons gaps = narrow_gaps(island, outer_cov, cov, P, a, n);
        if (it == 0) {
            ns_gap_a0 += a;
            ns_gap_n0 += n;
        }
        if (gaps.empty())
            break;
        std::vector<BoundingBox> gbb, cbb;
        BoundingBox all_g;
        for (const ExPolygon& gp : gaps) {
            gbb.push_back(get_extents(gp));
            all_g.merge(gbb.back());
        }
        for (const ExPolygon& c : cov)
            cbb.push_back(get_extents(c));
        const coord_t grow_bb = scaled<coord_t>(reach + P.max_bead);
        all_g.offset(grow_bb);
        bool touched = false;
        for (NsLine& L : lines) {
            const size_t m = L.pts.size();
            if (L.kind == Kind::Outer || m < 2)
                continue;
            BoundingBox lb;
            for (const Vec2d& q : L.pts)
                lb.merge(Point(scaled<coord_t>(q.x()), scaled<coord_t>(q.y())));
            if (!lb.overlap(all_g))
                continue;
            const std::vector<Vec2d> nrm = normals(L.pts, false);
            std::vector<double> dw(m, 0.), dc(m, 0.);
            bool any = false;
            for (size_t i = 0; i < m; ++i) {
                const double half = std::max(0., L.w[i] - marg) / 2.;
                double need[2] = { 0., 0. };
                for (int s = 0; s < 2; ++s) {
                    const Vec2d d = nrm[i] * (s == 0 ? 1. : -1.);
                    double first = -1., last = -1.;
                    for (double t = half + 0.5 * probe; t <= half + reach; t += probe) {
                        const Vec2d  q  = L.pts[i] + d * t;
                        const Point  pq(scaled<coord_t>(q.x()), scaled<coord_t>(q.y()));
                        if (in_any(gaps, gbb, pq)) {
                            if (first < 0.)
                                first = t;
                            last = t;
                        } else if (first >= 0. || t > half + entry) {
                            break;
                        }
                    }
                    if (first < 0.)
                        continue;
                    double ext = last + 0.5 * probe - half;
                    // ¿Hay otra línea justo al otro lado? Entonces el hueco se reparte a medias.
                    const Vec2d  qb = L.pts[i] + d * (last + 1.5 * probe);
                    const Point  pb(scaled<coord_t>(qb.x()), scaled<coord_t>(qb.y()));
                    if (in_any(cov, cbb, pb))
                        ext = (first - half) + 0.5 * (last - first + probe);
                    need[s] = ext + extra;
                }
                double tot = need[0] + need[1];
                if (tot <= 0.)
                    continue;
                const double room = std::max(0., P.max_bead - L.w[i]);
                if (tot > room) {
                    const double f = room / tot;
                    need[0] *= f;
                    need[1] *= f;
                    tot = room;
                }
                if (tot <= 1e-6)
                    continue;
                dw[i] = tot;
                dc[i] = 0.5 * (need[0] - need[1]);   // + = hacia `nrm`
                any   = true;
            }
            if (!any)
                continue;
            // Reparto a lo largo de la línea: sin esto el ancho da saltos de muestra a muestra.
            std::vector<double> s(m, 0.);
            for (size_t i = 1; i < m; ++i)
                s[i] = s[i - 1] + (L.pts[i] - L.pts[i - 1]).norm();
            std::vector<double> sw(m, 0.), sc(m, 0.);
            size_t lo = 0, hi = 0;
            double sum_w = 0., sum_c = 0.;
            for (size_t i = 0; i < m; ++i) {
                while (hi < m && s[hi] - s[i] <= smooth_r) {
                    sum_w += dw[hi];
                    sum_c += dc[hi];
                    ++hi;
                }
                while (s[i] - s[lo] > smooth_r) {
                    sum_w -= dw[lo];
                    sum_c -= dc[lo];
                    ++lo;
                }
                const double cnt = double(hi - lo);
                sw[i] = std::max(dw[i], sum_w / cnt);
                sc[i] = sum_c / cnt;
            }
            for (size_t i = 0; i < m; ++i) {
                if (sw[i] <= 0.)
                    continue;
                L.w[i] = std::min(P.max_bead, L.w[i] + sw[i]);
                // 🚨 Los EXTREMOS no se mueven: son los que la costura empareja con otras líneas.
                if (i != 0 && i + 1 != m)
                    L.pts[i] += nrm[i] * sc[i];
            }
            ++ns_gap_moved;
            touched = true;
        }
        if (!touched)
            break;
    }
    if (ns_gap_dbg) {
        double a;
        size_t n;
        narrow_gaps(island, outer_cov, real_cover(lines, marg), P, a, n);
        ns_gap_a1 += a;
        ns_gap_n1 += n;
        ns_gap_mat1 += lines_material(lines);
    }
}

// ── s338: el planificador por CAMPO (NeoStrokeField.*) ──────────────────────────
// NEOTKO_NEOSTROKE_TAG s338 — sustituye plan_island + stitch: devuelve CAMINOS ya cosidos, cada uno continuo
// (en `path_runs` una sola línea es un solo Run). Lo de después (flow_rules, emit_path, patín, orden) no cambia.
// 🏁 s338 (tras el TEST24) — es el planificador POR DEFECTO. `ORCA_NS_STROKES` (presencia) vuelve al de trazos para
// comparar. El de trazos sigue de reserva para las islas que el campo no toma (pieza ancha, rejilla, sin caminos).
// Mandos finos por env var mientras se afina: ORCA_NS_FIELD_HYST / _PRUNE / _TAIL / _SNAP / _TGT.
static bool ns_field_on()
{
    static const bool on = std::getenv("ORCA_NS_STROKES") == nullptr;
    return on;
}

static std::vector<NsPath> plan_island_field(const ExPolygon& island, const Polygons& outer_cov, const NsParams& P,
                                             const SkeletonParams& skp, StrokeStats& st, FieldStats& fs)
{
    FieldParams fp;
    fp.outer_w = P.outer_w;
    {   // s342f — MANDO DE SESIÓN: la rejilla del campo en mm (0.01 de fábrica). Más gruesa = menos memoria y menos
        //    tiempo por isla (10 islas a la vez a 0.01 se pelean por la memoria: cada letra grande ~150 MB). Sin tocar
        //    el 3mf ni recompilar, para medir cuánto cambia el G-code antes de decidir nada.
        double v;
        if (ns_env_get("ORCA_NS_CELL", 0.005, 0.05, v))
            fp.cell = v;
    }
    fp.max_stroke_w = P.max_stroke_w;   // *Widest shape handled*: lo más ancho es una pieza, no una letra
    fp.band         = P.band;           // s340 — modo banda
    // Los límites son los MISMOS mandos de hoy, pasados a separación (el campo trabaja con lo que el cordón
    // cubre de verdad): máximo = *Widest line allowed*, mínimo = *Thinnest printable line*, objetivo =
    // *Target line width*.
    // 🚨 s338 (TEST23d) — todo en SEPARACIÓN, que es lo que es `w` en NeoStroke: el conversor suma h(1−π/4) él solo.
    //    Hasta el TEST23d se sumaba aquí también y cada cordón del campo salía 0.043 más ancho (~+12 % de plástico).
    fp.wmax = std::max(0.10, P.max_bead);
    fp.wmin = std::max(0.08, std::min(fp.wmax, P.bead_min));
    fp.tgt  = std::clamp(P.ceiling_w, fp.wmin, fp.wmax);
    double v;
    if (ns_env_get("ORCA_NS_FIELD_HYST", 0., 20., v))  fp.hyst     = v;
    if (ns_env_get("ORCA_NS_FIELD_PRUNE", 0., 50., v)) fp.prune    = v;
    if (ns_env_get("ORCA_NS_FIELD_TAIL", 0., 1., v))   fp.cola_min = v;
    if (ns_env_get("ORCA_NS_FIELD_SNAP", 0., 0.49, v)) fp.snap     = v;
    if (ns_env_get("ORCA_NS_FIELD_TGT", 0.1, 1.5, v))  fp.tgt      = v;

    (void)skp;
    std::vector<FieldLane> lanes = field_lanes(island, fp, fs);
    if (fs.skipped || lanes.empty())
        return {};
    lanes = field_resample(lanes, fp.step);   // 🚨 un punto por celda ahogaba el remate (s338)
    // remate: el `close_gaps` de s338 sobre los carriles (en `;WIDTH`, que es lo que espera)
    NsLines ls;
    ls.reserve(lanes.size());
    for (const FieldLane& L : lanes) {
        NsLine n;
        n.kind = Kind::Stroke;
        n.pts  = L.pts;
        n.w  = L.w;   // separación, igual que el resto de NeoStroke
        if (L.closed) {   // close_gaps y line_poly trabajan con polilíneas: el anillo se cierra a mano
            n.pts.push_back(L.pts.front());
            n.w.push_back(n.w.front());
        }
        ls.push_back(std::move(n));
    }
    const auto t_rem = std::chrono::steady_clock::now();
    close_gaps(ls, island, outer_cov, P);
    fs.t_remate += ns_since(t_rem);   // ns_since ya da ms
    for (size_t i = 0; i < lanes.size(); ++i) {
        const size_t m = lanes[i].pts.size();
        for (size_t k = 0; k < m; ++k) {
            lanes[i].pts[k] = ls[i].pts[k];
            lanes[i].w[k]   = std::max(0.01, ls[i].w[k]);
        }
    }
    lanes = field_trim_tails(lanes, fp.cola_min, fs);
    const std::vector<FieldLane> tramos = field_stitch(island, lanes, fp, fs);
    // 🏁 s338 (TEST24, la onda) — ARRANCAR POR EL LADO GORDO. Un tramo del campo que empieza en una COLA afilada
    //    arranca fino justo después de un viaje, con la retracción y la presión aún sin llegar: el cordón sale corto.
    //    Si además esa cola cae junto a otras puntas (el nudo donde cambia el nº de cordones), queda un hoyo, y como
    //    la geometría es la misma capa a capa, se apila (G-code del TEST24: dos arranques y una parada a 0.23 mm).
    //    Se imprime al revés: el arranque cae donde el cordón es ancho y la cola fina queda al FINAL, con presión.
    //    Mismo camino y mismo plástico; sólo el sentido. Se fija para que el orden de la isla no lo deshaga.
    //    `ORCA_NS_NO_WIDE_START` (presencia) lo apaga para comparar.
    static const bool wide_start = std::getenv("ORCA_NS_NO_WIDE_START") == nullptr;
    auto end_width = [](const std::vector<Vec2d>& p, const std::vector<double>& w, bool at_start) {
        double acc = 0., sum = 0., len = 0.;
        const size_t n = p.size();
        for (size_t k = 0; k + 1 < n && acc < 0.30; ++k) {
            const size_t i = at_start ? k : n - 1 - k, j = at_start ? k + 1 : n - 2 - k;
            const double l = (p[j] - p[i]).norm();
            sum += 0.5 * (w[i] + w[j]) * l;
            len += l;
            acc += l;
        }
        return len > 1e-9 ? sum / len : (at_start ? w.front() : w.back());
    };
    // 🏁 s339 (TEST25, el agujero de arriba de los anillos en las 8 zonas) — ACABAR EN LOS CRUCES. El lado gordo de
    //    arriba mete los arranques JUSTO en la bifurcación (donde el carril es ancho): tres tramos arrancando juntos
    //    tras su viaje, ninguno con presión, y queda un agujero que el plan daba por tapado. Las PARADAS juntas no lo
    //    dejan: llegan con presión de sobra. Con `end_at_junctions`, el extremo con MÁS puntas ajenas cerca (a menos de
    //    `junction_mm`) es donde el tramo ACABA; si los dos extremos están igual, decide el lado gordo como hasta ahora.
    //    Mismo camino, mismo plástico: sólo el sentido.
    std::vector<int> crowd0(tramos.size(), 0), crowd1(tramos.size(), 0);
    if (P.end_at_junctions) {
        std::vector<std::pair<Vec2d, size_t>> tips;
        for (size_t i = 0; i < tramos.size(); ++i) {
            const auto& p = tramos[i].pts;
            if (p.size() >= 2 && (p.front() - p.back()).norm() > 1e-6) {
                tips.emplace_back(p.front(), i);
                tips.emplace_back(p.back(), i);
            }
        }
        const double r2 = P.junction_mm * P.junction_mm;
        for (size_t i = 0; i < tramos.size(); ++i) {
            const auto& p = tramos[i].pts;
            if (p.size() < 2 || (p.front() - p.back()).norm() <= 1e-6)
                continue;
            for (const auto& [q, j] : tips) {
                if (j == i) continue;
                if ((q - p.front()).squaredNorm() < r2) ++crowd0[i];
                if ((q - p.back()).squaredNorm() < r2)  ++crowd1[i];
            }
        }
    }
    std::vector<NsPath> paths;
    paths.reserve(tramos.size());
    for (size_t ti = 0; ti < tramos.size(); ++ti) {
        const FieldLane& t = tramos[ti];
        NsLine n;
        n.kind = Kind::Stroke;
        n.pts  = t.pts;
        for (double s : t.w)
            n.w.push_back(std::min(P.max_bead, s));
        const bool open = n.pts.size() >= 2 && (n.pts.front() - n.pts.back()).norm() > 1e-6;
        if (P.end_at_junctions && open && crowd0[ti] != crowd1[ti]) {
            if (crowd0[ti] > crowd1[ti]) {   // arranca en el racimo: se gira para acabar en él
                n.reverse();
                ++fs.junction_ends;
            }
            n.fixed_dir = true;
        } else if (wide_start && open) {
            if (end_width(n.pts, n.w, true) < 0.95 * end_width(n.pts, n.w, false)) {
                n.reverse();
                ++fs.wide_starts;
            }
            n.fixed_dir = true;
        }
        ++st.strokes;
        paths.push_back(NsPath{ std::move(n) });
    }
    return paths;
}

static NsLines plan_island(const ExPolygon& island, const Polygons& outer_cov, const NsParams& P,
                           const SkeletonParams& skp, StrokeStats& st, SkeletonStats* sk_stats)
{
    auto tp = std::chrono::steady_clock::now();
    const StrokeSkeleton sk(island, skp);
    if (sk_stats != nullptr)
        *sk_stats = sk.stats();
    const Boundary bnd(island);
    ns_t_skel += ns_since(tp);

    tp = std::chrono::steady_clock::now();
    // NEOTKO_NEOSTROKE_TAG s329 — 🚨 NINGUNA RAMA VUELVE A PINTAR LO QUE YA PINTÓ OTRA.
    // Medido en el G-code del TEST08: una barra lisa de 3.20 x 12 mm salía con `ramas=5 lineas=33`
    // y el 38 % de su superficie pintada DOS veces, concentrado en los 2 mm de cada extremo
    // (60-98 % doble ahí, 11.5 % en el centro, que es el solape de 0.043 de diseño). El volumen
    // extruido era 1.25 veces el hueco disponible. ESO es el pegote de los remates.
    // La causa: el esqueleto de la barra son el eje MÁS cuatro ramitas de esquina (viven porque
    // `corner_hooks` baja la poda a 0.8), y cada ramita planificaba su abanico ENTERO de k líneas
    // encima del abanico del eje.
    // El arreglo no es cazar "ramitas de esquina": el test `spur` (hoja ↔ cruce) marcaría también
    // el palo de una T, que es un trazo de verdad y necesita su abanico. Se planifica de la rama
    // más larga a la más corta y se mide: una línea cuya huella ya está cubierta no aporta nada.
    // Lo que quede libre en la esquina se lo lleva el residuo, que es donde vive la rampa.
    // NEOTKO_NEOSTROKE_TAG s330 — EL NODO DE TAPA, y por qué la tapa plana salía vacía.
    // Medido en el G-code del TEST08-A: el 100 % de lo que queda sin cubrir en la barra de 3.20
    // está en los 2 mm de las tapas, y en la cuña los 36 primeros de sus 39 mm salen con CERO
    // hueco. El esqueleto de la barra lo explica solo:
    //     rama 1: largo 8.80  deg0=3 deg1=3  de (1.60,1.60) a (1.60,10.40)   <- el tronco
    //     ramas 0,2,3,4: 2.2 mm, deg 3 -> 1, cada una a una esquina a 45°
    // El tronco muere en un cruce de GRADO 3 a 1.6 mm del borde, y `extend_tips` sólo alargaba
    // puntas de grado 1. O sea: las k líneas se paraban en seco y la tapa entera se la dejaban a
    // dos ramitas diagonales, que abren su abanico hacia las esquinas. Eso es el hueco, y también
    // el abanico feo del preview.
    // Un nodo es de TAPA cuando lo que sale de él, aparte de este trazo, son SÓLO hojas cortas:
    // todas las demás ramas mueren libres y miden menos de dos radios del disco inscrito. Una T,
    // una cruz o la unión de una letra no lo cumplen, porque ahí las otras ramas siguen. Y si
    // alguna rama del nodo se cayó por corta, el grado no cuadra y no es tapa: se prefiere no
    // alargar a alargar de más.
    // 🚨 EL ORDEN IMPORTA: `extend_tips` se hace AHORA, rama a rama, ANTES de medir el solape
    // contra lo ya puesto. Dejarlo al final (como estaba) significa que las ramitas de esquina se
    // miden contra un tronco que todavía no ha llegado a la tapa, sobreviven a la poda, y luego
    // el tronco alargado las repinta: en el prototipo eso es el doble de solape y un 10 % más de
    // material. Alargando dentro del bucle, las ramitas se caen solas y la barra sale de UN
    // camino en vez de cinco.
    NsLines lines;
    // NEOTKO_NEOSTROKE_TAG s335 — el muro exterior es SIEMPRE de Classic. Aquí sólo el interior,
    // planificado sobre la isla entera y su esqueleto, que es el que ya está construido arriba.
    const NsParams& Pi = P;
    {
        CoverIndex done;   // s329 — ninguna rama vuelve a pintar lo que ya pintó otra
        const auto plan_pieza = [&](const ExPolygon& pieza, const StrokeSkeleton& psk, const Boundary& pbnd) {
        const ExPolygons R = offset_ex(ExPolygons{ pieza }, -float(scaled<double>(P.outer_w)));
        std::vector<const StrokeBranch*> brs;
        brs.reserve(psk.branches().size());
        for (const StrokeBranch& br : psk.branches())
            if (br.length_mm >= P.step * 2.)
                brs.push_back(&br);
        // Quién toca a quién: los extremos se comparan por posición redondeada a la micra, que es
        // como los emparejó el propio esqueleto al extraer las ramas.
        auto key = [](const Vec2d& q) {
            return std::make_pair(std::llround(q.x() * 1000.), std::llround(q.y() * 1000.));
        };
        std::map<std::pair<long long, long long>, std::vector<const StrokeBranch*>> at_node;
        for (const StrokeBranch* br : brs) {
            if (br->closed || br->pts.empty())
                continue;
            at_node[key(br->pts.front())].push_back(br);
            at_node[key(br->pts.back())].push_back(br);
        }
        auto is_cap = [&](const StrokeBranch* br, bool at_back) {
            if (br->closed || br->pts.empty() || br->r.empty())
                return false;
            const size_t deg = at_back ? br->deg_back : br->deg_front;
            if (deg < 3)
                return false;
            const Vec2d  q = at_back ? br->pts.back() : br->pts.front();
            const double rr = at_back ? br->r.back()  : br->r.front();
            auto it = at_node.find(key(q));
            if (it == at_node.end())
                return false;
            size_t others = 0;
            for (const StrokeBranch* o : it->second) {
                if (o == br)
                    continue;
                ++others;
                // la OTRA punta de la vecina tiene que ser hoja, y la vecina tiene que ser corta
                const bool o_front_here = key(o->pts.front()) == key(q);
                const size_t o_far_deg  = o_front_here ? o->deg_back : o->deg_front;
                if (o_far_deg != 1 || o->length_mm > 2. * rr)
                    return false;
            }
            // si falta alguna rama del nodo (se cayó por corta), no se da por tapa
            return others > 0 && others + 1 == deg;
        };
        std::stable_sort(brs.begin(), brs.end(),
                         [](const StrokeBranch* a, const StrokeBranch* b) { return a->length_mm > b->length_mm; });
        for (const StrokeBranch* br : brs) {
            NsLines l = plan_stroke(*br, pbnd, Pi, st, is_cap(br, false), is_cap(br, true));
            {
                auto tp_tips = std::chrono::steady_clock::now();
                // 🚨 De dueños del muro no hay banda que descontar al alargar una punta: el margen
                //    es 0 y el hueco es `2·bnd` sobre el borde de SU zona. Con `outer_w` ahí, las
                //    puntas se paraban un muro antes del borde de su propia zona, que es justo la
                //    cobertura del contorno que el muro de Classic tapaba sin que se notara.
                extend_tips(l, R, Pi, pbnd, P.outer_w, &st.caps);
                ns_t_tips += ns_since(tp_tips);
            }
            for (NsLine& L : l) {
                const ExPolygons foot = union_ex(line_poly(L));
                const double a_foot = area_mm2(foot);
                if (a_foot <= 0.)
                    continue;
                const ExPolygons done_u = done.empty() ? ExPolygons{} : done.near(get_extents(foot));
                const double over_a = done_u.empty() ? 0. : area_mm2(intersection_ex(foot, done_u));
                if (over_a >= P.branch_overlap * a_foot) {
                    ++st.repainted;
                    continue;
                }
                done.add(foot);
                lines.push_back(std::move(L));
            }
        }
        };   // plan_pieza
        plan_pieza(island, sk, bnd);
    }
    ns_t_strokes += ns_since(tp);
    tp = std::chrono::steady_clock::now();

    // Varias vueltas: la primera línea de un engorde deja una media luna a su lado, y esa media
    // luna también cabe. Se para sola cuando ya no queda nada que quepa. La cobertura se lleva
    // acumulada, no se rehace en cada vuelta.
    {
        Polygons cov = outer_cov;
        for (const NsLine& l : lines)
            append(cov, to_polygons(union_ex(line_poly(l))));   // línea a línea, luego el total
        ExPolygons covered = union_ex(cov);
        for (int pass = 0; pass < P.residual_passes; ++pass) {
            NsLines add = fill_residual(island, covered, Pi, skp, st);
            if (add.empty())
                break;
            Polygons more;
            for (const NsLine& l : add)
                append(more, to_polygons(union_ex(line_poly(l))));
            covered = union_ex(covered, more);
            lines.insert(lines.end(), std::make_move_iterator(add.begin()), std::make_move_iterator(add.end()));
        }
    }
    ns_t_residual += ns_since(tp);
    tp = std::chrono::steady_clock::now();

    // 🚨 Un relleno del residuo es un DETALLE por definición: un ir de A a B. La máquina no lo sabe
    // hacer más fino que `detail_min`, así que el que no admite el cordón se va (trampa 15 y el
    // filtro de C5). Dejaba trocitos en el remate de un palo y las costuras entre ellos se cruzaban
    // por el medio: la "cruz" de la U. Mejor el hueco que la cruz.
    // 🚨 Y el ancho del detalle es max(detail_min, hueco local), NO `detail_min` a secas: una cuña
    // triangular no admite un cordón constante y se descartaría entera (trampa 15).
    // 🚨 s329 — el suelo era `P.nozzle`, el diámetro del cabezal, y eso es FALSO: una boquilla de
    // 0.4 saca 0.25. Con el cabezal a pelo, una media luna de 0.2 mm se engordaba a 0.4, pisaba de
    // más y se descartaba: el hueco se quedaba, y la sonda decía `CABE el cordon=0.000` tan
    // tranquila con 20 mm² sin cubrir. Ahora lo pone `neostroke_detail_min_pct`. Y de paso esto
    // DESBLOQUEA la rampa: el caso "engorde sin ramas útiles" de `fill_residual` ya escribe una
    // línea con el ancho siguiendo el hueco punto a punto, y no llegaba nunca a ejecutarse.
    {
        NsLines keep;
        CoverIndex room;             // el muro exterior también es material puesto
        // NEOTKO_NEOSTROKE_TAG s335 — 🚨 desde que NeoWall se fue, el muro exterior lo pone SIEMPRE
        // Classic y vive entero en `outer_cov`: NINGUNA línea de `lines` es `Kind::Outer`. El bucle
        // de abajo no llega a añadir nada nunca. Se deja porque el día que NeoStroke vuelva a poner
        // muro propio, esto es exactamente lo que hay que volver a contar: sin ello, una banda fina
        // pegada a NUESTRO muro reparte medio ensanche contra él, se pasa del 35 % y se tira entera
        // (fue el canal del travesaño de la H en el G-code de s333).
        ExPolygons outer_cov_ex = union_ex(outer_cov);
        {
            Polygons wall;
            for (const NsLine& l : lines)
                if (l.kind == Kind::Outer)
                    append(wall, line_poly(l));
            if (!wall.empty())
                outer_cov_ex = union_ex(outer_cov_ex, wall);
        }
        room.add(outer_cov_ex);
        for (const NsLine& l : lines)
            if (l.kind != Kind::Fill) {
                keep.push_back(l);
                room.add(union_ex(line_poly(l)));   // una forma por línea, no un círculo por segmento
            }
        const ExPolygons isl{ island };
        for (NsLine& l : lines) {
            if (l.kind != Kind::Fill)
                continue;
            std::vector<double> wide_w(l.pts.size());
            for (size_t i = 0; i < l.pts.size(); ++i)
                wide_w[i] = std::min(P.max_bead, std::max(P.detail_min, l.w[i]));
            const ExPolygons wide = union_ex(line_poly(l.pts, wide_w));
            if (wide.empty())
                continue;
            const double a_wide = area_mm2(wide);
            if (a_wide <= 0.)
                continue;
            const double out_a  = area_mm2(diff_ex(wide, isl));
            const ExPolygons room_u = room.empty() ? ExPolygons{} : room.near(get_extents(wide));
            double over_a = room_u.empty() ? 0. : area_mm2(intersection_ex(wide, room_u));
            // NEOTKO_NEOSTROKE_TAG s330 — el solape contra el MURO se cuenta aparte del solape
            // contra los trazos. Lo que `detail_overlap` quiere evitar es la "cruz" de la U: dos
            // detalles cruzándose por el medio. Pisar el muro exterior no es eso: engorda un pelo
            // el perímetro y ya. Y el caso que se estaba perdiendo es SIEMPRE el mismo, una banda
            // fina pegada al muro (la cara de la tapa): al engordarla al cordón mínimo repartía
            // medio ensanche contra el muro, se pasaba del 35 % y se tiraba entera. El hueco se
            // quedaba y la sonda lo cantaba como `CABE el cordon`.
            if (ns_wall_apart() && !outer_cov_ex.empty())
                over_a = std::max(0., over_a - area_mm2(intersection_ex(wide, outer_cov_ex)));
            if (out_a < 0.10 * a_wide && over_a < P.detail_overlap * a_wide) {
                l.w = wide_w;
                keep.push_back(l);
                room.add(wide);
            } else {
                st.fills = st.fills > 0 ? st.fills - 1 : 0;
            }
        }
        lines = std::move(keep);
    }
    // s338 — con TODAS las líneas ya puestas, se miran todos los huecos a la vez.
    close_gaps(lines, island, outer_cov, P);
    ns_t_details += ns_since(tp);
    return lines;
}

// ── C4: la costura ──────────────────────────────────────────────────────────
// Aquí es donde se gana la continuidad. Dos cosas se cosen: seguir RECTO en un cruce, y el giro
// en U entre dos líneas VECINAS del mismo trazo — que es lo que hace que un trazo de k líneas
// salga de una sola pasada en serpentina, y de donde viene casi toda la continuidad. El giro en U
// cuesta más que seguir recto, así que el cruce gana si hay cruce.
static std::vector<NsPath> stitch(NsLines lines, const ExPolygons& region, const NsParams& P)
{
    const int n = int(lines.size());
    if (n == 0)
        return {};

    struct End { int line; int side; };   // side 0 = principio, 1 = final
    std::vector<End> ends;
    for (int i = 0; i < n; ++i) {
        const NsLine& l = lines[i];
        if ((l.pts.front() - l.pts.back()).norm() < 1e-6 && l.length() > 1.0)
            continue;                     // ya es un bucle cerrado
        // NEOTKO_NEOSTROKE_TAG s335 — inerte hoy: sin NeoWall nada es `Kind::Outer`. Regla que hay
        // que conservar si vuelve un muro propio: un muro NO se cose a nada, ni a otro anillo ni a
        // un trazo, porque su valor entero es ser UN bucle continuo sin costuras nuevas.
        if (l.kind == Kind::Outer)
            continue;
        ends.push_back({ i, 0 });
        ends.push_back({ i, 1 });
    }
    auto pt = [&](const End& e) { return e.side == 0 ? lines[e.line].pts.front() : lines[e.line].pts.back(); };
    auto dirv = [&](const End& e) {       // dirección de SALIDA del extremo
        const NsLine& l = lines[e.line];
        const Vec2d a = e.side == 0 ? l.pts[1] : l.pts[l.pts.size() - 2];
        const Vec2d b = e.side == 0 ? l.pts.front() : l.pts.back();
        Vec2d d = b - a;
        const double m = d.norm();
        return m > 0. ? Vec2d(d / m) : Vec2d(1., 0.);
    };

    struct Cand { double cost; int ia, ib; };
    std::vector<Cand> cand;
    for (size_t ia = 0; ia < ends.size(); ++ia)
        for (size_t ib = ia + 1; ib < ends.size(); ++ib) {
            const End &ea = ends[ia], &eb = ends[ib];
            if (ea.line == eb.line)
                continue;
            const double gap = (pt(ea) - pt(eb)).norm();
            if (gap > P.join_tol)
                continue;
            const Vec2d da = dirv(ea), db = dirv(eb);
            // El enlace continúa si la salida de a y la ENTRADA a b van en el mismo sentido.
            const double turn = std::acos(std::clamp(-(da.dot(db)), -1., 1.)) * 180. / PI;
            if (turn > P.join_turn)
                continue;
            if (gap > 1e-6 && !inside(region, pt(ea), pt(eb)))
                continue;
            cand.push_back({ gap + 0.004 * turn, int(ia), int(ib) });
        }
    for (size_t ia = 0; ia < ends.size(); ++ia)
        for (size_t ib = ia + 1; ib < ends.size(); ++ib) {
            const End &ea = ends[ia], &eb = ends[ib];
            const NsLine &la = lines[ea.line], &lb = lines[eb.line];
            if (ea.line == eb.line || la.sid < 0 || la.sid != lb.sid)
                continue;
            if (std::abs(la.idx - lb.idx) != 1 || ea.side != eb.side)
                continue;
            const double gap = (pt(ea) - pt(eb)).norm();
            if (gap > P.uturn_tol)
                continue;
            if (gap > 1e-6 && !inside(region, pt(ea), pt(eb)))
                continue;
            cand.push_back({ gap + P.uturn_cost, int(ia), int(ib) });
        }
    std::stable_sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });

    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> root = [&](int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    std::vector<char> used(ends.size(), 0);
    std::map<std::pair<int, int>, std::pair<int, int>> joins;   // (línea,lado) -> (línea,lado)
    for (const Cand& c : cand) {
        if (used[c.ia] || used[c.ib])
            continue;
        const End &ea = ends[c.ia], &eb = ends[c.ib];
        if (root(ea.line) == root(eb.line))
            continue;
        used[c.ia] = used[c.ib] = 1;
        parent[root(ea.line)] = root(eb.line);
        joins[{ ea.line, ea.side }] = { eb.line, eb.side };
        joins[{ eb.line, eb.side }] = { ea.line, ea.side };
    }

    // Recorre las cadenas, empezando siempre por un extremo libre.
    std::vector<char>   seen(n, 0);
    std::vector<NsPath> paths;
    for (int i = 0; i < n; ++i) {
        if (seen[i])
            continue;
        std::pair<int, int> start{ i, 0 };
        {
            std::vector<int> stack{ i };
            std::vector<int> comp;
            std::vector<char> in(n, 0);
            while (!stack.empty()) {
                const int c = stack.back();
                stack.pop_back();
                if (in[c])
                    continue;
                in[c] = 1;
                comp.push_back(c);
                for (int side = 0; side < 2; ++side) {
                    auto it = joins.find({ c, side });
                    if (it != joins.end())
                        stack.push_back(it->second.first);
                }
            }
            bool found = false;
            for (int c : comp) {
                for (int side = 0; side < 2 && !found; ++side)
                    if (joins.find({ c, side }) == joins.end()) {
                        start = { c, side };
                        found = true;
                    }
                if (found)
                    break;
            }
        }
        NsPath path;
        std::pair<int, int> cur = start;
        while (true) {
            const int li = cur.first, side = cur.second;
            if (seen[li])
                break;
            seen[li] = 1;
            NsLine L = lines[li];
            if (side != 0)
                L.reverse();
            path.push_back(std::move(L));
            auto it = joins.find({ li, 1 - side });
            if (it == joins.end())
                break;
            cur = it->second;
        }
        if (!path.empty())
            paths.push_back(std::move(path));
    }
    return paths;
}

// Los puntos de un camino ya encadenados, con su ancho. Ancho 0 = ese tramo NO se extruye.
// 🚨 El tipo de cada tramo sale del MISMO bucle que los puntos (trampa 14): montarlo por separado
// se desalinea en cuanto se mete una costura.
// 🚨 Y la costura entre dos líneas que casi se tocan no se extruye (trampa 13): si el salto es más
// corto que medio ancho, los dos remates redondos ya se solapan — el material está puesto y
// extruir encima deja un pegote (el punto rosa de la `e` de Gypsea).
struct Run {
    std::vector<Vec2d>  pts;
    std::vector<double> w;
    // s331b — factor de caudal por punto (1 = sin tocar). Lo llena `flow_ramp` y lo aplica
    // `apply_flow_ramp` DESPUÉS del conversor, sobre `mm3_per_mm`, para no tocar el ancho.
    std::vector<double> f;
    // s336c — los campos `join/join_to/join_w` de s331d se fueron con `cap_join`.
};

// Los puntos de un camino, ya encadenados, partidos en TRAMOS QUE SE EXTRUYEN.
// 🚨 La costura entre dos líneas que casi se tocan no se extruye (trampa 13): si el salto es más
// corto que medio ancho, los dos remates redondos ya se solapan — el material está puesto y
// extruir encima deja un pegote (el punto rosa de la `e` de Gypsea). Ese salto parte el camino en
// dos tramos, que se imprimen seguidos y sin retracción por ir en el mismo cubo sin reordenar.
// 🚨 El corte sale del MISMO bucle que los puntos (trampa 14): llevar la lista por separado se
// desalinea en cuanto se mete una costura.
// NEOTKO_NEOSTROKE_TAG s336 (2_47) — `neostroke_continuous_turns`. LA VUELTA EN U SE EXTRUYE.
// Hasta 2_46 la U se cortaba, y eso deja `k−1` cortes de flujo en cada extremo de cada trazo. s332 midió
// que la raja son justo cortes de flujo; el pegote de s329 no era la U sino el repintado de ramas.
// 🔑 s336g — U CUADRADA, no semicírculo. La v1 recortaba las dos líneas medio cordón y las unía con un
//    semicírculo. En el TEST20 salían MUESCAS en todas las tapas, y el banco `verify/s336_ucap.py` dice por
//    qué: el semicírculo redondea la esquina de un bloque de DOS cordones (radio grande) y deja MÁS hueco
//    que dos remates sueltos (59 contra 35 ×10⁻³ mm² en una tapa de 3 cordones). Además su radio (medio
//    cordón, ~0.17) es más cerrado de lo que la boquilla (radio 0.2) sabe dibujar. La U cuadrada — las dos
//    líneas ya acaban a la misma altura, se unen en RECTO sin recortar — deja 23: sólo la curva normal de
//    cualquier esquina. En movimiento continuo no hay remates redondos que se pisen: sólo una esquina.
// El salto `gap <= w/2` (dos remates que ya se tocan) también se une en recto.
static void join_continuous(Run& cur, const NsLine& L, bool uturn, double w)
{
    (void)uturn;
    (void)w;
    cur.pts.push_back(L.pts.front());
    cur.w.push_back(std::min(cur.w.back(), L.w.front()));
    cur.pts.insert(cur.pts.end(), L.pts.begin() + 1, L.pts.end());
    cur.w.insert(cur.w.end(), L.w.begin() + 1, L.w.end());
}

static std::vector<Run> path_runs(const NsPath& path, const NsParams& P)
{
    std::vector<Run> out;
    Run cur;
    int prev_sid = -1, prev_idx = 0;
    for (const NsLine& L : path) {
        if (L.pts.size() < 2)
            continue;
        if (cur.pts.empty()) {
            cur.pts = L.pts;
            cur.w   = L.w;
            prev_sid = L.sid; prev_idx = L.idx;
            continue;
        }
        const double gap = (cur.pts.back() - L.pts.front()).norm();
        const double w   = std::min(cur.w.back(), L.w.front());
        // NEOTKO_NEOSTROKE_TAG s329 — 🚨 LA VUELTA EN U NO SE EXTRUYE NUNCA.
        // El umbral de arriba (`gap <= w/2`) no salta JAMÁS en el caso que más se repite: las k
        // líneas de un trazo están separadas exactamente `W/k`, que es exactamente su ancho, o sea
        // `gap == w`, el doble del umbral. Así que el remate de cada giro de la serpentina se
        // imprimía como un segmento de ancho `w` y largo `w` tumbado al 100 % encima de los dos
        // cordones vecinos. Con k = 6 son cinco cordones de más en cada extremo del trazo.
        // No vale subir el umbral a secas: una costura de verdad llega hasta `join_tol` = 1.10 y
        // ésa SÍ hay que imprimirla. El giro en U se reconoce por lo que la línea ya lleva encima:
        // mismo trazo y líneas vecinas, que es la misma pareja que eligió `stitch`.
        const bool uturn = (prev_sid >= 0 && prev_sid == L.sid && std::abs(prev_idx - L.idx) == 1);
        prev_sid = L.sid; prev_idx = L.idx;
        if (gap > 1e-9 && (uturn || gap <= w / 2.) && P.cont_turns && cur.pts.size() >= 2) {
            join_continuous(cur, L, uturn, w);   // s336 — sin cortar el flujo
            continue;
        }
        if (gap > 1e-9 && (uturn || gap <= w / 2.)) {
            // Giros continuos APAGADOS: el salto no se extruye y el camino se corta aquí (2.46).
            out.push_back(std::move(cur));       // salto no extruido: se corta aquí
            cur = Run{ L.pts, L.w };
            continue;
        }
        if (gap > 1e-9) {                        // costura de verdad: se imprime
            cur.pts.push_back(L.pts.front());
            cur.w.push_back(w);
            cur.pts.insert(cur.pts.end(), L.pts.begin() + 1, L.pts.end());
            cur.w.insert(cur.w.end(), L.w.begin() + 1, L.w.end());
        } else {
            cur.pts.insert(cur.pts.end(), L.pts.begin() + 1, L.pts.end());
            cur.w.insert(cur.w.end(), L.w.begin() + 1, L.w.end());
        }
    }
    if (cur.pts.size() >= 2)
        out.push_back(std::move(cur));
    return out;
}

// Todos los puntos del camino en orden, sin partir. Para ordenar y para el ancla de C6.
static std::vector<Vec2d> path_polyline(const NsPath& path)
{
    std::vector<Vec2d> pts;
    for (const NsLine& L : path) {
        if (L.pts.empty())
            continue;
        if (pts.empty())
            pts = L.pts;
        else if ((pts.back() - L.pts.front()).norm() > 1e-9)
            pts.insert(pts.end(), L.pts.begin(), L.pts.end());
        else
            pts.insert(pts.end(), L.pts.begin() + 1, L.pts.end());
    }
    return pts;
}

static void reverse_path(NsPath& path)
{
    std::reverse(path.begin(), path.end());
    for (NsLine& l : path)
        l.reverse();
}

// ── la regla de caudal (la parte de planificación) ──────────────────────────
// De Neotko, s324: un camino LARGO es continuo — su ancho puede subir y bajar entre el suelo y el
// techo, porque el caudal se mueve despacio y la máquina lo sigue. Un camino CORTO es un detalle,
// un ir de A a B: o se imprime al ancho del cabezal o no se imprime. Que quede el hueco es mejor
// que poner una raya que la máquina no sabe hacer.
//
// 🚨 "No menos que el cabezal" NO es "exactamente el cabezal" (trampa 15): un detalle va a
// max(cabezal, hueco local), o una cuña triangular — que no admite cordón constante pero sí uno
// que se abre con ella — se descarta entera. Era la pata gorda de la `y` de Gypsea.
// 🚨 Y el umbral de continuo es delicado (trampa 11): a 3.0 mm se caen trazos de verdad (la panza
// de la B mide 2.97 y uno de la A 2.27). A 2.0 se imprimen.
//
// El reparto de C6 se queda sólo con los MOVIMIENTOS (patinaje, wipe dirigido, viaje), que son los
// que tocan el emisor de G-code. Esto es plan, y sin ello quedan pelos de 0.05 mm.
static std::vector<NsPath> flow_rules(std::vector<NsPath> paths, const ExPolygon& island,
                                      const NsParams& P, size_t& dropped)
{
    std::vector<NsPath> keep;
    const ExPolygons isl{ island };
    CoverIndex done;   // lo que ya lleva material, para no pisarlo al ensanchar un detalle
    // NEOTKO_NEOSTROKE_TAG s335 — inerte hoy (el muro de Classic no pasa por esta lista, así que
    // `wall_room` queda vacío). Sólo cuenta si vuelve un muro propio: pisar el muro no es la "cruz"
    // de dos detalles, engorda un pelo el perímetro y ya, y sin esta carve-out se llevaba por
    // delante todas las tiras finas que lo tocan, que son justo las que dejan canal.
    CoverIndex wall_room;
    for (const NsPath& path : paths)
        for (const NsLine& l : path)
            if (l.kind == Kind::Outer)
                wall_room.add(union_ex(line_poly(l)));
    for (NsPath& path : paths) {
        // Aplanado con el mapa de índices de cada línea, para poder devolver los anchos a su
        // sitio. (El prototipo lo hacía con aritmética de índices y se desalineaba en cuanto
        // había una costura; aquí se lleva el mapa.)
        std::vector<Vec2d>  pts;
        std::vector<double> ws;
        std::vector<std::pair<size_t, size_t>> span;   // por línea: [inicio, nº de puntos)
        for (NsLine& L : path) {
            if (L.pts.empty()) {
                span.emplace_back(pts.size(), 0);
                continue;
            }
            const bool joined = !pts.empty() && (pts.back() - L.pts.front()).norm() <= 1e-9;
            span.emplace_back(pts.size() - (joined ? 1 : 0), L.pts.size());
            if (joined) {
                pts.insert(pts.end(), L.pts.begin() + 1, L.pts.end());
                ws.insert(ws.end(), L.w.begin() + 1, L.w.end());
            } else {
                pts.insert(pts.end(), L.pts.begin(), L.pts.end());
                ws.insert(ws.end(), L.w.begin(), L.w.end());
            }
        }
        if (pts.size() < 2)
            continue;
        const double len = poly_len(pts);
        if (len >= P.cont_len) {   // continuo: el ancho ya va clavado entre suelo y techo
            keep.push_back(std::move(path));
            done.add(union_ex(line_poly(pts, ws)));   // una forma por camino
            continue;
        }
        // Detalle.
        std::vector<double> wide_w(ws.size());
        for (size_t i = 0; i < ws.size(); ++i)
            wide_w[i] = std::min(P.max_bead, std::max(P.detail_min, ws[i]));
        size_t lo = 0, hi = ws.size() - 1;
        while (lo < hi && ws[lo] < P.detail_min * 0.8)
            ++lo;
        while (hi > lo && ws[hi] < P.detail_min * 0.8)
            --hi;
        if (hi <= lo) {
            ++dropped;
            continue;
        }
        const std::vector<Vec2d>  seg (pts.begin() + lo, pts.begin() + hi + 1);
        const std::vector<double> segw(wide_w.begin() + lo, wide_w.begin() + hi + 1);
        const ExPolygons wide = union_ex(line_poly(seg, segw));
        if (wide.empty()) {
            ++dropped;
            continue;
        }
        const double a_wide = area_mm2(wide);
        const double out_a  = area_mm2(diff_ex(wide, isl));
        const ExPolygons done_u = done.empty() ? ExPolygons{} : done.near(get_extents(wide));
        double over_a = done_u.empty() ? 0. : area_mm2(intersection_ex(wide, done_u));
        if (ns_wall_apart() && !wall_room.empty()) {
            const ExPolygons wall_u = wall_room.near(get_extents(wide));
            if (!wall_u.empty())
                over_a = std::max(0., over_a - area_mm2(intersection_ex(wide, wall_u)));
        }
        if (a_wide <= 0. || out_a >= 0.10 * a_wide || over_a >= P.detail_overlap * a_wide) {
            ++dropped;
            continue;
        }
        if (lo != 0 || hi != ws.size() - 1) {
            NsLine L;                        // recortado: se queda en una sola línea
            L.kind = path.front().kind;
            L.sid  = path.front().sid;
            L.idx  = path.front().idx;
            L.k    = path.front().k;
            L.pts  = seg;
            L.w    = segw;
            keep.push_back(NsPath{ std::move(L) });
        } else {
            for (size_t li = 0; li < path.size(); ++li) {
                const auto& sp = span[li];
                if (sp.second == 0)
                    continue;
                for (size_t j = 0; j < sp.second && sp.first + j < wide_w.size(); ++j)
                    path[li].w[j] = wide_w[sp.first + j];
            }
            keep.push_back(std::move(path));
        }
        done.add(wide);
    }
    return keep;
}

// ── emision ─────────────────────────────────────────────────────────────────
// NEOTKO_NEOSTROKE_TAG s331 — CURVA DE OVERLAP (peticion de Neotko tras ver el TEST10 impreso).
//
// 🔑 De donde sale, y esto hay que tenerlo claro antes de tocar nada: EL PLAN NO DEJA HUECO.
// Las k lineas van de (i−1)/k a i/k del hueco con ancho W/k, el conversor emite
// `;WIDTH = W/k + h(1−pi/4)` y `mm3_per_mm = h·(W/k)`, asi que dos vecinas se solapan
// h(1−pi/4) = 0.043 mm CLAVADOS y la suma de las k da h·W exacto, a cualquier techo. El raster
// del TEST08 lo confirma en los tres anillos y con los tres techos: sin cubrir 0.000 a 0.037 mm2,
// ratio 1.00 a 1.06. Por eso el medidor no ve la raja y no la va a ver nunca: rasteriza `;WIDTH`
// como una huella plana. La raja esta entre el modelo de cordon y el cordon de verdad.
//
// Lo que SI cambia con el techo es el ANCHO del cordon, porque `k = ceil(W/techo)`:
//     techo 100 % -> toda la lamina de calibracion cae entre 0.369 y 0.398, o sea el cabezal
//     techo 150 % -> cae entre 0.386 y 0.597, segun donde caiga el redondeo de la division
// y el margen de fusion sigue siendo 0.043 FIJOS: el 10 % de un cordon de 0.43 y el 6.7 % de uno
// de 0.64. Las rajas salen en las formas que caen arriba de esa escalera Y donde el camino gira
// (los anillos y las eses; las barras rectas salen igual con los dos techos).
//
// 🚨 El suavizado de caudal de s330 (`neostroke_flow_smoothing`) se probo en el TEST10 y NO movio
// nada en plastico a techo 150 %. Eso descarta la oscilacion de caudal como causa de estas rajas,
// y el mando se ha retirado en s331 junto con `neostroke_flow_adjust`, que era de pruebas.
//
// Lo que hace esto. Multiplica el ancho del cordon por `1 + pct * g_ancho * g_giro`:
//   · g_ancho — 0 por debajo del cabezal, rampa hasta 1 en `ovl_w1` cabezales. Los cordones
//     finos NO se tocan, que es la condicion que puso Neotko.
//   · g_giro  — 0 en recto, rampa entre `ovl_turn0` y `ovl_turn1` grados por mm. 🚨 El giro se
//     mide sobre un tramo de `ovl_span` mm, NO muestra a muestra: con el paso de 0.15 mm un solo
//     grado de ruido ya son 6.7 grados/mm y el recto se encenderia solo.
// Las dos rampas son smoothstep, o sea una curva de verdad y no un escalon que se vea en la pieza.
//
// 🚨 El ancho de una `ThickPolyline` de NeoStroke es la SEPARACION, no la huella. Subirlo sube a
// la vez y en la MISMA proporcion la huella (o sea el solape real con el vecino) y el volumen
// (`mm3_per_mm = h*w`). Es una sobreextrusion deliberada, y va solo donde hace falta.
//
// ⚠️ Dos cosas que conviene saber al leer los resultados:
//   · esto corre al EMITIR, o sea despues del plan, del filtro de cobertura y del residuo. Nadie
//     replanifica con el ancho nuevo, que es justo lo que se quiere: solape ENCIMA del plan.
//   · la linea de fuera de un trazo tambien engorda, y esa crece hacia el muro exterior. Con un
//     10 % sobre un cordon de 0.55 son 0.027 mm por lado. Si sale bulto contra el muro, es esto.
static double ovl_ramp(double x, double a, double b)
{
    if (b <= a)
        return x >= b ? 1. : 0.;
    const double t = std::clamp((x - a) / (b - a), 0., 1.);
    return t * t * (3. - 2. * t);
}

static void flow_ramp(Run& r, const NsParams& P)
{
    const size_t n = r.pts.size();
    r.f.assign(n, 1.0);
    if (P.ovl_pct <= 0. || n < 3 || r.w.size() != n)
        return;
    ++ns_ov_runs;
    std::vector<double> s(n, 0.), ds(n, 0.), turn(n, 0.);
    for (size_t i = 1; i < n; ++i)
        s[i] = s[i - 1] + (r.pts[i] - r.pts[i - 1]).norm();
    for (size_t i = 0; i < n; ++i) {
        const double a = i > 0     ? (r.pts[i]     - r.pts[i - 1]).norm() : 0.;
        const double b = i + 1 < n ? (r.pts[i + 1] - r.pts[i]).norm()     : 0.;
        ds[i] = std::max(1e-9, 0.5 * (a + b));
    }
    // Giro SOSTENIDO: angulo entre la cuerda que entra y la que sale, media ventana cada una. Los
    // dos punteros son monotonos, asi que esto es O(n) y no O(n x ventana).
    const double half = 0.5 * P.ovl_span;
    size_t lo = 0, hi = 0;
    for (size_t i = 0; i < n; ++i) {
        while (lo < i && s[i] - s[lo] > half)
            ++lo;
        if (hi < i)
            hi = i;
        while (hi + 1 < n && s[hi + 1] - s[i] <= half)
            ++hi;
        const double span = s[hi] - s[lo];
        if (lo >= i || hi <= i || span <= 1e-9)
            continue;
        const Vec2d  d1 = r.pts[i]  - r.pts[lo];
        const Vec2d  d2 = r.pts[hi] - r.pts[i];
        const double l1 = d1.norm(), l2 = d2.norm();
        if (l1 <= 1e-9 || l2 <= 1e-9)
            continue;
        const double cs = std::clamp(d1.dot(d2) / (l1 * l2), -1., 1.);
        // 🔑 El angulo entre las dos cuerdas es el giro entre los PUNTOS MEDIOS de cada media
        // ventana, o sea medio `span`. Por eso se divide por span/2 y no por span: asi en un arco
        // sale la curvatura de verdad, (180/pi)/R grados por mm, y un radio de 2 mm da 28.6.
        turn[i] = std::acos(cs) * 180. / PI / (0.5 * span);
    }
    // Las dos puntas no tienen ventana por un lado: heredan la de al lado en vez de salir a cero,
    // que en un anillo dejaria la costura sin overlap justo donde mas se ve.
    turn[0]     = turn[1];
    turn[n - 1] = turn[n - 2];

    double a0 = 0., a1 = 0.;
    bool   any = false;
    for (size_t i = 0; i < n; ++i) {
        const double w0 = r.w[i];
        const double gw = ovl_ramp(w0, P.w_ref, P.ovl_w1 * P.w_ref);
        // 🔑 s331b — `ovl_straight` es cuánto de la rampa se aplica donde NO hay giro. A 1 la
        // rampa depende sólo del ancho y las rectas cobran igual que las curvas, que es lo que
        // pidió Neotko tras el TEST11; a 0 se recupera el comportamiento de s331, sólo en curva.
        const double gt = P.ovl_straight
                        + (1. - P.ovl_straight) * ovl_ramp(turn[i], P.ovl_turn0, P.ovl_turn1);
        const double f  = 1. + P.ovl_pct * gw * gt;
        r.f[i] = f;
        if (f > 1. + 1e-6)
            any = true;
        a0 += w0 * ds[i];
        a1 += w0 * f * ds[i];
    }
    if (any)
        ++ns_ov_touched;
    ns_ov_vol_in  += a0;
    ns_ov_vol_out += a1;
}

// s331b — la rampa se aplica DESPUÉS del conversor, sobre `mm3_per_mm`, para que el ancho, el
// `;WIDTH` y el visor sigan diciendo lo que el PLAN decidió y sólo cambie el plástico.
// 🔑 Se puede emparejar por longitud de arco porque `thick_polyline_to_multi_path` no reordena ni
// mueve puntos: sólo parte segmentos por donde el ancho cambia más que la tolerancia. O sea que el
// recorrido de salida es el MISMO recorrido, con más puntos.
// ⚠️ Lo que esto NO hace: el `;WIDTH` no crece, así que ni el visor ni el medidor de cobertura ven
// este material. La única sonda que lo ve es el `dvol=` de la línea [NS], y la E del G-code.
static void apply_flow_ramp(ExtrusionMultiPath& mp, const Run& r)
{
    const size_t n = r.pts.size();
    if (r.f.size() != n || n < 2)
        return;
    bool any = false;
    for (double v : r.f)
        if (v > 1. + 1e-9) { any = true; break; }
    if (!any)
        return;
    std::vector<double> s(n, 0.);
    for (size_t i = 1; i < n; ++i)
        s[i] = s[i - 1] + (r.pts[i] - r.pts[i - 1]).norm();
    const double total = s.back();
    if (total < 1e-9)
        return;
    double done = 0.;
    size_t j = 0;
    for (ExtrusionPath& p : mp.paths) {
        const double len = unscale<double>(p.polyline.length());
        const double mid = std::min(total, done + 0.5 * len);
        while (j + 1 < n && s[j + 1] < mid)
            ++j;
        double f = r.f[j];
        if (j + 1 < n && s[j + 1] > s[j] + 1e-12) {
            const double u = std::clamp((mid - s[j]) / (s[j + 1] - s[j]), 0., 1.);
            f = r.f[j] + u * (r.f[j + 1] - r.f[j]);
        }
        p.mm3_per_mm *= f;
        done += len;
    }
}

// NEOTKO_NEOSTROKE_TAG s339 — el SOLAPE LATERAL FIJO, sobre `mm3_per_mm` como la rampa: el ancho, el `;WIDTH` y el
// plan no cambian, sólo el plástico (el visor lo ve por el ancho de caudal). Los carriles del campo se planean
// TANGENTES; esto es lo que la extrusión extra en curva hacía sólo en las curvas cerradas (hasta +17 %, la montaña).
static void apply_lane_overlap(ExtrusionMultiPath& mp, const NsParams& P)
{
    if (P.lane_ovl > 0.)
        for (ExtrusionPath& p : mp.paths)
            p.mm3_per_mm *= 1. + P.lane_ovl;
}

// s339b — ARRANQUE ADELANTADO. La IDA: de q (a `lead_in` mm dentro del carril) hasta el arranque real, o sea el
// principio del recorrido al revés. Se emite como su propio trozo, pegado al recorrido (acaba donde éste empieza: sin
// viaje ni retracción entre los dos), a `lead_in_flow` del caudal. Vacía si no aplica.
// 🚨 Sólo si el recorrido es al menos el doble de largo: en un tramo corto la ida y la vuelta serían todo el tramo.
static Run lead_in_of(const Run& r, const NsParams& P)
{
    Run out;
    const double L = P.lead_in;
    if (L <= 1e-6 || r.pts.size() < 2 || r.w.size() != r.pts.size() || poly_len(r.pts) < 2. * L)
        return out;
    double acc = 0.;
    for (size_t k = 0; k + 1 < r.pts.size(); ++k) {
        const double l = (r.pts[k + 1] - r.pts[k]).norm();
        if (acc + l >= L) {
            const double t = l > 1e-12 ? (L - acc) / l : 0.;
            out.pts.push_back(r.pts[k] + t * (r.pts[k + 1] - r.pts[k]));
            out.w.push_back(r.w[k] + t * (r.w[k + 1] - r.w[k]));
            for (size_t m = k + 1; m-- > 0;) { out.pts.push_back(r.pts[m]); out.w.push_back(r.w[m]); }
            break;
        }
        acc += l;
    }
    if (out.pts.size() < 2) { out.pts.clear(); out.w.clear(); }
    return out;
}

// Un camino continuo sale como UNA ThickPolyline por tramo extruido, en un cubo sin reordenar:
// así la impresora lo hace de una pasada y sin retracciones por el medio, que es justo lo que C4
// ha ido a buscar. El conversor es el mismo que usa el gap-fill de Classic y la espina: el ancho
// de la ThickPolyline es el hueco, y el volumen sale hueco × altura, igual que el W de S3D.
static void emit_path(const NsPath& path, PerimeterGenerator& g, ExtrusionEntitiesPtr& out,
                      double& emitted_mm, const NsParams& P)
{
    // NEOTKO_NEOSTROKE_TAG s333 — EL ROL DE CADA CAMINO, en su sitio (decisión suya).
    // Hasta s332 todo NeoStroke salía como `erGapFill`, y eso dejaba fuera de juego media pestaña
    // de ajustes: el muro y el interior se imprimían a velocidad de gap-fill, con el ventilador del
    // gap-fill, y en el visor salían del color del gap infill. Ahora el interior sale como
    // **muro interior**, que es lo que es. (El `erExternalPerimeter` de abajo es la rama de un
    // muro propio, que hoy no existe: lo pone Classic desde que se retiró NeoWall en s335.)
    // 🚨 El ORDEN no cambia por esto: lo decide el cubo por isla con `no_sort` y la rotación de
    //    `wall_sequence` del final, y eso ya era nuestro. Lo que cambia es que las velocidades, la
    //    aceleración y el ventilador de MURO pasan a aplicarse de verdad, y que el preview deja de
    //    mentir sobre qué es cada cosa.
    // 🚨 UN rol por CAMINO, no por línea: un `Run` puede llevar trazo, costura y residuo seguidos, y
    //    partirlo por rol sería cortar el camino continuo — justo lo que cuesta cortes de flujo
    //    (s332). Así que el residuo que viaja cosido dentro de un camino sale como muro interior
    //    también, y se pierde el poder distinguirlo por `;TYPE`. La cuenta sigue en `rellenos=` de
    //    la sonda `[NS]`.
    const ExtrusionRole role = (!path.empty() && path.front().kind == Kind::Outer) ? erExternalPerimeter
                                                                                  : erPerimeter;
    std::vector<Run> runs_of = path_runs(path, P);
    if (runs_of.size() > 1)
        ns_cuts += runs_of.size() - 1;      // s336 — la sonda `cortes=`
    for (Run& r : runs_of) {
        if (r.pts.size() < 2)
            continue;
        // s339b — la ida adelantada, antes del recorrido y pegada a él.
        if (role != erExternalPerimeter) {
            const Run li = lead_in_of(r, P);
            if (li.pts.size() >= 2) {
                ThickPolyline lt;
                for (const Vec2d& q : li.pts)
                    lt.points.emplace_back(scaled<coord_t>(q.x()), scaled<coord_t>(q.y()));
                for (size_t i = 0; i + 1 < li.pts.size(); ++i) {
                    lt.width.push_back(coordf_t(scaled<double>(li.w[i])));
                    lt.width.push_back(coordf_t(scaled<double>(li.w[i + 1])));
                }
                lt.endpoints = { true, true };
                ExtrusionMultiPath lm = thick_polyline_to_multi_path(lt, role, g.solid_infill_flow,
                                                                     scaled<float>(ns_conv_tol()), float(SCALED_EPSILON));
                for (ExtrusionPath& q : lm.paths)
                    q.mm3_per_mm *= P.lead_in_flow;
                if (lm.paths.size() == 1)
                    out.emplace_back(new ExtrusionPath(std::move(lm.paths.front())));
                else if (!lm.paths.empty())
                    out.emplace_back(new ExtrusionMultiPath(std::move(lm)));
                emitted_mm += poly_len(li.pts);
            }
        }
        flow_ramp(r, P);                    // s331b — se sale solo si el mando está a 0
        ThickPolyline tp;
        tp.points.reserve(r.pts.size());
        // ⚠️ Dos trampas juntas en `ThickPolyline::width`, y las dos se pagan con basura:
        //   · va ESCALADO, no en mm;
        //   · lleva DOS entradas por SEGMENTO (el ancho de entrada y el de salida), no una por
        //     punto: `thicklines()` lee width[2i] y width[2i+1]. Con una por punto se lee fuera y
        //     salen anchos de 0 o de -inf, y el slice aborta con "negative spacing".
        for (const Vec2d& q : r.pts)
            tp.points.emplace_back(scaled<coord_t>(q.x()), scaled<coord_t>(q.y()));
        tp.width.reserve(2 * (r.pts.size() - 1));
        for (size_t i = 0; i + 1 < r.pts.size(); ++i) {
            tp.width.push_back(coordf_t(scaled<double>(r.w[i])));
            tp.width.push_back(coordf_t(scaled<double>(r.w[i + 1])));
        }
        tp.endpoints = { true, true };
        emitted_mm += poly_len(r.pts);
        // 🚨 NO se pasa por `variable_width()`: cuando el rol es erGapFill y la polilínea está
        // CERRADA, manda el bucle a `closed_gap_fill_loop_to_extrusion_paths()`, que le saca la
        // MEDIA de los anchos y emite el bucle entero a ancho constante. Para el gap-fill de Orca
        // tiene sentido (evita el escalón en la costura); para NeoStroke es la muerte: en un
        // anillo excéntrico — el WEIRD de la lámina, pared de 0.28 a 1.49 mm — el ancho real va
        // de 0.05 a 0.79 y el bucle salía entero a 0.23, o sea pisando el muro por el lado fino y
        // sin llegar a cubrir por el grueso (s325, lo vio Neotko en el visor). El ancho variable
        // ES el motor. De paso se evita el `are_near_duplicate_closed_gap_fill_loops` de ahí
        // dentro, que podría tirar uno de dos bucles concéntricos de un trazo con k = 2.
        // 🚨 El FLUJO sigue siendo `solid_infill_flow` a propósito, aunque el rol ya no lo sea: de él
        //    sólo se usan la altura y el modelo de cordón (`with_width()` le pone el ancho del plan
        //    punto a punto, ver `VariableWidth.cpp:177`), así que cambiarlo movería el VOLUMEN
        //    extruido, que es lo único que no queremos tocar al cambiar de rol. Un cambio a la vez.
        ExtrusionMultiPath mp = thick_polyline_to_multi_path(tp, role, g.solid_infill_flow,
                                                             scaled<float>(ns_conv_tol()), float(SCALED_EPSILON));
        if (mp.paths.empty())
            continue;
        apply_flow_ramp(mp, r);             // s331b — el plástico de más, sólo en `mm3_per_mm`
        if (role != erExternalPerimeter)
            apply_lane_overlap(mp, P);      // s339 — solape lateral fijo (0 = nada)
        if (mp.paths.size() == 1)
            out.emplace_back(new ExtrusionPath(std::move(mp.paths.front())));
        else
            out.emplace_back(new ExtrusionMultiPath(std::move(mp)));
    }
}

// ── C6: patinaje ────────────────────────────────────────────────────────────
// De un camino al siguiente POR ENCIMA de lo ya puesto: sin extruir y sin retraer. Sale como un
// ExtrusionPath sin extrusión (lo mismo que el "move inwards before retraction" de GCode.cpp): el
// emisor lo recorre a la velocidad del rol, y como el siguiente camino arranca justo donde acaba,
// no hay viaje, ni retracción, ni wipe hacia atrás.
//
// Dos casos, y si ninguno cabe se queda el viaje de hoy (con su retracción):
//   1. recto  — el salto va entero sobre material;
//   2. rodeo  — el camino más corto POR DENTRO del material, si no pasa de `detour × salto recto`.
//      Es el factor de S3D (Maximum movement detour factor), relativo al salto y no en mm.
// Prior art del combing: docs/FUTURE/NEOSTROKE_FASE_C_PREPLAN.md §9.

// ¿El segmento va entero sobre el suelo? Se tolera un resto de 0.02 mm por los bordes.
// Sonda de coste (s326): cuántas veces se pregunta a Clipper, por capa. thread_local porque las
// capas se laminan en paralelo y cada una corre entera en su hilo.
static thread_local size_t ns_probe_seg_calls = 0, ns_probe_raster_rows = 0, ns_probe_astar = 0;

static bool skate_seg_ok(const Vec2d& a, const Vec2d& b, const ExPolygons& ground)
{
    ++ns_probe_seg_calls;
    const Point pa(scaled<coord_t>(a.x()), scaled<coord_t>(a.y()));
    const Point pb(scaled<coord_t>(b.x()), scaled<coord_t>(b.y()));
    if (pa == pb)
        return true;
    double out = 0.;
    for (const Polyline& p : diff_pl(Polylines{ Polyline(pa, pb) }, ground))
        out += unscale<double>(p.length());
    return out <= 0.02;
}

// Ruta sobre `ground` de `a` a `b`, o vacía si no cabe en `max_len`. El rodeo se busca en una
// rejilla (A* a 8 vecinos) y se tensa después, uniendo cada punto con el más lejano que se vea en
// recto. La rejilla se limita a la elipse que admite el largo: un rodeo no puede salir de ahí.
static std::vector<Vec2d> skate_route(const Vec2d& a, const Vec2d& b, const ExPolygons& ground,
                                      double max_len, double cell)
{
    const double direct = (b - a).norm();
    if (ground.empty() || direct > max_len)
        return {};
    if (skate_seg_ok(a, b, ground))
        return { a, b };
    if (max_len <= direct * 1.0001)
        return {};

    const double pad = 0.5 * (max_len - direct) + cell;
    double x0 = std::min(a.x(), b.x()) - pad, x1 = std::max(a.x(), b.x()) + pad;
    double y0 = std::min(a.y(), b.y()) - pad, y1 = std::max(a.y(), b.y()) + pad;
    const BoundingBox gb = get_extents(ground);
    x0 = std::max(x0, unscale<double>(gb.min.x())); x1 = std::min(x1, unscale<double>(gb.max.x()));
    y0 = std::max(y0, unscale<double>(gb.min.y())); y1 = std::min(y1, unscale<double>(gb.max.y()));
    if (x1 <= x0 || y1 <= y0)
        return {};
    const int nx = int(std::ceil((x1 - x0) / cell)) + 1;
    const int ny = int(std::ceil((y1 - y0) / cell)) + 1;
    if (int64_t(nx) * int64_t(ny) > 250000)   // una isla gigante no es un caso de patinaje
        return {};

    // Rasterizado por filas: una intersección de Clipper por fila, no una prueba por celda.
    std::vector<char> ok(size_t(nx) * size_t(ny), 0);
    ++ns_probe_astar;
    ns_probe_raster_rows += size_t(ny);
    for (int j = 0; j < ny; ++j) {
        const double y = y0 + j * cell;
        const Polyline row(Point(scaled<coord_t>(x0 - cell), scaled<coord_t>(y)),
                           Point(scaled<coord_t>(x1 + cell), scaled<coord_t>(y)));
        for (const Polyline& s : intersection_pl(Polylines{ row }, ground)) {
            const double xa = unscale<double>(std::min(s.first_point().x(), s.last_point().x()));
            const double xb = unscale<double>(std::max(s.first_point().x(), s.last_point().x()));
            const int ia = std::max(0, int(std::ceil((xa - x0) / cell)));
            const int ib = std::min(nx - 1, int(std::floor((xb - x0) / cell)));
            for (int i = ia; i <= ib; ++i)
                ok[size_t(j) * size_t(nx) + size_t(i)] = 1;
        }
    }
    const auto cell_of = [&](const Vec2d& p) -> int {
        const int ci = int(std::lround((p.x() - x0) / cell));
        const int cj = int(std::lround((p.y() - y0) / cell));
        int best = -1;
        double bd = std::numeric_limits<double>::max();
        for (int dj = -2; dj <= 2; ++dj)
            for (int di = -2; di <= 2; ++di) {
                const int i = ci + di, j = cj + dj;
                if (i < 0 || j < 0 || i >= nx || j >= ny || !ok[size_t(j) * size_t(nx) + size_t(i)])
                    continue;
                const double d = double(di * di + dj * dj);
                if (d < bd) { bd = d; best = j * nx + i; }
            }
        return best;
    };
    const int s = cell_of(a), t = cell_of(b);
    if (s < 0 || t < 0)
        return {};

    const size_t N = size_t(nx) * size_t(ny);
    std::vector<double> gcost(N, std::numeric_limits<double>::max());
    std::vector<int>    prev(N, -1);
    using QItem = std::pair<double, int>;
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> open;
    const int tx = t % nx, ty = t / nx;
    const auto h = [&](int c) {
        const double dx = std::abs(c % nx - tx), dy = std::abs(c / nx - ty);
        return (std::max(dx, dy) + 0.41421356 * std::min(dx, dy)) * cell;
    };
    const double budget = max_len;   // los tramos a y b hasta su celda se añaden al final
    gcost[size_t(s)] = 0.;
    open.emplace(h(s), s);
    while (!open.empty()) {
        const auto [f, c] = open.top();
        open.pop();
        if (c == t)
            break;
        if (f > budget)
            return {};
        const int ci = c % nx, cj = c / nx;
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                if (!di && !dj)
                    continue;
                const int i = ci + di, j = cj + dj;
                if (i < 0 || j < 0 || i >= nx || j >= ny)
                    continue;
                const int n = j * nx + i;
                if (!ok[size_t(n)])
                    continue;
                const double g2 = gcost[size_t(c)] + ((di && dj) ? 1.41421356 : 1.) * cell;
                if (g2 < gcost[size_t(n)]) {
                    gcost[size_t(n)] = g2;
                    prev[size_t(n)] = c;
                    open.emplace(g2 + h(n), n);
                }
            }
    }
    if (prev[size_t(t)] < 0 && s != t)
        return {};

    std::vector<Vec2d> raw;
    for (int c = t; c >= 0; c = prev[size_t(c)]) {
        raw.emplace_back(x0 + (c % nx) * cell, y0 + (c / nx) * cell);
        if (c == s)
            break;
    }
    std::reverse(raw.begin(), raw.end());
    raw.front() = a;
    if (raw.size() == 1)
        raw.push_back(b);
    else
        raw.back() = b;

    // Tensar: desde cada punto, el más lejano que se ve en recto.
    std::vector<Vec2d> route{ raw.front() };
    size_t i = 0;
    while (i + 1 < raw.size()) {
        size_t k = i + 1;
        while (k + 1 < raw.size() && skate_seg_ok(raw[i], raw[k + 1], ground))
            ++k;
        route.push_back(raw[k]);
        i = k;
    }
    double len = 0.;
    for (size_t q = 1; q < route.size(); ++q)
        len += (route[q] - route[q - 1]).norm();
    return len <= max_len ? route : std::vector<Vec2d>{};
}

static void emit_skate(const std::vector<Vec2d>& route, PerimeterGenerator& g, ExtrusionEntitiesPtr& out)
{
    Polyline pl;
    pl.points.reserve(route.size());
    for (const Vec2d& q : route)
        pl.points.emplace_back(scaled<coord_t>(q.x()), scaled<coord_t>(q.y()));
    pl.remove_duplicate_points();
    if (pl.points.size() < 2)
        return;
    // Caudal 0, como el "move inwards" de Orca: el tope volumétrico da infinito y no toca la velocidad.
    ExtrusionPath p(erGapFill, 0., g.solid_infill_flow.width(), float(g.layer_height), true);
    p.polyline = std::move(pl);
    out.emplace_back(new ExtrusionPath(std::move(p)));
}

// NEOTKO_NEOSTROKE_TAG s342 — los NsParams de una config, sacados de `run_neostroke` para poder rehacerlos POR ISLA
// (ajustes por isla: la misma receta sobre una copia de la Config con las claves de la isla pisadas). Sin cambios de
// contenido respecto al bloque de antes: el G-code sin anclas es el mismo byte a byte.
static NsParams make_params(PerimeterGenerator& g, const Config& cfg, const PrintRegionConfig* original_cfg)
{
    NsParams P;
    P.nozzle     = g.print_config->nozzle_diameter.get_at(0);
    P.outer_w    = g.ext_perimeter_flow.spacing();
    // NEOTKO_NEOSTROKE_TAG s336e/g — SOLAPE CONTRA EL MURO. Medido en el TEST19,
    // la B y la E de BEACH: el 95 % del área de hueco está en la JUNTA entre el bucle de Classic y
    // NeoStroke (bolsitas en las esquinas interiores, donde el bucle dobla con su radio). Classic cuenta
    // con su gap-fill para taparlas, pero con NeoStroke el gap-fill va apagado (`gap_infill_speed = 0`
    // abajo), y NeoStroke empezaba EXACTAMENTE donde acaba el muro. Orca le da a su relleno un 15-25 %
    // de solape contra el muro (`infill_wall_overlap`) justo por esto. Aquí: el territorio de NeoStroke se
    // mete ese % del muro por debajo, así que las líneas de borde y las puntas llegan a las bolsas.
    // Sólo mueve el PLAN; lo que se cuenta como cubierto por el muro (`outer_cov`) sigue siendo honesto.
    // 🔑 s336g — UN SOLO MANDO: es el MISMO «Infill/Wall overlap» del perfil (`infill_wall_overlap`), el que
    //    Classic usa para su relleno. TEST20: 20 % cierra el 80 % del hueco con +16 % de plástico (y se ve
    //    en el visor: el ancho crece, no es caudal escondido); 35 % casi no gana más y junta las letras.
    //    Se aplica sobre el ancho del MURO (no sobre el del relleno, como Orca) para que el 20 % de la zona
    //    B del TEST20 se reproduzca tal cual. Sólo mueve el PLAN; `outer_cov` sigue siendo honesto.
    P.outer_w   *= 1. - std::max(0., std::min(50., original_cfg->infill_wall_overlap.value)) / 100.;
    // s331c — la referencia de los porcentajes. 0 = automática: el ancho de línea del relleno
    // macizo, que es el flujo con el que NeoStroke emite. El env var sólo pisa.
    P.w_ref      = cfg.neostroke_width_ref > 1e-6 ? cfg.neostroke_width_ref
                                                  : double(g.solid_infill_flow.width());
    if (ns_ovl_env().has_wref)
        P.w_ref  = ns_ovl_env().wref;
    P.w_ref      = std::max(0.05, std::min(5.0, P.w_ref));
    // s326 — mandos propios. El techo decide k; el suelo nunca por encima del techo.
    P.ceiling_w  = std::max(0.05, cfg.neostroke_max_width_pct / 100.) * P.w_ref;
    P.floor_w    = std::min(P.ceiling_w, std::max(0.01, cfg.neostroke_min_width_pct / 100.) * P.w_ref);
    P.sliver     = P.floor_w;
    // s329 — el cordón más fino que la máquina sabe hacer de verdad. Suelo de los DETALLES (los
    // rellenos del residuo y los caminos cortos), donde antes estaba el diámetro del cabezal.
    P.detail_min = std::min(P.ceiling_w, std::max(P.floor_w,
                            std::max(0.05, cfg.neostroke_detail_min_pct / 100.) * P.w_ref));
    P.corner_hooks = cfg.neostroke_corner_hooks;
    // NEOTKO_NEOSTROKE_TAG s331 — la curva de overlap, resuelta aqui una vez. El mando va POR
    // OBJETO (varios valores en la misma placa); los env var solo PISAN, para barrer la FORMA de
    // la curva sin tocar el 3mf ni volver a compilar.
    P.ovl_pct      = std::max(0.,   std::min(50.,  cfg.neostroke_curve_overlap)) / 100.;
    // El fin de la rampa de ancho se guarda en % del cabezal, igual que el suelo y el techo.
    // s336h — la FORMA de la rampa (ovl_w1/turn0/turn1/span/straight) va fija en NsParams.
    // s331b — tope duro del cordón y ancho máximo de un trazo. El tope va en % del cabezal, como
    // el suelo y el techo; el ancho máximo va en mm, porque lo ancha que es una forma no depende
    // de ningún ajuste.
    P.max_bead     = std::max(0.05, std::min(250., cfg.neostroke_max_bead_pct) / 100. * P.w_ref);
    P.max_stroke_w = std::max(0.5,  std::min(30.,  cfg.neostroke_max_stroke_width));
    P.band         = std::clamp(cfg.neostroke_band_mm, 0., 30.);   // s340
    {   // los env var pisan, uno a uno y solo el que este puesto
        const NsOvlEnv& E = ns_ovl_env();
        if (E.has_pct)  P.ovl_pct      = E.pct / 100.;
        if (E.has_w1)   P.ovl_w1       = E.w1;
        if (E.has_g0)   P.ovl_turn0    = E.g0;
        if (E.has_g1)   P.ovl_turn1    = E.g1;
        if (E.has_span) P.ovl_span     = E.span;
        if (E.has_str)  P.ovl_straight = E.str;
        if (E.has_bead) P.max_bead     = E.bead;
        if (E.has_sw)   P.max_stroke_w = E.sw;
    }
    P.cont_turns   = cfg.neostroke_continuous_turns;   // s336
    P.var_k        = cfg.neostroke_variable_k;
    {
        const NsOvlEnv& E = ns_ovl_env();
        if (E.has_turns) P.cont_turns   = E.turns > 0.5;
        if (E.has_vark)  P.var_k        = E.vark  > 0.5;
    }
    // 🚨 El tope del cordón nunca por debajo del suelo, ni el techo por encima del tope: si el
    // techo pide un cordón que el tope no deja, manda el tope y lo paga k.
    P.max_bead = std::max(P.max_bead, P.floor_w);
    // NEOTKO_NEOSTROKE_TAG s332 — el cordón más fino que la máquina saca DE VERDAD. Va con el
    // CABEZAL y no con `width_ref`: es una propiedad física de la boquilla, no del perfil, y la
    // regla de la casa es que `P.nozzle` se queda sólo para lo físico (s331c). Una 0.4 saca 0.25.
    // 🚨 Se calcula AQUÍ, después de `max_bead`, porque va acotado por él.
    P.bead_min = std::max(0.05, std::min(P.max_bead,
                          cfg.neostroke_bead_min_pct / 100. * P.nozzle));
    // 🚨 La rampa nunca al reves: si el fin queda por debajo del principio, smoothstep se vuelve
    // un escalon en `b` y el mando deja de ser una curva.
    P.ovl_turn1 = std::max(P.ovl_turn0 + 0.1, P.ovl_turn1);
    P.layer_jitter = cfg.neostroke_layer_jitter;   // s332
    P.skate        = cfg.neostroke_skate;
    P.skate_detour = std::max(1.0, std::min(999.0, cfg.neostroke_skate_detour));
    // s339 — TEST25. Los env var pisan para barrer sin tocar el 3mf.
    P.lane_ovl         = std::clamp(cfg.neostroke_lane_overlap, 0., 30.) / 100.;
    P.end_at_junctions = cfg.neostroke_end_at_junctions;
    P.lead_in          = std::clamp(cfg.neostroke_lead_in, 0., 3.);
    {
        double v;
        if (ns_env_get("ORCA_NS_LANE_OVL", 0., 30., v))      P.lane_ovl = v / 100.;
        if (ns_env_get("ORCA_NS_JUNCTIONS", 0., 1., v))      P.end_at_junctions = v > 0.5;
        if (ns_env_get("ORCA_NS_JUNCTION_MM", 0.1, 2., v))   P.junction_mm = v;
        if (ns_env_get("ORCA_NS_LEAD_IN", 0., 3., v))        P.lead_in = v;
        if (ns_env_get("ORCA_NS_LEAD_IN_FLOW", 0., 1.5, v))  P.lead_in_flow = v;
    }
    return P;
}

// s342 — pisa en `c` UNA clave de la lista blanca (`island_override_keys()`). Valor mal escrito = se ignora (false).
static bool apply_island_override(Config& c, const std::string& key, const std::string& val)
{
    // Lo escribe el gizmo con `serialize()` de la opción: un % sale como «15%» y un sí/no como «1»/«0».
    char* end = nullptr;
    const double v = std::strtod(val.c_str(), &end);
    if (end == val.c_str())
        return false;
    while (end && (*end == '%' || *end == ' '))
        ++end;
    if (!end || *end != '\0')
        return false;
    const bool b = v > 0.5;
    if      (key == "neostroke_min_width_pct")    c.neostroke_min_width_pct    = v;
    else if (key == "neostroke_max_width_pct")    c.neostroke_max_width_pct    = v;
    else if (key == "neostroke_detail_min_pct")   c.neostroke_detail_min_pct   = v;
    else if (key == "neostroke_bead_min_pct")     c.neostroke_bead_min_pct     = v;
    else if (key == "neostroke_max_bead_pct")     c.neostroke_max_bead_pct     = v;
    else if (key == "neostroke_max_stroke_width") c.neostroke_max_stroke_width = v;
    else if (key == "neostroke_band_mm")          c.neostroke_band_mm          = v;
    else if (key == "neostroke_curve_overlap")    c.neostroke_curve_overlap    = v;
    else if (key == "neostroke_lane_overlap")     c.neostroke_lane_overlap     = v;
    else if (key == "neostroke_lead_in")          c.neostroke_lead_in          = v;
    else if (key == "neostroke_end_at_junctions") c.neostroke_end_at_junctions = b;
    else if (key == "neostroke_continuous_turns") c.neostroke_continuous_turns = b;
    else if (key == "neostroke_variable_k")       c.neostroke_variable_k       = b;
    else if (key == "neostroke_corner_hooks")     c.neostroke_corner_hooks     = b;
    else return false;
    return true;
}

// NEOTKO_NEOSTROKE_TAG s342e — los contadores `thread_local` de las sondas, para las islas en paralelo. Cada isla
// guarda lo que ELLA sumó en el hilo que la hizo y deja ese hilo como estaba (así da igual qué hilo coja qué isla, y
// el propio hilo que llama también puede hacer islas); el total se suma después en el hilo de la capa.
struct NsCounters {
    size_t probe_seg = 0, probe_rows = 0, probe_astar = 0, ov_runs = 0, ov_touched = 0, cuts = 0, gap_n0 = 0, gap_n1 = 0, gap_moved = 0;
    double t_skel = 0., t_strokes = 0., t_tips = 0., t_residual = 0., t_details = 0., t_stitch_only = 0., t_flow = 0.;
    double ov_in = 0., ov_out = 0., gap_a0 = 0., gap_a1 = 0., gap_mat0 = 0., gap_mat1 = 0.;
    static NsCounters now()
    {
        NsCounters c;
        c.probe_seg = ns_probe_seg_calls; c.probe_rows = ns_probe_raster_rows; c.probe_astar = ns_probe_astar;
        c.ov_runs = ns_ov_runs; c.ov_touched = ns_ov_touched; c.cuts = ns_cuts;
        c.gap_n0 = ns_gap_n0; c.gap_n1 = ns_gap_n1; c.gap_moved = ns_gap_moved;
        c.t_skel = ns_t_skel; c.t_strokes = ns_t_strokes; c.t_tips = ns_t_tips; c.t_residual = ns_t_residual;
        c.t_details = ns_t_details; c.t_stitch_only = ns_t_stitch_only; c.t_flow = ns_t_flow;
        c.ov_in = ns_ov_vol_in; c.ov_out = ns_ov_vol_out;
        c.gap_a0 = ns_gap_a0; c.gap_a1 = ns_gap_a1; c.gap_mat0 = ns_gap_mat0; c.gap_mat1 = ns_gap_mat1;
        return c;
    }
    void set_this_thread() const
    {
        ns_probe_seg_calls = probe_seg; ns_probe_raster_rows = probe_rows; ns_probe_astar = probe_astar;
        ns_ov_runs = ov_runs; ns_ov_touched = ov_touched; ns_cuts = cuts;
        ns_gap_n0 = gap_n0; ns_gap_n1 = gap_n1; ns_gap_moved = gap_moved;
        ns_t_skel = t_skel; ns_t_strokes = t_strokes; ns_t_tips = t_tips; ns_t_residual = t_residual;
        ns_t_details = t_details; ns_t_stitch_only = t_stitch_only; ns_t_flow = t_flow;
        ns_ov_vol_in = ov_in; ns_ov_vol_out = ov_out;
        ns_gap_a0 = gap_a0; ns_gap_a1 = gap_a1; ns_gap_mat0 = gap_mat0; ns_gap_mat1 = gap_mat1;
    }
    // this = b − a
    void diff(const NsCounters& b, const NsCounters& a)
    {
        probe_seg = b.probe_seg - a.probe_seg; probe_rows = b.probe_rows - a.probe_rows; probe_astar = b.probe_astar - a.probe_astar;
        ov_runs = b.ov_runs - a.ov_runs; ov_touched = b.ov_touched - a.ov_touched; cuts = b.cuts - a.cuts;
        gap_n0 = b.gap_n0 - a.gap_n0; gap_n1 = b.gap_n1 - a.gap_n1; gap_moved = b.gap_moved - a.gap_moved;
        t_skel = b.t_skel - a.t_skel; t_strokes = b.t_strokes - a.t_strokes; t_tips = b.t_tips - a.t_tips;
        t_residual = b.t_residual - a.t_residual; t_details = b.t_details - a.t_details;
        t_stitch_only = b.t_stitch_only - a.t_stitch_only; t_flow = b.t_flow - a.t_flow;
        ov_in = b.ov_in - a.ov_in; ov_out = b.ov_out - a.ov_out;
        gap_a0 = b.gap_a0 - a.gap_a0; gap_a1 = b.gap_a1 - a.gap_a1; gap_mat0 = b.gap_mat0 - a.gap_mat0; gap_mat1 = b.gap_mat1 - a.gap_mat1;
    }
    void add_to_this_thread() const
    {
        ns_probe_seg_calls += probe_seg; ns_probe_raster_rows += probe_rows; ns_probe_astar += probe_astar;
        ns_ov_runs += ov_runs; ns_ov_touched += ov_touched; ns_cuts += cuts;
        ns_gap_n0 += gap_n0; ns_gap_n1 += gap_n1; ns_gap_moved += gap_moved;
        ns_t_skel += t_skel; ns_t_strokes += t_strokes; ns_t_tips += t_tips; ns_t_residual += t_residual;
        ns_t_details += t_details; ns_t_stitch_only += t_stitch_only; ns_t_flow += t_flow;
        ns_ov_vol_in += ov_in; ns_ov_vol_out += ov_out;
        ns_gap_a0 += gap_a0; ns_gap_a1 += gap_a1; ns_gap_mat0 += gap_mat0; ns_gap_mat1 += gap_mat1;
    }
};
// Al empezar la isla: foto del hilo y el interruptor de la sonda de huecos (sólo lo pone el hilo de la capa). Al
// acabar (también por un `return` temprano): lo sumado va a `out` y el hilo vuelve a como estaba.
struct NsCounterGuard {
    NsCounters& out;
    NsCounters  start;
    bool        gap_dbg_before;
    NsCounterGuard(NsCounters& o, bool gap_dbg) : out(o), start(NsCounters::now()), gap_dbg_before(ns_gap_dbg) { ns_gap_dbg = gap_dbg; }
    ~NsCounterGuard()
    {
        out.diff(NsCounters::now(), start);
        start.set_this_thread();
        ns_gap_dbg = gap_dbg_before;
    }
};

// ── C1-C5: el despacho de una capa ──────────────────────────────────────────
void run_neostroke(PerimeterGenerator& g, const Config& cfg, const PrintRegionConfig* original_cfg)
{
    // Sonda de tiempos (s326): ms por fase, una línea [NS-T] por capa en DISPATCH.
    using ns_clk = std::chrono::steady_clock;
    const auto ns_ms = [](ns_clk::time_point a) { return std::chrono::duration<double, std::milli>(ns_clk::now() - a).count(); };
    const ns_clk::time_point t_start = ns_clk::now();
    double t_classic = 0., t_plan = 0., t_stitch = 0., t_order = 0., t_ground = 0., t_route = 0., t_emit = 0., t_cover = 0., t_fill = 0.;
    ns_probe_seg_calls = ns_probe_raster_rows = ns_probe_astar = 0;
    ns_t_skel = ns_t_strokes = ns_t_tips = ns_t_residual = ns_t_details = ns_t_stitch_only = ns_t_flow = 0.;
    ns_ov_runs = ns_ov_touched = 0;   // s331 — la sonda de la curva de overlap, por laminado
    ns_ov_vol_in = ns_ov_vol_out = 0.;
    ns_cuts = 0;                      // s336
    ns_gap_n0 = ns_gap_n1 = ns_gap_moved = 0;   // s338 — el cierre de huecos
    ns_gap_a0 = ns_gap_a1 = ns_gap_mat0 = ns_gap_mat1 = 0.;
    ns_gap_dbg = NeoDebug::enabled(NeoDebug::NEOSTROKE);
    int walls = original_cfg->wall_loops.value;
    if (walls < 1) {
        g.process_classic();
        return;
    }

    // 1. Muro exterior Classic, y sólo el exterior. Igual que la v3 (run_classic_spine).
    PrintRegionConfig modified_cfg = *original_cfg;
    modified_cfg.wall_loops.value           = 1;
    std::fill(modified_cfg.gap_infill_speed.values.begin(), modified_cfg.gap_infill_speed.values.end(), 0.); // Upstream Snapmaker #794: todas las variantes
    // (el interior por trazos sustituye al gap-fill)
    modified_cfg.alternate_extra_wall.value = false;

    ExPolygons original_slice;
    original_slice.reserve(g.slices->surfaces.size());
    for (const Surface& s : g.slices->surfaces)
        original_slice.push_back(s.expolygon);

    const size_t loops_before = g.loops->entities.size();
    g.config = &modified_cfg;
    { const auto t0 = ns_clk::now(); g.process_classic(); t_classic = ns_ms(t0); }
    g.config = original_cfg;
    size_t loops_after_outer = g.loops->entities.size();

    // Lo que tapa el muro exterior, medido por su separación (como lo mide Classic). Sin esto, ni
    // el recorte del relleno ni la sonda saben lo que ya está puesto.
    Polygons outer_cov;
    for (size_t i = loops_before; i < loops_after_outer; ++i)
        g.loops->entities[i]->polygons_covered_by_spacing(outer_cov, float(SCALED_EPSILON));
    const ExPolygons outer_cov_ex = union_ex(outer_cov);

    // 2. Parámetros. El muro exterior se come una banda medida como la mide Classic (su
    //    separación): es lo que hay que descontar para saber el hueco útil.
    const NsParams P = make_params(g, cfg, original_cfg);
    SkeletonParams skp;               // los de C1, verificados contra el prototipo
    if (P.corner_hooks)
        skp.prune_mm = P.hook_prune;  // sin esto las ramitas de esquina ni existen

    // 3. Isla a isla, y dentro de la isla los muros primero (trampa 7). El cubo por isla es lo
    //    que impide que la impresora salte de letra en letra a medio rellenar una.
    const ExPolygons islands = union_ex(original_slice);
    std::vector<ExtrusionEntityCollection> per_island(islands.size());

    // NEOTKO_NEOSTROKE_TAG s342 — AJUSTES POR ISLA (NeoStrokeIslands.hpp). Cada ancla es un punto en coordenadas del
    // OBJETO; se pasa al marco de laminado (`g.ns_obj_to_slice` = XY de `trafo_centered()`) y la isla que lo CONTIENE
    // en ESTA capa usa sus ajustes. Dos anclas en la misma isla (letras que se juntan en alguna capa): gana la
    // PRIMERA de la lista. Ancla que no cae en ninguna isla (la letra se estrecha, curva fuerte): la capa usa los del
    // objeto. Sin anclas, `island_P` queda vacío y todo sale con `P`: el G-code de antes, byte a byte.
    std::vector<NsParams>       island_P;
    std::vector<SkeletonParams> island_skp;
    std::vector<int>            island_anchor(islands.size(), -1);
    if (!cfg.neostroke_island_overrides.empty() && !islands.empty()) {
        size_t skipped = 0;
        const std::vector<NsIslandOverride> anchors = parse_island_overrides(cfg.neostroke_island_overrides, &skipped);
        const double* T = g.ns_obj_to_slice;
        size_t lost = 0, shared = 0;
        for (size_t a = 0; a < anchors.size(); ++a) {
            const double sx = T[0] * anchors[a].x + T[1] * anchors[a].y + T[2];
            const double sy = T[3] * anchors[a].x + T[4] * anchors[a].y + T[5];
            const Point  pt(scaled<coord_t>(sx), scaled<coord_t>(sy));
            int hit = -1;
            for (size_t k = 0; k < islands.size() && hit < 0; ++k)
                if (islands[k].contains(pt))
                    hit = int(k);
            const char* what = "";
            if (hit < 0) {
                ++lost;
                what = " FUERA (la capa usa los del objeto)";
            } else if (island_anchor[hit] >= 0) {
                ++shared;
                what = " COMPARTE isla con otra ancla (gana la primera)";
            } else
                island_anchor[hit] = int(a);
            if (NeoDebug::enabled(NeoDebug::NEOSTROKE)) {
                char ab[320];
                snprintf(ab, sizeof(ab), "[NS] L%d ancla %zu «%s» obj(%.3f,%.3f) -> slice(%.3f,%.3f) isla=%d claves=%zu%s",
                         g.layer_id, a, anchors[a].name.c_str(), anchors[a].x, anchors[a].y, sx, sy, hit,
                         anchors[a].values.size(), what);
                NeoDebug::write(NeoDebug::NEOSTROKE, ab);
            }
        }
        if (skipped > 0 && NeoDebug::enabled(NeoDebug::NEOSTROKE)) {
            char ab[120];
            snprintf(ab, sizeof(ab), "[NS] L%d anclas: %zu entradas o claves descartadas al leer", g.layer_id, skipped);
            NeoDebug::write(NeoDebug::NEOSTROKE, ab);
        }
        (void)lost; (void)shared;
        island_P.assign(islands.size(), P);
        island_skp.assign(islands.size(), SkeletonParams{});
        for (size_t k = 0; k < islands.size(); ++k) {
            island_skp[k] = skp;
            if (island_anchor[k] < 0)
                continue;
            island_skp[k] = SkeletonParams{};   // la poda depende de los ganchos, que la isla puede cambiar
            Config ci = cfg;
            for (const auto& kv : anchors[island_anchor[k]].values)
                apply_island_override(ci, kv.first, kv.second);
            island_P[k] = make_params(g, ci, original_cfg);
            if (island_P[k].corner_hooks)
                island_skp[k].prune_mm = island_P[k].hook_prune;
        }
    }
    const auto params_of = [&](size_t k) -> const NsParams& { return k < island_P.size() ? island_P[k] : P; };
    const auto skel_of   = [&](size_t k) -> const SkeletonParams& { return k < island_skp.size() ? island_skp[k] : skp; };
    Polygons covered_all;
    StrokeStats  total;
    size_t n_paths = 0, n_lines = 0;
    double emitted_mm = 0.;
    double cov_sum    = 0.;   // s329 — suma de las huellas camino a camino, SIN unir

    // NEOTKO_NEOSTROKE_TAG s335 — canal PROPIO. Hasta s334 la sonda `[NS]` se colaba por DISPATCH,
    // que es el trazado genérico de `extrude_entity`: encenderlo para leer una línea de NeoStroke
    // abría un grifo que no tenía nada que ver. Y ahora además este canal es la llave de
    // depuración del generador, así que quien lo enciende para usarlo ya tiene el log.
    const bool dbg = NeoDebug::enabled(NeoDebug::NEOSTROKE);
    // C6: el muro exterior sólo es suelo para patinar si se imprime ANTES que el interior.
    const bool outer_first = original_cfg->wall_sequence.value == WallSequence::OuterInner;
    // NEOTKO_NEOSTROKE_TAG s342e — LAS ISLAS EN PARALELO. Cada isla se planifica, se ordena y se emite sin mirar a
    // las demás (su cubo `per_island[i]`, su suelo de patín, su cabeza), así que el trabajo va en hilos y lo común
    // (huellas, sumas, tiempos, líneas del log) se guarda en `IslandOut` y se junta DESPUÉS, en el orden de siempre:
    // el G-code sale igual byte a byte. Los contadores `thread_local` de las sondas se restauran en el hilo que
    // hizo la isla y se suman aquí (ver `NsCounters`). `ORCA_NS_SERIAL` = una isla detrás de otra, para comparar.
    struct IslandOut {
        Polygons                 covered;
        double                   cov_sum = 0., emitted_mm = 0.;
        size_t                   n_paths = 0, n_lines = 0;
        double                   t_plan = 0., t_stitch = 0., t_order = 0., t_ground = 0., t_route = 0., t_emit = 0., t_cover = 0.;
        StrokeStats              total;
        NsCounters               cnt;
        std::vector<std::string> log;
    };
    std::vector<IslandOut> island_out(islands.size());
    const bool gap_dbg_main = ns_gap_dbg;
    const auto process_island = [&](size_t i) {
        IslandOut&     io = island_out[i];
        NsCounterGuard guard(io.cnt, gap_dbg_main);
        // s342 — ajustes por isla: dentro del bucle `P` y `skp` son los de ESTA isla (los del objeto si no tiene ancla).
        const NsParams&       P   = params_of(i);
        const SkeletonParams& skp = skel_of(i);
        const ExPolygon& island = islands[i];
        StrokeStats   st;
        SkeletonStats sk_st;
        // Sólo el trozo de muro de ESTA isla: lo demás sobra en cada resta.
        const Polygons island_outer = to_polygons(intersection_ex(outer_cov_ex, ExPolygons{ island }));
        // 🔎 s340 SONDA — visor y laminado dan la MISMA isla por área/celdas y aun así distinto plan (la A de
        //    NeoStroke-TEST: pares=2 en el laminado, 0 en el visor). Vuelca la isla y el muro EXACTOS de la capa
        //    `ORCA_NS_DUMP_ISLAND=<layer_id>` para compararlos punto a punto. Sólo con el canal NEOSTROKE.
        if (dbg) {
            static const int dump_layer = std::getenv("ORCA_NS_DUMP_ISLAND") ? std::atoi(std::getenv("ORCA_NS_DUMP_ISLAND")) : -1;
            if (dump_layer == g.layer_id) {
                auto dump_poly = [&](const char* tag, const Polygon& pg) {
                    std::string s = std::string("[NS-ISLA] ") + tag + " n=" + std::to_string(pg.points.size()) + " :";
                    char pb[48];
                    for (const Point& q : pg.points) {
                        snprintf(pb, sizeof(pb), " %.4f,%.4f", unscale<double>(q.x()), unscale<double>(q.y()));
                        s += pb;
                    }
                    io.log.emplace_back(s.c_str());
                };
                char hb[160];
                snprintf(hb, sizeof(hb), "[NS-ISLA] L%d isla %zu area=%.5f holes=%zu muro_polys=%zu outer_w(plan)=%.4f",
                         g.layer_id, i, island.area() * SCALING_FACTOR * SCALING_FACTOR, island.holes.size(),
                         island_outer.size(), P.outer_w);
                io.log.emplace_back(hb);
                dump_poly("contour", island.contour);
                for (const Polygon& h : island.holes)
                    dump_poly("hole", h);
                for (const Polygon& m : island_outer)
                    dump_poly("muro", m);
            }
        }
        auto t0 = ns_clk::now();
        // NEOTKO_NEOSTROKE_TAG s338 — con `ORCA_NS_FIELD` los caminos salen del CAMPO, ya cosidos. Si la isla
        // es demasiado grande para la rejilla (fs.skipped), se cae al planificador de trazos de siempre.
        std::vector<NsPath> paths;
        size_t     island_lines = 0;
        FieldStats fs;
        bool       by_field = false;
        if (ns_field_on()) {
            paths    = plan_island_field(island, island_outer, P, skp, st, fs);
            // 🚨 s338 (auditoría) — sin caminos (isla muy fina, sin eje) = que lo intente el planificador viejo,
            //    como con las islas grandes. Si no, esa isla se quedaba SIN interior.
            by_field = !fs.skipped && !paths.empty();
            island_lines = paths.size();
            if (dbg) {
                char fb[420];
                snprintf(fb, sizeof(fb),
                         "[NS] L%d isla %zu/%zu CAMPO%s celdas=%zu pares=%zu centro=%zu podadas=%zu colas=%zu juntas=%zu empalmes=%zu tramos=%zu girados=%zu cruces=%zu"
                         " | eje %.1f campo %.1f carriles %.1f remate %.1f costura %.1f = %.1f ms",
                         g.layer_id, i, islands.size(), fs.skipped ? " (pieza/grande: trazos)" : (paths.empty() ? " (vacío: trazos)" : ""), fs.cells, fs.pair_lanes,
                         fs.center_lanes, fs.pruned, fs.tails_cut, fs.joints, fs.splices, fs.tramos, fs.wide_starts, fs.junction_ends,
                         fs.t_eje, fs.t_campo, fs.t_carriles, fs.t_remate, fs.t_costura, ns_ms(t0));
                io.log.emplace_back(fb);
            }
        }
        const size_t cuts_before = ns_cuts;   // s336
        const ExPolygons region{ island };
        if (!by_field) {
            NsLines lines = plan_island(island, island_outer, P, skp, st, &sk_st);
            io.t_plan += ns_ms(t0);
            if (lines.empty())
                return;
            island_lines = lines.size();
            t0 = ns_clk::now();
            paths = stitch(std::move(lines), region, P);
            ns_t_stitch_only += ns_ms(t0);
        } else {
            io.t_plan += ns_ms(t0);
            if (paths.empty())
                return;
            t0 = ns_clk::now();
        }
        io.n_lines += island_lines;
        const auto t_fr = ns_clk::now();
        size_t dropped = 0;
        // 🚨 s338 — los caminos del CAMPO no pasan por la regla de detalles. Esa regla se hizo para los restos sueltos
        //    del planificador de trazos: un camino de menos de `cont_len` (2 mm) se ensancha y se TIRA si pisa más de
        //    un 35 %. En el campo un camino corto es un carril planificado con su sitio exacto entre vecinos (el
        //    central de un palo de 1.2 mm): en el TEST23a se tiraba y dejaba la franja vacía (la H de HÔTEL, el
        //    brazo de la y; `detalles_fuera=1-2` justo en esas copias). El campo ya pone sus límites: anchos entre
        //    mínimo y máximo, colas recortadas y remate.
        if (!by_field)
            paths = flow_rules(std::move(paths), island, P, dropped);
        ns_t_flow += ns_ms(t_fr);
        io.t_stitch += ns_ms(t0);
        st.dropped_detail += dropped;
        // NEOTKO_NEOSTROKE_TAG s335 — `wall_paths` sale SIEMPRE vacío desde que NeoWall se fue: no
        // hay camino `Kind::Outer`. Se conserva el reparto porque es la pieza que respeta
        // `wall_sequence` si vuelve un muro propio — un muro se imprime de una vez, fuera del
        // vecino-más-cercano, que si no lo trocearía entre trazos y añadiría justo las costuras
        // que NeoStroke existe para no tener.
        std::vector<NsPath> wall_paths;
        {
            std::vector<NsPath> inner;
            inner.reserve(paths.size());
            for (NsPath& pp : paths) {
                if (!pp.empty() && pp.front().kind == Kind::Outer)
                    wall_paths.push_back(std::move(pp));
                else
                    inner.push_back(std::move(pp));
            }
            paths = std::move(inner);
        }
        io.n_paths += paths.size() + wall_paths.size();

        // Orden dentro de la isla: al vecino más cercano, empezando por el camino más largo.
        // 🚨 Aquí es donde engancha C6 (NeoStrokeLink.hpp): hoy cada salto es un viaje y el
        //    planificador de enlaces está vacío, pero el contexto que necesita ya se monta.
        std::vector<char> left(paths.size(), 1);
        size_t            remaining = paths.size();
        Vec2d             head      = Vec2d::Zero();
        bool              has_head  = false;
        // NEOTKO_NEOSTROKE_TAG s332 — sin esto el orden es 100 % determinista (empieza SIEMPRE por
        // el camino más largo y de ahí al vecino más cercano), así que las capas salen calcadas y
        // los cortes de flujo taladran en vertical. Ver el comentario de `layer_jitter` en NsParams.
        // El ancla orbita el centro de la isla con el ÁNGULO ÁUREO (2.39996 rad ≈ 137.5°): no se
        // repite nunca, a diferencia de rotar por las cuatro esquinas, que volvería a alinearse
        // cada cuatro capas — y cuatro capas es justo el espesor de estas letras.
        // 🚨 Sólo cambia POR DÓNDE SE EMPIEZA y en qué orden se recorre. No toca el reparto de `k`,
        //    ni el ancho, ni el caudal: el material puesto es exactamente el mismo.
        // 🚨 NO se puede hacer poniendo `has_head = true`: el planificador de enlaces (`link_planner`)
        //    y el patín (`P.skate`) miran ESE mismo `has_head` para saber si vienen de un cordón ya
        //    puesto. Con el ancla ahí dentro intentarían enlazar y patinar DESDE UN PUNTO QUE ESTÁ
        //    FUERA DE LA ISLA. El ancla es otra cosa y va en su propia variable: sólo elige por
        //    dónde se empieza, y desaparece en cuanto hay un `head` de verdad.
        Vec2d anchor     = Vec2d::Zero();
        bool  has_anchor = false;
        if (P.layer_jitter && !paths.empty()) {
            const BoundingBox bb = island.contour.bounding_box();
            const Vec2d c(unscale<double>(bb.center().x()), unscale<double>(bb.center().y()));
            const double r = 0.5 * std::hypot(unscale<double>(bb.size().x()),
                                              unscale<double>(bb.size().y())) + 1.0;
            const double ang = 2.399963229728653 * double(std::max(0, g.layer_id));
            anchor     = c + Vec2d(std::cos(ang), std::sin(ang)) * r;
            has_anchor = true;
        }
        ExPolygons        covered_island;   // sólo para el ancla (hoy sin planificador): no se rellena
        CoverIndex        covered_idx;      // lo YA puesto en esta isla, para el suelo del patín
        std::vector<Vec2d> last_bead;
        size_t skates_straight = 0, skates_routed = 0, skate_travels = 0;   // C6
        // Emitir y apuntar lo que cubre. La contabilidad se mide sobre los caminos FINALES, no sobre
        // las líneas planificadas (trampa 8).
        const auto emit_and_account = [&](NsPath& path) {
            auto t_e = ns_clk::now();
            emit_path(path, g, per_island[i].entities, io.emitted_mm, P);
            io.t_emit += ns_ms(t_e);
            t_e = ns_clk::now();
            Polygons mine;
            for (const NsLine& l : path)
                append(mine, line_poly(l));
            append(io.covered, mine);
            const ExPolygons mine_u = union_ex(mine);
            io.cov_sum += area_mm2(mine_u);
            covered_idx.add(mine_u);
            io.t_cover += ns_ms(t_e);
        };
        // s333 — 🚨 el muro NO toca `head`: quien elige por dónde empieza el interior es el ancla de
        //    capa, y con `head` puesto el interior arrancaría siempre pegado al final del muro, o sea
        //    calcado capa a capa — justo lo que `layer_jitter` viene a romper. Con el muro de Classic
        //    tampoco había `head`, porque se emite fuera de este cubo.
        if (outer_first)
            for (NsPath& wp : wall_paths)
                emit_and_account(wp);
        while (remaining > 0) {
            auto t_sel = ns_clk::now();
            size_t best = 0;
            bool   best_rev = false;
            double bd = std::numeric_limits<double>::max();
            for (size_t j = 0; j < paths.size(); ++j) {
                if (!left[j])
                    continue;
                const std::vector<Vec2d> pl = path_polyline(paths[j]);
                if (pl.empty())
                    continue;
                for (int side = 0; side < 2; ++side) {
                    // s338 — un camino con el sentido fijado (arranca por su lado gordo) no se da la vuelta.
                    if (side == 1 && !paths[j].empty() && paths[j].front().fixed_dir)
                        continue;
                    // s332 — sin `head` todavía: si hay ancla de capa, gana el camino más cercano a
                    // ella (y así el arranque cambia capa a capa); si no, el más largo, como siempre.
                    const Vec2d  p = side ? pl.back() : pl.front();
                    const double d = has_head   ? (head - p).norm()
                                   : has_anchor ? (anchor - p).norm()
                                                : -poly_len(pl);
                    if (d < bd) {
                        bd = d;
                        best = j;
                        best_rev = (side == 1);
                    }
                }
            }
            left[best] = 0;
            --remaining;
            if (best_rev)
                reverse_path(paths[best]);
            const std::vector<Vec2d> pl = path_polyline(paths[best]);
            if (pl.empty())
                continue;
            io.t_order += ns_ms(t_sel);
            if (has_head && link_planner()) {
                LinkContext lc;
                const Polyline bead = to_polyline(last_bead);
                lc.island = &island;
                lc.covered = &covered_island;
                lc.last_bead = &bead;
                lc.from = Point(scaled<coord_t>(head.x()), scaled<coord_t>(head.y()));
                lc.to   = Point(scaled<coord_t>(pl.front().x()), scaled<coord_t>(pl.front().y()));
                lc.nozzle_mm = P.nozzle;
                lc.outer_w_mm = P.outer_w;
                link_planner()(lc);   // C6: hoy nadie responde y todo sale como un viaje
            }
            // C6 — patinaje. Suelo = lo que YA está puesto en esta isla (menos un cuarto de cabezal,
            // para ir por encima del cordón y no por su borde), más el muro exterior si se imprimió
            // antes (sólo con outer:inner), más un cuadrado de un cabezal en el arranque del
            // siguiente camino, que todavía no está puesto pero es donde se va a extruir ya.
            if (P.skate && has_head) {
                const Vec2d  to     = pl.front();
                const double direct = (to - head).norm();
                if (direct > 0.01) {
                    const double max_len = P.skate_detour * direct;
                    // El suelo dentro de una ventana alrededor del salto. 🚨 En dos pasos (s326):
                    // primero una ventana ESTRECHA, que basta para el patín recto (a un cabezal del
                    // segmento el recorte de la ventana no llega a tocarlo); la ancha, la del rodeo,
                    // sólo si el recto no vale. Con detour 10 en el marco la ancha era la pieza entera.
                    const auto build_ground = [&](double pad) {
                        BoundingBox win(Point(scaled<coord_t>(std::min(head.x(), to.x()) - pad), scaled<coord_t>(std::min(head.y(), to.y()) - pad)),
                                        Point(scaled<coord_t>(std::max(head.x(), to.x()) + pad), scaled<coord_t>(std::max(head.y(), to.y()) + pad)));
                        const Polygons win_poly{ win.polygon() };
                        Polygons base = covered_idx.empty() ? Polygons{}
                                                            : to_polygons(intersection_ex(covered_idx.near(win), win_poly));
                        if (outer_first)
                            append(base, intersection(island_outer, win_poly));
                        ExPolygons gr = offset_ex(union_ex(base), -float(scaled<double>(0.25 * P.nozzle)));
                        const coord_t half = scaled<coord_t>(0.5 * P.nozzle);
                        for (const Vec2d& q : { head, to }) {
                            const Point c(scaled<coord_t>(q.x()), scaled<coord_t>(q.y()));
                            Polygon sq{ Point(c.x() - half, c.y() - half), Point(c.x() + half, c.y() - half),
                                        Point(c.x() + half, c.y() + half), Point(c.x() - half, c.y() + half) };
                            gr = union_ex(gr, ExPolygons{ ExPolygon(sq) });
                        }
                        return intersection_ex(gr, intersection_ex(ExPolygons{ island }, win_poly));
                    };
                    auto t_g = ns_clk::now();
                    ExPolygons ground = build_ground(P.nozzle);
                    io.t_ground += ns_ms(t_g);
                    std::vector<Vec2d> route;
                    auto t_r = ns_clk::now();
                    if (skate_seg_ok(head, to, ground))
                        route = { head, to };
                    io.t_route += ns_ms(t_r);
                    if (route.empty() && max_len > direct * 1.0001) {
                        t_g = ns_clk::now();
                        ground = build_ground(0.5 * (max_len - direct) + P.nozzle);
                        io.t_ground += ns_ms(t_g);
                        t_r = ns_clk::now();
                        route = skate_route(head, to, ground, max_len, 0.25 * P.nozzle);
                        io.t_route += ns_ms(t_r);
                    }
                    if (route.size() >= 2) {
                        emit_skate(route, g, per_island[i].entities);
                        ++(route.size() == 2 ? skates_straight : skates_routed);
                    } else {
                        ++skate_travels;
                    }
                }
            }
            // Lo que cubre este camino entra en la contabilidad de `emit_and_account`: la regla de
            // caudal tira detalles después de coser, y medirlos antes los contaba como puestos.
            emit_and_account(paths[best]);
            head = pl.back();
            has_head = true;
            last_bead = pl;
        }
        if (!outer_first)
            for (NsPath& wp : wall_paths)
                emit_and_account(wp);
        // s342f — la huella de la isla, YA unida, en su hilo. Luego la capa junta 15 formas limpias en vez de las miles
        // de huellas sueltas de sus líneas (esa unión única se comía 1.6 s en el relleno y otra igual en las bolsitas).
        // Mismo territorio: sólo cambia CUÁNDO se une.
        io.covered = to_polygons(union_ex(io.covered));
        io.total.strokes += st.strokes;
        io.total.slivers += st.slivers;
        io.total.fills   += st.fills;
        io.total.too_wide += st.too_wide;
        io.total.dropped_detail += st.dropped_detail;
        io.total.repainted += st.repainted;
        io.total.caps += st.caps;
        io.total.thin += st.thin;       // s336
        io.total.thin_of += st.thin_of;
        for (const auto& kv : st.k_hist)
            io.total.k_hist[kv.first] += kv.second;

        if (dbg) {
            std::string kh;
            for (const auto& kv : st.k_hist)
                kh += (kh.empty() ? "" : " ") + std::to_string(kv.first) + "x" + std::to_string(kv.second);
            const BoundingBox bb = get_extents(island);
            char buf[768];
            snprintf(buf, sizeof(buf),
                     "[NS] L%d isla %zu/%zu %.2fx%.2f mm area=%.2f | ramas=%zu trazos=%zu k=[%s]"
                     " grietas=%zu rellenos=%zu repintadas=%zu tapas=%zu | lineas=%zu caminos=%zu | patin recto=%zu rodeo=%zu viaje=%zu"
                     " | giros=%d kvar=%d cortes=%zu finos=%zu/%zu",
                     g.layer_id, i, islands.size(), unscale<double>(bb.size().x()), unscale<double>(bb.size().y()),
                     island.area() * SCALING_FACTOR * SCALING_FACTOR, sk_st.branches, st.strokes,
                     kh.c_str(), st.slivers, st.fills, st.repainted, st.caps, island_lines, paths.size(),
                     skates_straight, skates_routed, skate_travels,
                     int(P.cont_turns), int(P.var_k), ns_cuts - cuts_before, st.thin, st.thin_of);
            io.log.emplace_back(buf);
        }
    };
    {
        static const bool serial = std::getenv("ORCA_NS_SERIAL") != nullptr;
        if (serial || islands.size() < 2) {
            for (size_t i = 0; i < islands.size(); ++i)
                process_island(i);
        } else {
            // 🚨 `isolate`: mientras espera, este hilo no puede coger OTRA capa del laminado (el bucle de capas de
            //    Orca también es tbb); si la cogiera, su `run_neostroke` pondría a cero los contadores de ÉSTA.
            tbb::this_task_arena::isolate([&] {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, islands.size(), 1),
                                  [&](const tbb::blocked_range<size_t>& r) {
                                      for (size_t i = r.begin(); i != r.end(); ++i)
                                          process_island(i);
                                  });
            });
        }
    }
    for (IslandOut& io : island_out) {   // lo común, en el orden de las islas
        append(covered_all, std::move(io.covered));
        cov_sum    += io.cov_sum;
        emitted_mm += io.emitted_mm;
        n_paths    += io.n_paths;
        n_lines    += io.n_lines;
        t_plan += io.t_plan; t_stitch += io.t_stitch; t_order += io.t_order; t_ground += io.t_ground;
        t_route += io.t_route; t_emit += io.t_emit; t_cover += io.t_cover;
        total.strokes += io.total.strokes;
        total.slivers += io.total.slivers;
        total.fills   += io.total.fills;
        total.too_wide += io.total.too_wide;
        total.dropped_detail += io.total.dropped_detail;
        total.repainted += io.total.repainted;
        total.caps += io.total.caps;
        total.thin += io.total.thin;
        total.thin_of += io.total.thin_of;
        for (const auto& kv : io.total.k_hist)
            total.k_hist[kv.first] += kv.second;
        io.cnt.add_to_this_thread();
        for (const std::string& l : io.log)
            NeoDebug::write(NeoDebug::NEOSTROKE, l.c_str());
    }

    // NEOTKO_NEOSTROKE_TAG s337b — LAS BOLSITAS LAS RELLENA NEOSTROKE, NO EL RELLENO.
    // Lo que los trazos no cubren y admite un cordón se cedía al relleno normal (paso 4, abajo). En una pieza ancha
    // eso es el centro y está bien. En letras son bolsitas pequeñas de forma rara donde el relleno macizo no mete ni
    // una línea: en el G-code del logotipo del TEST23 no hay NI UNA línea de relleno, y la P de PLAGE salía con un
    // agujero grande en la junta del palo con la panza (Neotko: «es raro generar fallos y era hueco grande»).
    // Criterio con un mando que YA existe: lo más estrecho que `max_stroke_w` (*Widest shape handled*) es un trazo y
    // es de NeoStroke; lo más ancho sigue siendo relleno. Se rellena con ANILLOS concéntricos al paso del relleno
    // macizo, de fuera adentro: un bucle cerrado por vuelta = un solo arranque cada uno.
    // 🚨 Va ANTES del paso 4 y apunta lo que cubre en `covered_all`, para que el paso 4 ya no ceda esas bolsitas.
    size_t pockets = 0, pocket_loops = 0;
    {
        Polygons all_cov = covered_all;
        append(all_cov, outer_cov);
        const ExPolygons covered_now = union_ex(all_cov);
        // El MISMO criterio de «cabe un cordón» que el paso 4, para que los dos no puedan discrepar.
        const float      half      = float(scaled<double>(P.w_ref / 2.));
        const ExPolygons left_over = offset_ex(offset_ex(diff_ex(original_slice, covered_now), -half), half);
        const Flow&      fl        = g.solid_infill_flow;
        const float      sp        = float(fl.scaled_spacing());
        // 🚨 s340 — en MODO BANDA el centro que deja la banda ES relleno: sólo es bolsita lo más estrecho que la banda.
        // s342 — la banda y el ancho máximo son de la ISLA de cada bolsita (ajustes por isla): se busca la isla ANTES.
        const auto wide_half_of = [&](const NsParams& Q) {
            return float(scaled<double>((Q.band > 0. ? std::min(Q.max_stroke_w, Q.band) : Q.max_stroke_w) / 2.));
        };
        if (sp > 0.f)
            for (const ExPolygon& pk : left_over) {
                size_t isl = 0;
                for (size_t k = 0; k < islands.size(); ++k)
                    if (islands[k].contains(pk.contour.points.front()) || islands[k].contains(pk.contour.centroid())) {
                        isl = k;
                        break;
                    }
                if (!offset_ex(pk, -wide_half_of(params_of(isl))).empty())
                    continue;   // más ancha que un trazo: es relleno de verdad
                ExtrusionEntityCollection coll;
                coll.no_sort = true;
                ExPolygons ring = offset_ex(pk, -0.5f * sp);
                for (int guard = 0; !ring.empty() && guard < 200; ++guard) {
                    for (const ExPolygon& r : ring)
                        for (const Polygon& poly : to_polygons(r)) {
                            if (poly.length() < scaled<double>(0.3))
                                continue;
                            ExtrusionPath path(erPerimeter, fl.mm3_per_mm(), fl.width(), fl.height());
                            path.polyline = poly.split_at_first_point();
                            coll.append(ExtrusionLoop(std::move(path)));
                            ++pocket_loops;
                        }
                    ring = offset_ex(ring, -sp);
                }
                if (coll.empty())
                    continue;
                append(covered_all, to_polygons(pk));   // la bolsita ya es de NeoStroke
                if (isl < per_island.size())
                    per_island[isl].append(coll);
                ++pockets;
            }
        if (dbg && ns_gap_close_on()) {   // s338 — huecos estrechos con la huella REAL, antes/después
            char gbuf[240];
            snprintf(gbuf, sizeof(gbuf),
                     "[NS] L%d cierre de huecos: antes=%zu (%.3f mm2) despues=%zu (%.3f mm2) lineas_tocadas=%zu material %+.1f%%",
                     g.layer_id, ns_gap_n0, ns_gap_a0, ns_gap_n1, ns_gap_a1, ns_gap_moved,
                     ns_gap_mat0 > 1e-9 ? 100. * (ns_gap_mat1 - ns_gap_mat0) / ns_gap_mat0 : 0.);
            NeoDebug::write(NeoDebug::NEOSTROKE, gbuf);
        }
        if (dbg && pockets > 0) {
            char buf[160];
            snprintf(buf, sizeof(buf), "[NS] L%d bolsitas rellenas por NeoStroke=%zu anillos=%zu", g.layer_id, pockets, pocket_loops);
            NeoDebug::write(NeoDebug::NEOSTROKE, buf);
        }
    }

    for (ExtrusionEntityCollection& bucket : per_island)
        if (!bucket.empty()) {
            bucket.no_sort = true;   // el orden lo hemos decidido nosotros
            g.loops->append(bucket);
        }

    // Igual que la v3: nada de lo emitido aquí dispara SpiralLift.
    for (size_t i = loops_before; i < g.loops->entities.size(); ++i)
        set_no_spiral_lift_recursive(g.loops->entities[i]);

    // Orden exterior/interiores, igual que la v3 y que Hybrid v2.
    {
        const WallSequence ws = original_cfg->wall_sequence;
        const size_t inners_end = g.loops->entities.size();
        if (ws != WallSequence::OuterInner && loops_before < loops_after_outer && loops_after_outer < inners_end)
            std::rotate(g.loops->entities.begin() + loops_before,
                        g.loops->entities.begin() + loops_after_outer,
                        g.loops->entities.begin() + inners_end);
    }

    // 4. Relleno: SÓLO lo que los trazos no hayan cubierto. En una letra no queda nada; en una
    //    pieza ancha queda el centro, porque un trazo más ancho que `max_stroke_w` no se
    //    planifica (ver plan_stroke) y esa zona se la queda el relleno de siempre.
    {
        const auto t_f = ns_clk::now();
        Polygons all_cov = covered_all;
        append(all_cov, outer_cov);
        const ExPolygons covered = union_ex(all_cov);
        // 🚨 Sólo es "sin cubrir" lo que admite un cordón. Restar áreas deja astillas de
        // centésimas por todo el borde, y con el solape del relleno vuelven a crecer ENCIMA de lo
        // ya puesto: en una capa maciza salen como `Top surface` sobre material (s325, lo vio
        // Neotko en el G-code de la G). El criterio es el mismo que el de "hecho" de la fase P —
        // ¿cabe el cordón? — así que la sonda y el recorte no pueden discrepar por definición.
        const float half = float(scaled<double>(P.w_ref / 2.));   // s331c — un cordón de relleno entero
        const ExPolygons raw_left = diff_ex(original_slice, covered);
        const ExPolygons left_over = offset_ex(offset_ex(raw_left, -half), half);
        const bool topbottom = (g.layer_id == 0 || g.upper_slices == nullptr);
        const ConfigOptionPercent& ov_opt = topbottom ? original_cfg->top_bottom_infill_wall_overlap
                                                      : original_cfg->infill_wall_overlap;
        const coord_t base = coord_t(g.perimeter_flow.scaled_spacing() / 2) + g.solid_infill_flow.scaled_spacing() / 2;
        const double  ov   = scale_(ov_opt.get_abs_value(unscale<double>(base)));
        const ExPolygons fill_area = offset_ex(left_over, float(ov));
        Surfaces clipped;
        clipped.reserve(g.fill_surfaces->surfaces.size());
        for (const Surface& s : g.fill_surfaces->surfaces)
            for (ExPolygon& ex : intersection_ex(ExPolygons{ s.expolygon }, fill_area))
                clipped.emplace_back(Surface(s, std::move(ex)));
        g.fill_surfaces->surfaces = std::move(clipped);
        if (g.fill_no_overlap != nullptr && !g.fill_no_overlap->empty())
            *g.fill_no_overlap = intersection_ex(*g.fill_no_overlap, left_over);
        t_fill = ns_ms(t_f);

        if (dbg) {
            // Sonda de tiempos: se escribe SIEMPRE que el canal está encendido, actúe o no el patín.
            char tb[1024];
            snprintf(tb, sizeof(tb),
                     "[NS-T] L%d islas=%zu caminos=%zu | total=%.1f ms | classic=%.1f plan=%.1f costura=%.1f orden=%.1f"
                     " suelo_patin=%.1f ruta_patin=%.1f emision=%.1f cobertura=%.1f relleno=%.1f"
                     " | seg_ok=%zu astar=%zu filas=%zu"
                     " | plan: esqueleto=%.1f trazos=%.1f puntas=%.1f residuo=%.1f detalles=%.1f"
                     " | costura: coser=%.1f caudal=%.1f",
                     g.layer_id, islands.size(), n_paths, ns_ms(t_start), t_classic, t_plan, t_stitch, t_order,
                     t_ground, t_route, t_emit, t_cover, t_fill,
                     ns_probe_seg_calls, ns_probe_astar, ns_probe_raster_rows,
                     ns_t_skel, ns_t_strokes, ns_t_tips, ns_t_residual, ns_t_details,
                     ns_t_stitch_only, ns_t_flow);
            NeoDebug::write(NeoDebug::NEOSTROKE, tb);
            // El criterio de "hecho" de la fase P: cero zonas donde quepa el cordón.
            // 🚨 s329 — el cordón se mide con `detail_min`, NO con el diámetro del cabezal. Con el
            // cabezal esta cifra decía `0.000` mientras había 20 mm² sin cubrir en la misma capa,
            // y eso tranquilizaba sin motivo: los huecos que se ven en la pieza son justo los que
            // no llegan al cabezal. (`left_over`, lo que se le pasa al relleno de Orca, sí se
            // queda con el criterio de cabezal: ese relleno necesita cordón entero.)
            const ExPolygons holes = offset_ex(raw_left, -float(scaled<double>(P.detail_min / 2.)));
            // 🚨 s329 — SOLAPE: lo que se pinta dos veces. Es el número que delata el pegote y no
            // existía. Medido en el TEST08 antes del arreglo: 38 % en una barra de 3.20 x 12 mm,
            // concentrado en los 2 mm de cada extremo, con el volumen extruido a 1.25 veces el
            // hueco disponible. Sale de restar la unión a la suma de las huellas camino a camino.
            const double cov_union = area_mm2(union_ex(covered_all));
            const double solape    = std::max(0., cov_sum - cov_union);
            char sbuf[200];
            if (P.ovl_pct > 0.)          // formato literal: un ternario aqui es -Wformat-nonliteral
                snprintf(sbuf, sizeof(sbuf),
                         "caudal %.1f%%%s ancho %.2f-%.2f recto %.0f%% giro %.1f-%.1f g/mm"
                         " span=%.2f tramos=%zu tocados=%zu dvol=%+.3f%%",
                         100. * P.ovl_pct, ns_ovl_env().has_pct ? " (env)" : "",
                         P.w_ref, P.ovl_w1 * P.w_ref, 100. * P.ovl_straight,
                         P.ovl_turn0, P.ovl_turn1, P.ovl_span,
                         ns_ov_runs, ns_ov_touched,
                         ns_ov_vol_in > 1e-9 ? 100. * (ns_ov_vol_out - ns_ov_vol_in) / ns_ov_vol_in : 0.);
            else
                snprintf(sbuf, sizeof(sbuf), "off");
            const std::string ovl(sbuf);
            char buf[1024];
            snprintf(buf, sizeof(buf),
                     "[NS] L%d islas=%zu | trazos=%zu lineas=%zu caminos=%zu extruido=%.1f mm"
                     " | grietas=%zu rellenos=%zu anchos=%zu detalles_fuera=%zu repintadas=%zu tapas=%zu"
                     " | sin cubrir=%.3f mm2, de eso donde CABE el cordon=%.3f mm2 (a relleno: %.3f)"
                     " | solape=%.3f mm2 de %.3f (%.1f %%)"
                     " | ref=%.3f muro=%.3f suelo=%.3f techo=%.3f detalle=%.3f cordon<=%.3f"
                     " trazo<=%.1f ganchos=%d patin=%d jitter=%d minreal=%.3f giros=%d kvar=%d rampa=%s",
                     g.layer_id, islands.size(), total.strokes, n_lines, n_paths, emitted_mm,
                     total.slivers, total.fills, total.too_wide, total.dropped_detail, total.repainted,
                     total.caps,
                     area_mm2(raw_left), area_mm2(holes), area_mm2(left_over),
                     solape, cov_union, cov_union > 0. ? 100. * solape / cov_union : 0.,
                     P.w_ref, P.outer_w, P.floor_w, P.ceiling_w, P.detail_min, P.max_bead,
                     P.max_stroke_w, int(P.corner_hooks), int(P.skate), int(P.layer_jitter),
                     // 🚨 Si se toca este formato, CONTAR los especificadores contra los
                     //    argumentos A MANO. Ya pasó una vez: un `%s` de más se comió un double
                     //    como si fuera `char*`, `strlen` reventó dentro de `snprintf` y el build
                     //    NO lo paró — compiló y petó al laminar, en un hilo de TBB.
                     P.bead_min,
                     int(P.cont_turns), int(P.var_k), ovl.c_str());
            NeoDebug::write(NeoDebug::NEOSTROKE, buf);
        }
    }
}

}} // namespace Slic3r::NeoArachne
