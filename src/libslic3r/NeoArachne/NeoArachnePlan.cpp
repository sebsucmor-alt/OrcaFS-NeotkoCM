// NEOTKO_NEOARACHNE_TAG fase1+fase2+fase3.0
#include "NeoArachnePlan.hpp"
#include "NeoArachneInterior.hpp"
#include "NeoArachneRuntime.hpp"
#include "NeoArachneDebug.hpp"
#include "NeoArachneSpine.hpp"
#include "NeoStroke.hpp"   // NEOTKO_NEOSTROKE_TAG C1 (s325)

#include "../NeoDebug.hpp"
#include "../PerimeterGenerator.hpp"
#include "../ShortestPath.hpp"
#include "../PrintConfig.hpp"
#include "../Print.hpp"
#include "../SurfaceCollection.hpp"
#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"

#include <algorithm>  // std::clamp
#include <cstdio>     // snprintf (sonda v3-spine)

namespace Slic3r { namespace NeoArachne {

// NEOTKO_NEOARACHNE_TAG s93 #30b — Recursively walk an ExtrusionEntity tree and
// set force_no_spiral_lift=true on every leaf ExtrusionPath. Used in Plan::run
// to mark the Classic outer paths (emitted by process_classic) that participate
// in Hybrid v2, so the travel from Classic outer → Arachne inner doesn't
// trigger Auto Lift's SpiralLift heuristic.
//
// Without this, ~143 G3 helical lifts persist in base02 after #30 — these are
// the travels OUT of Classic outer paths that, isolated, look like normal
// perimeter exits and trigger spiral. With this, ALL paths emitted during a
// NeoArachne region opt out of spiral, restoring linear LazyLift everywhere
// within the region.
void set_no_spiral_lift_recursive(ExtrusionEntity *ee)
{
    if (ee == nullptr) return;
    if (auto *path = dynamic_cast<ExtrusionPath*>(ee)) {
        path->force_no_spiral_lift = true;
        return;
    }
    if (auto *loop = dynamic_cast<ExtrusionLoop*>(ee)) {
        loop->force_no_spiral_lift = true;
        for (ExtrusionPath &p : loop->paths)
            p.force_no_spiral_lift = true;
        return;
    }
    if (auto *mp = dynamic_cast<ExtrusionMultiPath*>(ee)) {
        mp->force_no_spiral_lift = true;
        for (ExtrusionPath &p : mp->paths)
            p.force_no_spiral_lift = true;
        return;
    }
    if (auto *coll = dynamic_cast<ExtrusionEntityCollection*>(ee)) {
        coll->force_no_spiral_lift = true;
        for (ExtrusionEntity *child : coll->entities)
            set_no_spiral_lift_recursive(child);
        return;
    }
}

// NEOTKO_NEOARACHNE_TAG fase2.5 + s93 #32 — Dispatch table (revised).
//
//   outer family    │ inner family   │ gap (advisory) │ action                       │ status
//   ────────────────┼────────────────┼────────────────┼──────────────────────────────┼─────────
//   Classic         │ Classic        │ *              │ process_classic              │ ✅ sanity
//   ArachneStock    │ ArachneStock   │ *              │ process_arachne              │ ✅ sanity
//   ArachneNotEdge  │ ArachneNotEdge │ *              │ process_arachne + warn       │ ⏳ Fase 5 (real injection)
//   Classic         │ Arachne*       │ *              │ Hybrid v2 (Classic outer + Arachne inner) │ ✅ DEFAULT
//   Arachne*        │ Classic        │ *              │ validator BLOQUEA (alinea)   │ ⛔ rejected
//
// s93 #32 changes from the original Fase 2.5 dispatch:
//   • Removed the gap_fill == Off constraint from sanity branches. Classic
//     and Arachne handle gap_fill internally (Classic via medial-axis,
//     Arachne via is_odd beads); the neoarachne_gap_fill selector only
//     advises the Hybrid v2 pipeline.
//   • Added ArachneNotEdge/ArachneNotEdge → process_arachne with warning
//     (Fase 5 will inject cap_widening / hysteresis params into make_paths_params
//     so this combo runs the full NeotkoEdge pipeline natively).
//   • Documented the validator-blocked case explicitly.
//
// Combos blocked by the UI validator (ConfigManipulation::update_print_fff_config):
//   outer = Off                              → forced to Classic
//   outer = Arachne* + inner = Classic       → inner aligned to outer

// NEOTKO_NEOARACHNE_TAG v3-spine (s323) — NeoArachne v3, "tipo S3D":
//   1. Classic SÓLO para el muro exterior (y thin walls, si el usuario las tiene), sin gap-fill.
//   2. Zona = lo que queda dentro del eje del muro exterior y no tapa ese muro.
//   3. Muros interiores PROPIOS, nivel a nivel, de ancho fijo. ANTES de cada uno, cada zona:
//        · cabe ENTERA en una línea <= techo → espina, y ahí no hay más muros (regla S3D);
//        · no quedan muros por poner         → relleno;
//        · si no                             → un bucle más (offset −spacing/2) y se sigue dentro.
//      Así desaparecen los muros PARCIALES de Classic y las migas entre muros que la fase 1
//      rellenaba al suelo (anillo o0.45: 121 % de material y 17–20 caminos, s323).
//   4. Espina (NeoArachneSpine) y relleno recortado a lo que sobra, con el solape de Classic.
static void run_classic_spine(PerimeterGenerator& g, const Config& cfg, const PrintRegionConfig* original_cfg)
{
    // Muros de verdad para esta capa (mismas reglas que Classic / Hybrid v2).
    int walls = original_cfg->wall_loops.value;
    if (walls < 1) {   // sin muros no hay nada que decidir: Classic tal cual
        g.process_classic();
        return;
    }
    if (g.layer_id == g.object_config->raft_layers && original_cfg->only_one_wall_first_layer)
        walls = 1;
    if (walls > 1 && original_cfg->only_one_wall_top && g.upper_slices == nullptr)
        walls = 1;

    // Copia local, como en Hybrid v2: otras LayerRegion del mismo PrintRegion pueden estar
    // laminándose en paralelo.
    PrintRegionConfig modified_cfg = *original_cfg;
    modified_cfg.wall_loops.value           = 1;      // Classic sólo el exterior
    modified_cfg.gap_infill_speed.value     = 0;      // la espina sustituye al gap-fill de Classic
    modified_cfg.alternate_extra_wall.value = false;  // los muros interiores los decide la regla S3D

    ExPolygons original_slice;
    original_slice.reserve(g.slices->surfaces.size());
    for (const Surface& s : g.slices->surfaces)
        original_slice.push_back(s.expolygon);

    const size_t loops_before = g.loops->entities.size();
    const size_t gap_before   = (g.gap_fill != nullptr) ? g.gap_fill->entities.size() : 0;
    g.config = &modified_cfg;
    g.process_classic();
    g.config = original_cfg;
    const size_t loops_after_outer = g.loops->entities.size();

    // Lo que tapa el muro exterior, medido por su separación (así el primer interior no deja hueco).
    Polygons outer_cov;
    for (size_t i = loops_before; i < loops_after_outer; ++i)
        g.loops->entities[i]->polygons_covered_by_spacing(outer_cov, float(SCALED_EPSILON));

    // Sólo el interior: por fuera del eje del muro exterior mandan el muro y detect_thin_wall.
    const float eps           = float(scaled<double>(0.005));   // migas numéricas, no puntas
    const float inside_offset = float(g.ext_perimeter_flow.scaled_spacing()) / 2.f;
    ExPolygons todo = diff_ex(offset_ex(original_slice, -inside_offset), outer_cov, ApplySafetyOffset::Yes);
    todo = opening_ex(todo, eps);

    const double line_w = g.perimeter_flow.width();
    SpineParams sp;
    sp.floor_mm      = line_w * cfg.spine_min_width_pct / 100.;
    sp.ceiling_mm    = line_w * cfg.spine_max_width_pct / 100.;
    sp.min_length_mm = cfg.spine_min_length_mm;
    sp.sliver_mm     = line_w * cfg.spine_sliver_pct / 100.;
    const double ceiling_w = scaled<double>(std::max(sp.ceiling_mm, sp.floor_mm));
    const float  spacing   = float(g.perimeter_flow.scaled_spacing());
    // Cuellos que un bucle no alcanza: se quedan como zona si tienen cuerpo; las astillas de las
    // esquinas del bucle (más finas que medio suelo) se tiran.
    const float  neck_open = float(std::max(double(eps), scaled<double>(sp.floor_mm) / 4.));

    ExPolygons              spine_regions, forced_spine, fill_left;
    std::vector<ExPolygons> levels;   // ejes de los bucles interiores; levels[0] = primer interior
    // Minibucles (s323, S a 0.3: bucles de 0.86 y 1.36 mm en las colas): un bucle que encierra menos
    // de ~3 líneas de diámetro no es un muro, es una gota con su arranque y su parada. No se hace;
    // su zona va a espina aunque pase del techo (el ancho se recorta al techo).
    const double min_loop_len = PI * 3. * scaled<double>(line_w);
    size_t       tiny_loops   = 0;
    int loops_left = walls - 1;
    while (!todo.empty()) {
        ExPolygons next, centres;
        for (ExPolygon& comp : todo) {
            if (fits_in_one_line(comp, ceiling_w)) {
                spine_regions.emplace_back(std::move(comp));
                continue;
            }
            ExPolygons centre = offset_ex(ExPolygons{ comp }, -spacing / 2.f);
            const size_t n_raw = centre.size();
            centre.erase(std::remove_if(centre.begin(), centre.end(),
                                        [&](const ExPolygon& c) { return c.contour.length() < min_loop_len; }),
                         centre.end());
            const bool only_tiny = n_raw > 0 && centre.empty();   // sólo cabrían minibucles
            if (loops_left <= 0 || centre.empty()) {
                if (only_tiny) {
                    tiny_loops += n_raw;
                    forced_spine.emplace_back(std::move(comp));
                } else {   // quedan muros agotados, o ni cabe un bucle ni cabe la línea (techo del usuario)
                    fill_left.emplace_back(std::move(comp));
                }
                continue;
            }
            tiny_loops += n_raw - centre.size();
            ExPolygons inner = offset_ex(ExPolygons{ comp }, -spacing);
            append(inner, opening_ex(diff_ex(ExPolygons{ comp }, offset_ex(centre, spacing / 2.f)), neck_open));
            append(next, union_ex(inner));
            append(centres, std::move(centre));
        }
        if (!centres.empty())
            levels.emplace_back(std::move(centres));
        --loops_left;
        todo = std::move(next);
    }

    // Espina primero (se coloca abajo junto a los bucles de su isla). Lo que no da ninguna línea
    // vuelve a relleno (s323: el centro del ancla se perdía).
    SpineStats           st;
    ExPolygons           rejected;
    ExtrusionEntitiesPtr spine_ents;
    run_spine(g, spine_regions, sp, &st, &rejected, &forced_spine, &spine_ents);
    append(fill_left, std::move(rejected));

    // Bucles interiores, de ancho fijo como los de Classic, y la espina, en un cubo por isla (como
    // Interior::run) para que la impresora termine una isla antes de saltar a la siguiente.
    //
    // s323 — orden: la espina es lo MÁS interior. Con InnerOuter (de dentro a fuera) va la primera:
    // espina → interiores → exterior. Antes iba al final como gap-fill del bloque de relleno y la
    // impresora "volvía al interior" tras el exterior: un viaje y una retracción de más por isla.
    // Con OuterInner va la última.
    const WallSequence ws = original_cfg->wall_sequence;
    {
        const ExPolygons islands = union_ex(original_slice);
        std::vector<ExtrusionEntityCollection> per_island(islands.size() + 1);
        auto island_of = [&](const Point& p) -> size_t {
            for (size_t i = 0; i < islands.size(); ++i)
                if (islands[i].contains(p))
                    return i;
            return islands.size();
        };
        // Espina repartida por isla y encadenada por cercanía (con la vuelta que convenga a cada línea).
        std::vector<ExtrusionEntitiesPtr> spine_by_island(islands.size() + 1);
        for (ExtrusionEntity* e : spine_ents)
            spine_by_island[island_of(e->first_point())].push_back(e);
        spine_ents.clear();
        for (ExtrusionEntitiesPtr& v : spine_by_island)
            if (v.size() > 1)
                chain_and_reorder_extrusion_entities(v);
        auto emit_spine = [&]() {
            for (size_t i = 0; i < spine_by_island.size(); ++i)
                if (!spine_by_island[i].empty())
                    per_island[i].append(std::move(spine_by_island[i]));   // el cubo se queda la propiedad
        };
        if (ws != WallSequence::OuterInner)
            emit_spine();
        const Flow& pf = g.perimeter_flow;
        auto emit_level = [&](const ExPolygons& centres, int inset) {
            for (const ExPolygon& c : centres) {
                ExtrusionEntityCollection& dst = per_island[island_of(c.contour.first_point())];
                auto add = [&](const Polygon& poly, bool hole) {
                    ExtrusionPath path(erPerimeter, pf.mm3_per_mm(), pf.width(), pf.height());
                    path.polyline  = poly.split_at_first_point();
                    path.inset_idx = inset;
                    ExtrusionLoop loop(std::move(path));
                    loop.make_counter_clockwise();
                    loop.set_loop_role(hole ? elrHole : elrDefault);
                    loop.inset_idx = inset;
                    dst.append(std::move(loop));
                };
                add(c.contour, false);
                for (const Polygon& h : c.holes)
                    add(h, true);
            }
        };
        // Dentro de cada isla: de fuera a dentro con OuterInner, de dentro a fuera si no.
        if (ws == WallSequence::OuterInner)
            for (size_t i = 0; i < levels.size(); ++i)
                emit_level(levels[i], int(i) + 1);
        else
            for (size_t i = levels.size(); i-- > 0;)
                emit_level(levels[i], int(i) + 1);
        if (ws == WallSequence::OuterInner)
            emit_spine();
        for (ExtrusionEntityCollection& bucket : per_island)
            if (!bucket.empty()) {
                bucket.no_sort = true;
                g.loops->append(bucket);
            }
    }
    // Orden exterior/interiores, igual que Hybrid v2 (s94 task#12): con InnerOuter los interiores
    // van antes. InnerOuterInner se aproxima como InnerOuter.
    {
        const size_t inners_end = g.loops->entities.size();
        if (ws != WallSequence::OuterInner && loops_before < loops_after_outer && loops_after_outer < inners_end)
            std::rotate(g.loops->entities.begin() + loops_before,
                        g.loops->entities.begin() + loops_after_outer,
                        g.loops->entities.begin() + inners_end);
    }

    // Relleno: sólo lo que ha quedado ancho, con el mismo solape contra los muros que usa Classic.
    {
        const bool topbottom = (g.layer_id == 0 || g.upper_slices == nullptr);
        const ConfigOptionPercent& ov_opt = topbottom ? original_cfg->top_bottom_infill_wall_overlap
                                                      : original_cfg->infill_wall_overlap;
        const coord_t base = coord_t(spacing / 2.f) + g.solid_infill_flow.scaled_spacing() / 2;
        const double  ov   = scale_(ov_opt.get_abs_value(unscale<double>(base)));
        const ExPolygons fill_area = offset_ex(fill_left, float(ov));
        Surfaces clipped;
        clipped.reserve(g.fill_surfaces->surfaces.size());
        for (const Surface& s : g.fill_surfaces->surfaces)
            for (ExPolygon& ex : intersection_ex(ExPolygons{ s.expolygon }, fill_area))
                clipped.emplace_back(Surface(s, std::move(ex)));
        g.fill_surfaces->surfaces = std::move(clipped);
        if (g.fill_no_overlap != nullptr && !g.fill_no_overlap->empty())
            *g.fill_no_overlap = intersection_ex(*g.fill_no_overlap, fill_left);
    }

    // Igual que Hybrid v2: nada de lo emitido en una región NeoArachne dispara SpiralLift.
    for (size_t i = loops_before; i < g.loops->entities.size(); ++i)
        set_no_spiral_lift_recursive(g.loops->entities[i]);
    if (g.gap_fill != nullptr)
        for (size_t i = gap_before; i < g.gap_fill->entities.size(); ++i)
            set_no_spiral_lift_recursive(g.gap_fill->entities[i]);

    // Sonda (ORCA_DEBUG_DISPATCH → /tmp/neotko_logs/dispatch.log): una línea por capa y región.
    if (NeoDebug::enabled(NeoDebug::DISPATCH)) {
        size_t n_loops = 0;
        for (const ExPolygons& lv : levels)
            for (const ExPolygon& c : lv)
                n_loops += 1 + c.holes.size();
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "[NA-v3] L%d muros=%d line_w=%.3f suelo=%.3f techo=%.3f minlen=%.2f | interiores: %zu niveles, %zu bucles"
                 " | espina: %zu zonas (%.3f mm2), %zu lineas, %.2f mm, %zu cortas fuera, %zu ramitas podadas, %zu cruces unidos,"
                 " %zu grietas fuera, %zu bucles mini fuera | minibucles->espina: %zu (%zu zonas) | relleno: %zu zonas",
                 g.layer_id, walls, line_w, sp.floor_mm, sp.ceiling_mm, sp.min_length_mm, levels.size(), n_loops,
                 st.spine_components, st.spine_area_mm2, st.polylines, st.spine_len_mm, st.dropped_short,
                 st.spurs_pruned, st.junction_joins, st.slivers_skipped, st.tiny_loops_dropped,
                 tiny_loops, forced_spine.size(), fill_left.size());
        NeoDebug::write(NeoDebug::DISPATCH, buf);
    }
}

void Plan::run(PerimeterGenerator& g)
{
    // ── Build the per-region NeoArachne config from the live PrintRegionConfig ──
    // s91 Fase 2: was reading from Runtime::get().cfg (singleton with hardcoded
    // defaults). Now we read the actual config keys neoarachne_outer_wall /
    // _inner_walls / _gap_fill that the user picked in the UI. The singleton
    // is reserved for global Advanced ⚙ toggles in Fase 6.
    const PrintRegionConfig* original_cfg = g.config;
    Config cfg;
    cfg.enabled     = true;
    cfg.outer_wall  = original_cfg->neoarachne_outer_wall.value;
    cfg.inner_walls = original_cfg->neoarachne_inner_walls.value;
    cfg.gap_fill    = original_cfg->neoarachne_gap_fill.value;
    // NEOTKO_NEOSTROKE_TAG s332 — `wall_generator = NeoStroke` es una entidad propia, no un ajuste
    // del panel de la v3: fija las tres fuentes aquí y los tres selectores de NeoArachne dejan de
    // pintar nada (la UI ni los enseña). El muro exterior lo pone SIEMPRE Classic (s335: NeoWall,
    // que era la otra opción, se retiró entera).
    // 🚨 `wall_generator` es de OBJETO, no de región: vive en `PrintObjectConfig` (que es de donde
    // lo lee `LayerRegion.cpp`), no en `PrintRegionConfig`. Las `neostroke_*` sí son de región.
    // NEOTKO_NEOSTROKE_TAG s335 — CANDADO DE DEPURACIÓN, igual que el de Bump Mapping. NeoStroke se
    // publica, pero no es estable en plástico: hay que saber lo que se hace antes de imprimir con
    // él. Dos llaves, y las dos a la vez:
    //   1. LibreMode encendido (`neotko_libre_mode`, el espejo en el motor del ajuste de la app)
    //   2. el canal `ORCA_DEBUG_NEOSTROKE` (o `ORCA_DEBUG_ALL`)
    // 🚨 Este es el candado del MOTOR, y tiene que existir aparte del de la interfaz: la interfaz
    //    avisa y revierte (ConfigManipulation), pero un 3mf ajeno, una línea de órdenes o un perfil
    //    editado a mano no pasan por ella. Con el candado cerrado se cae a la ruta normal de
    //    NeoArachne y queda el aviso en el log, que es mejor que imprimir con un motor que el
    //    usuario no ha desbloqueado.
    // 🚨 `neotko_libre_mode` vive en `PrintObjectConfig` (el espejo que inyecta
    //    `background_process.apply`), NO en `PrintRegionConfig`, que es lo que apunta `original_cfg`.
    if (g.object_config != nullptr
        && g.object_config->wall_generator.value == PerimeterGeneratorType::NeoStroke) {
        const bool ns_gate_open = g.object_config->neotko_libre_mode.value
                               && NeoDebug::enabled(NeoDebug::NEOSTROKE);
        if (ns_gate_open) {
            cfg.outer_wall  = WallSource::Classic;
            cfg.inner_walls = WallSource::NeoStroke;
            cfg.gap_fill    = WallSource::Off;   // NeoStroke se come el interior entero, gap incluido
        } else {
            NeoDebug::write(NeoDebug::NEOSTROKE,
                "[NS] wall_generator=NeoStroke pero el candado esta CERRADO "
                "(hace falta LibreMode + ORCA_DEBUG_NEOSTROKE): se usa la ruta normal.");
        }
    }
    cfg.thin_walls  = WallSource::Classic;  // Fase 6
    // Fase 3.0 — Edge Closure params from the live config.
    cfg.allowed_overlap_pct  = original_cfg->neoarachne_allowed_overlap_pct.value;
    cfg.min_bead_width_pct   = original_cfg->neoarachne_min_bead_width_pct.value;
    cfg.max_bead_width_pct   = original_cfg->neoarachne_max_bead_width_pct.value;
    cfg.min_feature_size_pct = original_cfg->neoarachne_min_feature_size_pct.value;
    cfg.keep_short_tails     = original_cfg->neoarachne_keep_short_tails.value;
    // Fase 3 — NeotkoEdgeBeadingStrategy knobs.
    cfg.pin_outer_width           = original_cfg->neoarachne_pin_outer_width.value;
    cfg.bead_count_hysteresis_pct = original_cfg->neoarachne_bead_count_hysteresis_pct.value;
    // Fase 4 — SkeletalTrapezoidation transition smoothing.
    cfg.transition_filter_dist_mm = original_cfg->neoarachne_transition_filter_dist_mm.value;
    // NEOTKO_NEOARACHNE_TAG v3-spine (s323)
    cfg.spine               = original_cfg->neoarachne_spine.value;
    cfg.spine_min_width_pct = original_cfg->neoarachne_spine_min_width_pct.value;
    cfg.spine_max_width_pct = original_cfg->neoarachne_spine_max_width_pct.value;
    cfg.spine_min_length_mm = original_cfg->neoarachne_spine_min_length.value;
    cfg.spine_sliver_pct    = original_cfg->neoarachne_spine_sliver_pct.value;
    // NEOTKO_NEOSTROKE_TAG C5b (s325)
    cfg.neostroke_corner_hooks = original_cfg->neostroke_corner_hooks.value;
    cfg.neostroke_min_width_pct = original_cfg->neostroke_min_width_pct.value;
    cfg.neostroke_max_width_pct = original_cfg->neostroke_max_width_pct.value;
    cfg.neostroke_detail_min_pct = original_cfg->neostroke_detail_min_pct.value;   // NEOTKO_NEOSTROKE_TAG s329
    cfg.neostroke_width_ref      = original_cfg->neostroke_width_ref.value;       // NEOTKO_NEOSTROKE_TAG s331c
    cfg.neostroke_curve_overlap     = original_cfg->neostroke_curve_overlap.value;      // NEOTKO_NEOSTROKE_TAG s331
    cfg.neostroke_overlap_width_end = original_cfg->neostroke_overlap_width_end.value;
    cfg.neostroke_overlap_turn_min  = original_cfg->neostroke_overlap_turn_min.value;
    cfg.neostroke_overlap_turn_max  = original_cfg->neostroke_overlap_turn_max.value;
    cfg.neostroke_overlap_span      = original_cfg->neostroke_overlap_span.value;
    cfg.neostroke_overlap_straight  = original_cfg->neostroke_overlap_straight.value;     // s331b
    cfg.neostroke_cap_join          = original_cfg->neostroke_cap_join.value;            // s331d
    cfg.neostroke_max_bead_pct      = original_cfg->neostroke_max_bead_pct.value;
    cfg.neostroke_max_stroke_width  = original_cfg->neostroke_max_stroke_width.value;
    cfg.neostroke_bead_min_pct  = original_cfg->neostroke_bead_min_pct.value;   // s332
    cfg.neostroke_layer_jitter  = original_cfg->neostroke_layer_jitter.value;   // s332
    cfg.neostroke_skate         = original_cfg->neostroke_skate.value;
    cfg.neostroke_skate_detour  = original_cfg->neostroke_skate_detour.value;
    // pin_outer_width is gated upstream by neotko_edge_active anyway (ConfigManipulation
    // hides the control unless outer or inner wall source is ArachneNeotkoEdge).
    // Merge global advanced toggles from Runtime singleton (Fase 6 will fill these).
    const Config& runtime = Runtime::get().cfg;
    cfg.emit_svg_per_layer  = runtime.emit_svg_per_layer;
    cfg.emit_gcode_comments = runtime.emit_gcode_comments;
    cfg.svg_layer_from      = runtime.svg_layer_from;
    cfg.svg_layer_to        = runtime.svg_layer_to;

    // NEOTKO_NEOSTROKE_TAG C1 (s325) — sonda INCONDICIONAL: una línea cada vez que NeoArachne entra,
    // antes de decidir nada y valga lo que valga el combo. La falta de esto costó una sesión: el
    // desplegable marcaba NeoStroke, el 3mf guardaba `off` (bug del índice del combo, Field.cpp), el
    // despacho se iba a Hybrid v2 — que no escribe en este canal — y el fichero no aparecía, con lo
    // que parecía que el motor ni se había ejecutado. Un log que sólo escribe cuando actúa no
    // descarta nada.
    if (NeoDebug::enabled(NeoDebug::DISPATCH)) {
        auto src_name = [](WallSource w) {
            switch (w) {
            case WallSource::Classic:           return "classic";
            case WallSource::ArachneStock:      return "arachne_stock";
            case WallSource::ArachneNeotkoEdge: return "arachne_neotkoedge";
            case WallSource::Off:               return "off";
            case WallSource::NeoStroke:         return "neostroke";
            }
            return "?";
        };
        char b[256];
        snprintf(b, sizeof(b), "[NA] L%d entra: exterior=%s interior=%s gap=%s espina=%d muros=%d",
                 g.layer_id, src_name(cfg.outer_wall), src_name(cfg.inner_walls), src_name(cfg.gap_fill),
                 int(cfg.spine), original_cfg->wall_loops.value);
        NeoDebug::write(NeoDebug::DISPATCH, b);
    }

    // ── Dispatch on the wall-source combo (s93 #32 revised). ─────────────────
    // The user can mix-and-match outer/inner across {Classic, ArachneStock,
    // ArachneNeotkoEdge}. We map combo families to engine pipelines.
    const bool outer_is_arachne_family = (cfg.outer_wall  == WallSource::ArachneStock)
                                      || (cfg.outer_wall  == WallSource::ArachneNeotkoEdge);
    const bool inner_is_arachne_family = (cfg.inner_walls == WallSource::ArachneStock)
                                      || (cfg.inner_walls == WallSource::ArachneNeotkoEdge);

    // Case A: Classic outer + Classic inner — delegate to Classic. Classic
    // handles its own medial-axis gap_fill; the neoarachne_gap_fill selector
    // is informational only here.
    // NEOTKO_NEOSTROKE_TAG C1 (s325) — NeoStroke: interior por trazos. En C1 lo único que hace es
    // sacar el esqueleto de cada isla y escribirlo en DISPATCH (para casarlo con el prototipo de la
    // fase P); los muros los sigue poniendo la v3. C2 pone las k líneas por trazo.
    if (!outer_is_arachne_family && cfg.inner_walls == WallSource::NeoStroke) {
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "neostroke/C1-C5");
        run_neostroke(g, cfg, original_cfg);
        return;
    }

