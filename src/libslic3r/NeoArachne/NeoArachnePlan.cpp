// NEOTKO_NEOARACHNE_TAG fase1+fase2+fase3.0
#include "NeoArachnePlan.hpp"
#include "NeoArachneInterior.hpp"
#include "NeoArachneRuntime.hpp"
#include "NeoArachneDebug.hpp"

#include "../PerimeterGenerator.hpp"
#include "../PrintConfig.hpp"
#include "../Print.hpp"
#include "../SurfaceCollection.hpp"
#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"

#include <algorithm>  // std::clamp

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
static void set_no_spiral_lift_recursive(ExtrusionEntity *ee)
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
    cfg.thin_walls  = WallSource::Classic;  // Fase 6
    // Fase 3.0 — Edge Closure params from the live config.
    cfg.allowed_overlap_pct  = original_cfg->neoarachne_allowed_overlap_pct.value;
    cfg.min_bead_width_pct   = original_cfg->neoarachne_min_bead_width_pct.value;
    cfg.max_bead_width_pct   = original_cfg->neoarachne_max_bead_width_pct.value;
    cfg.min_feature_size_pct = original_cfg->neoarachne_min_feature_size_pct.value;
    cfg.keep_short_tails     = original_cfg->neoarachne_keep_short_tails.value;
    // Fase 3 — NeotkoEdgeBeadingStrategy knobs.
    cfg.bead_count_hysteresis_pct = original_cfg->neoarachne_bead_count_hysteresis_pct.value;
    // Fase 4 — SkeletalTrapezoidation transition smoothing.
    cfg.transition_filter_dist_mm = original_cfg->neoarachne_transition_filter_dist_mm.value;
    // cfg.pin_outer_width keeps its default true; a future UI Advanced ⚙ may
    // expose it. Pinning is gated upstream by neotko_edge_active anyway.
    // Merge global advanced toggles from Runtime singleton (Fase 6 will fill these).
    const Config& runtime = Runtime::get().cfg;
    cfg.emit_svg_per_layer  = runtime.emit_svg_per_layer;
    cfg.emit_gcode_comments = runtime.emit_gcode_comments;
    cfg.svg_layer_from      = runtime.svg_layer_from;
    cfg.svg_layer_to        = runtime.svg_layer_to;

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
    if (!outer_is_arachne_family && cfg.inner_walls == WallSource::Classic) {
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