    if (!outer_is_arachne_family && cfg.inner_walls == WallSource::Classic) {
        // NEOTKO_NEOARACHNE_TAG v3-spine (s323) — default v3: Classic walls + spine.
        if (cfg.spine) {
            log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "v3/classic-walls+spine");
            run_classic_spine(g, cfg, original_cfg);
            return;
        }
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "phase2.5/sanity-classic");
        g.process_classic();
        return;
    }

    // Case B: ArachneStock outer + ArachneStock inner — delegate to upstream
    // Arachne stock. No NeoArachne machinery engaged → bit-identical to
    // wall_generator=arachne. Useful as a baseline.
    if (cfg.outer_wall == WallSource::ArachneStock
        && cfg.inner_walls == WallSource::ArachneStock) {
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "phase2.5/sanity-arachne-stock");
        g.process_arachne();
        return;
    }

    // Case C: NeotkoEdge outer + NeotkoEdge inner — would ideally run upstream
    // Arachne with our cap/pin/hysteresis params injected. That requires
    // hooking make_paths_params to read NeoArachneConfig globally — pending
    // for Fase 5. For now, fallback to process_arachne with a loud log so the
    // user sees that the full-NeotkoEdge combo isn't producing different
    // output from stock Arachne.
    if (cfg.outer_wall == WallSource::ArachneNeotkoEdge
        && cfg.inner_walls == WallSource::ArachneNeotkoEdge) {
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1,
                     "phase2.5/full-neotko-edge-fallback-to-stock-arachne-fase5");
        g.process_arachne();
        return;
    }

    // Case D: outer = Arachne* + inner = Classic — invalid combo (Arachne is
    // a generator that handles both outer + inner together; can't have
    // "Arachne outer only"). The validator (ConfigManipulation) should
    // intercept this before we get here, but as a defensive fallback we log
    // it and route to process_arachne (the closer-to-intent behaviour).
    if (outer_is_arachne_family && !inner_is_arachne_family) {
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1,
                     "phase2.5/invalid-combo-arachne-outer-classic-inner-falling-back-to-arachne");
        g.process_arachne();
        return;
    }

    // Case E (default): Classic outer + Arachne* inner — the canonical Neotko
    // Hybrid v2. Interior::run handles the Arachne inner residual with our
    // cap_widening / is_odd-as-gap-fill / no-spiral-lift pipeline.
    log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "phase2.5/hybrid-v2");

    if (cfg.gap_fill == WallSource::ArachneNeotkoEdge
        || cfg.gap_fill == WallSource::ArachneStock) {
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1,
                     "phase2.5/extra-gap-fill-pass-pending-fase5-noop");
        // Extra gap_fill pass (Hybrid v1 legacy) — Fase 5.
    }
    // ── Capture originals from the live config ──────────────────────────────
    const int    orig_wall_loops = original_cfg->wall_loops.value;
    const double orig_gap_speed  = original_cfg->gap_infill_speed.value;

    // ── Build a thread-local copy with the two knobs mutated ────────────────
    // Strategy (A) from session s91: do NOT modify the shared config (other
    // LayerRegions on the same PrintRegion may be slicing in parallel). Make
    // an independent stack copy, swap the pointer, restore on exit.
    PrintRegionConfig modified_cfg = *original_cfg;
    modified_cfg.wall_loops.value           = 1;     // Classic emits only the outer.
    modified_cfg.gap_infill_speed.value     = 0;     // Suppress Classic medial-axis gap fill
                                                     // (Arachne integrates gap into the
                                                     // interior beading).
    // s91 fix — bug "capa sí capa no": alternate_extra_wall fires on odd layers
    // (PerimeterGenerator.cpp:1206) and would push loop_number from 0 to 1,
    // making Classic emit a second wall on top of which Arachne would lay its
    // own → double extrusion in alternating layers. Force off for the duration
    // of the Classic-outer pass.
    modified_cfg.alternate_extra_wall.value = false;
    // s91 fix — detect_thin_wall emits Classic's medial-axis thin walls in narrow
    // zones (PerimeterGenerator.cpp:1230). Those zones are exactly where Arachne
    // does its beading, so leaving it on causes overlapping extrusion in W-style
    // letter strokes (0.47–1 mm wide). Hand thin-wall handling to Arachne.
    modified_cfg.detect_thin_wall.value     = false;

    // ── Snapshot the slice extents BEFORE Classic so we can derive the
    //    residual outline that goes to Arachne. We can't reliably use
    //    g.fill_surfaces after process_classic because it has already been
    //    munged with infill_peri_overlap math that doesn't match what
    //    Arachne expects. ───────────────────────────────────────────────────
    ExPolygons original_slice;
    original_slice.reserve(g.slices->surfaces.size());
    for (const Surface& s : g.slices->surfaces)
        original_slice.push_back(s.expolygon);

    // ── Swap config, run Classic, restore. ──────────────────────────────────
    // Mark which entries existed in g.loops AND g.gap_fill BEFORE process_classic,
    // so we can identify what it appends and tag them with force_no_spiral_lift=true
    // (NEOTKO_NEOARACHNE_TAG s93 #30b).
    const size_t loops_size_before_classic    = g.loops->entities.size();
    const size_t gap_fill_size_before_classic = (g.gap_fill != nullptr) ? g.gap_fill->entities.size() : 0;
    g.config = &modified_cfg;
    log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "phase1/classic-outer-only");
    g.process_classic();
    g.config = original_cfg;
    // Snapshot — after this, Interior::run will append per-island buckets.
    // Used by the wall_sequence reorder at the end of this function.
    const size_t loops_size_after_classic = g.loops->entities.size();
    // Tag the Classic outer paths emitted by process_classic so the travel
    // OUT of these paths doesn't trigger SpiralLift in needs_retraction.
    // Without this, ~143 G3 helical lifts persisted in the s93 test because
    // travels from Classic outer → Arachne inner (within the same NeoArachne
    // region) read m_last_path_force_no_spiral_lift = false from the Classic
    // outer and chose SpiralLift. Now every path in a NeoArachne region opts
    // out, regardless of which engine emitted it.
    for (size_t i = loops_size_before_classic; i < g.loops->entities.size(); ++i)
        set_no_spiral_lift_recursive(g.loops->entities[i]);
    if (g.gap_fill != nullptr) {
        for (size_t i = gap_fill_size_before_classic; i < g.gap_fill->entities.size(); ++i)
            set_no_spiral_lift_recursive(g.gap_fill->entities[i]);
    }

    // ── Decide if Arachne interior is needed. ───────────────────────────────
    // Mirror Classic's wall-count reduction logic (PerimeterGenerator.cpp:1208-1212)
    // so NeoArachne respects the same "force single wall" toggles. Without this
    // guard NeoArachne would add Arachne interior walls on layers where Classic
    // would have emitted only the outer (raft top + only_one_wall_first_layer,
    // or the topmost layer + only_one_wall_top).
    int effective_walls = orig_wall_loops;
    if (g.layer_id == g.object_config->raft_layers && original_cfg->only_one_wall_first_layer)
        effective_walls = 1;
    if (effective_walls > 1 && original_cfg->only_one_wall_top && g.upper_slices == nullptr)
        effective_walls = 1;

    if (effective_walls <= 1) {
        // Only one wall is appropriate — Classic already emitted it. Done.
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "phase1/single-wall-no-interior");
        (void) orig_gap_speed;
        return;
    }

    // ── Compute the residual outline that Arachne should fill. ──────────────
    // Baseline: the Classic outer perimeter centerline sits at ext_perimeter_spacing/2
    // inside the slice boundary; its inner edge is ext_perimeter_spacing away.
    // Offsetting the slice by -ext_perimeter_spacing aligns Arachne's first bead
    // exactly with that inner edge → no overlap, no gap (in theory). In practice
    // tolerances leave a visible seam.
    //
    // Fase 3.0 Edge Closure: allowed_overlap_pct PULLS the residual outward toward
    // the Classic outer, so Arachne's first bead overlaps the Classic outer by
    // up to that fraction of ext_perimeter_spacing. Closes the seam gap visible
    // on letters (P/W observed s91). Default 50% balances seam closure against
    // Pressure Advance over-compensation.
    //
    //   residual = slice ⊖ (ext_perimeter_spacing × (1 − overlap_pct/100))
    //
    // overlap=0%  → original behavior (visible seam)
    // overlap=50% → Arachne first bead halfway through Classic outer (default)
    // overlap=100% → residual == slice, Arachne overwrites Classic entirely
    //               (validator blocks this; harmful)
    const coord_t ext_perimeter_spacing = g.ext_perimeter_flow.scaled_spacing();
    const double  overlap_frac          = std::clamp(cfg.allowed_overlap_pct / 100.0, 0.0, 1.0);
    const coord_t residual_offset       = coord_t(double(ext_perimeter_spacing) * (1.0 - overlap_frac));
    const ExPolygons residual = offset_ex(original_slice, -float(residual_offset));
    if (residual.empty()) {
        // Mono-wall fallback (a) from session s91 decision: accept that the
        // Classic outer is all there is. No interior to fill. The user can
        // tune line_width if a real feature is being lost; future "ancho
        // detector" will automate that.
        log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "phase1/mono-wall-fallback");
        return;
    }

    // ── Drive Arachne over the residual. ────────────────────────────────────
    const int inset_count = effective_walls - 1;
    const ExPolygons arachne_inner = Interior::run(g, to_polygons(residual), inset_count, cfg);

    // ── Clip fill_surfaces by Arachne's inner contour. ──────────────────────
    // process_classic populated g.fill_surfaces assuming there were no inner
    // walls (loop_number == 0 path inside it, with inset = ext_perimeter_spacing/2).
    // That leaves fill_surfaces covering ALMOST the entire interior. Now that
    // Arachne has consumed part of that area with N inner walls, restrict the
    // infill region to whatever Arachne left untouched.
    //
    // s91 bug fix: this clip MUST happen unconditionally. Earlier we guarded
    // with `if (!arachne_inner.empty())` which silently kept the full interior
    // when Arachne's getInnerContour() reported empty (common in thin regions
    // and certain beading configurations). Result: solid-top/solid-bottom
    // infill was emitted over the full interior, overlapping every Arachne
    // wall — the "capa imprime mal, después imprime bien" duplicate-extrusion
    // bug the user observed in the W letter test. Interior::run now ALWAYS
    // returns a bounded inner contour (Arachne-reported or conservative
    // fallback), so unconditional clipping is safe and correct.
    {
        Surfaces  clipped;
        const Surfaces& src = g.fill_surfaces->surfaces;
        clipped.reserve(src.size());
        for (const Surface& s : src) {
            ExPolygons isect = intersection_ex(ExPolygons{ s.expolygon }, arachne_inner);
            for (ExPolygon& ex : isect)
                clipped.emplace_back(Surface(s, std::move(ex)));
        }
        g.fill_surfaces->surfaces = std::move(clipped);
    }

    // Also restrict fill_no_overlap_expolygons. process_classic populated this
    // with the no-overlap variant of the same wall_loops=1 interior. Without
    // clipping it here it leaks the unclipped boundary into downstream infill
    // helpers (BBS no-overlap math) that some patterns consult. Mirror the
    // same intersection logic.
    if (g.fill_no_overlap != nullptr && !g.fill_no_overlap->empty()) {
        *g.fill_no_overlap = intersection_ex(*g.fill_no_overlap, arachne_inner);
    }

    // ── wall_sequence reorder (NEOTKO_NEOARACHNE_TAG s94 task#12). ──────────
    //
    // Bug verified empirically: setting wall_sequence in the UI had ZERO
    // effect on NeoArachne objects because Plan::run hardcoded the emission
    // order to outer-then-inner. Reason: process_classic ran with
    // wall_loops=1, which gave its internal wall_sequence reorder logic
    // (PerimeterGenerator.cpp:1441) a single entity to operate on — a
    // no-op. Then Interior::run appended inner walls AFTER, with no
    // consultation of wall_sequence.
    //
    // The fix: after both passes have populated g.loops, rotate the slice
    // of entries [outers_begin .. inners_end) according to the requested
    // wall_sequence. The outer block (one entity per island from
    // process_classic) lives at [loops_size_before_classic ..
    // loops_size_after_classic). The inner block (one bucket per island
    // from Interior::run) lives at [loops_size_after_classic .. end).
    //
    //   OuterInner       : current layout — no change.
    //   InnerOuter       : swap to [inners..., outers...]. The downstream
    //                      chain_extrusion_entities still picks islands by
    //                      proximity, so in practice it produces
    //                      (inner_A → outer_A → inner_B → outer_B → …)
    //                      because each (inner, outer) pair is co-located.
    //   InnerOuterInner  : sandwich mode. Splitting per-island inner walls
    //                      into "first-internal" and "rest" requires
    //                      reaching into the per_island buckets and
    //                      reordering by inset_idx — invasive. For this
    //                      first iteration we approximate as InnerOuter
    //                      (which is the closer of the two simple options).
    //                      Full sandwich support is task #12.5.
    const WallSequence ws = original_cfg->wall_sequence;
    if (ws != WallSequence::OuterInner) {
        const size_t outers_begin = loops_size_before_classic;
        const size_t outers_end   = loops_size_after_classic;
        const size_t inners_end   = g.loops->entities.size();
        if (outers_begin < outers_end && outers_end < inners_end) {
            auto it_begin  = g.loops->entities.begin() + outers_begin;
            auto it_middle = g.loops->entities.begin() + outers_end;
            auto it_end    = g.loops->entities.begin() + inners_end;
            // Rotate so the inner block (currently second) becomes first.
            std::rotate(it_begin, it_middle, it_end);
            if (ws == WallSequence::InnerOuterInner) {
                log_dispatch(cfg, g.layer_id, /*region_id=*/-1,
                             "phase2/wall_sequence=InnerOuterInner (approximated as InnerOuter; task#12.5)");
            } else {
                log_dispatch(cfg, g.layer_id, /*region_id=*/-1,
                             "phase2/wall_sequence=InnerOuter");
            }
        }
    }
}

}} // namespace Slic3r::NeoArachne
