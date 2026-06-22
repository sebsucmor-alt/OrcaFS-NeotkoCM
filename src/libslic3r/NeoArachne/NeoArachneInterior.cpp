// NEOTKO_NEOARACHNE_TAG fase1
#include "NeoArachneInterior.hpp"
#include "NeoArachneDebug.hpp"
#include "NeoArachneRuntime.hpp"

#include "../PerimeterGenerator.hpp"
#include "../PrintConfig.hpp"
#include "../Print.hpp"
#include "../Arachne/WallToolPaths.hpp"
#include "../Arachne/utils/ExtrusionLine.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../ClipperUtils.hpp"
#include "../libslic3r.h"

namespace Slic3r { namespace NeoArachne {

ExPolygons Interior::run(PerimeterGenerator& g,
                         const Polygons&     outline_polys,
                         int                 inset_count,
                         const Config&       cfg)
{
    // Nothing to do — no beads requested, or outline empty.
    if (inset_count <= 0 || outline_polys.empty())
        return union_ex(outline_polys);

    // All beads are "interior" beads from Arachne's POV: bead_width_0 == bead_width_x ==
    // perimeter_spacing. The outer perimeter has already been emitted by Classic.
    const coord_t perimeter_spacing = g.perimeter_flow.scaled_spacing();

    // Build params the same way process_arachne does, so global wall-related
    // knobs (transition length, etc) keep working unchanged.
    Arachne::WallToolPathsParams params = Arachne::make_paths_params(g.layer_id,
                                                                     *g.object_config,
                                                                     *g.print_config);

    // Fase 3.0 Edge Closure — override Arachne's beading floors with the per-region
    // NeoArachne config. The S3D-style behavior the user has relied on for 10+ years:
    //   min_bead_width → WideningBeadingStrategy::min_output_width
    //     Lower = thinner closure tails preserved (instead of being widened to
    //     min and emitted as full-width walls).
    //   min_feature_size → WideningBeadingStrategy::min_input_width
    //     Lower = thinner input geometry survives instead of being discarded
    //     entirely as "noise". Critical for letter strokes <0.5 mm.
    //   keep_short_tails → flow-through to WallToolPaths, which gates the
    //     removeSmallLines post-process that normally kills closure tails.
    //
    // s91 bug fix — UNIT CONTRACT: params.min_* live in MM as plain float, NOT
    // scaled coord_t. WallToolPaths::ctor (WallToolPaths.cpp:71-72) is the one
    // that calls `scaled<coord_t>(params.min_*)` to convert MM → coord_t. The
    // upstream `make_paths_params` follows this contract:
    //   input_params.min_feature_size = pct.value * 0.01 * nozzle_diameter;  // MM
    // My initial Fase 3.0 code wrapped the value in `scaled<double>()` thinking
    // params lived in coord_t. Result: double-scaling × 1e6 → min_bead_width
    // ≈ 120000 mm → every real bead is below floor → Widening discards all of
    // them → hollow W regardless of slider value. Removed the scaled<> wrapper.
    const double nozzle_diameter = g.print_config->nozzle_diameter.get_at(
        std::max(0, g.config->wall_filament.value - 1));
    if (nozzle_diameter > 0.0) {
        params.min_bead_width   = float(nozzle_diameter * cfg.min_bead_width_pct   / 100.0);
        params.min_feature_size = float(nozzle_diameter * cfg.min_feature_size_pct / 100.0);
    }
    params.keep_short_tails = cfg.keep_short_tails;

    // NEOTKO_NEOARACHNE_TAG max-bead-width — push user cap into WallToolPaths.
    // WallToolPaths.cpp:511 reads this and overrides wall_add_middle_threshold
    // (which auto-derives from min_bead_width otherwise). 0 = sentinel "use stock";
    // any value in [100,200] = explicit cap. UI validator clamps to that range.
    params.max_bead_width_pct = float(cfg.max_bead_width_pct);

    // NEOTKO_NEOARACHNE_TAG fase4 — transition_filter_dist exposure.
    // Always overrides the upstream default (100mm), even in non-NeotkoEdge
    // mode. Lower defaults (~20–50mm) produce crisper transitions which
    // benefit Hybrid v2 in general; the user can dial back to 100mm to
    // restore upstream behaviour if they observe over-sharp transitions.
    if (cfg.transition_filter_dist_mm > 0.0)
        params.wall_transition_filter_dist_mm = float(cfg.transition_filter_dist_mm);

    // NEOTKO_NEOARACHNE_TAG s93 — cap_widening ALWAYS active in Hybrid v2.
    //
    // The cap_widening behavior (= clamp every Arachne bead width to
    // optimal_width_outer) is the structural fix for the s93 over-deposit /
    // blob accumulation bug. It applies to ALL NeoArachne modes, not just
    // when the user picks ArachneNeotkoEdge — because the over-deposit is
    // intrinsic to Arachne's adaptive widening regardless of which selector
    // is set.
    //
    // pin_outer and bead_count_hysteresis remain gated on NeotkoEdge mode
    // (those are still "advanced math knobs"), but cap_widening becomes the
    // default Hybrid v2 behavior. To disable it the user can switch to
    // wall_generator=arachne (stock) directly.
    //
    // Engineering note: by enabling NeoArachneBeadingStrategy unconditionally
    // we lose the "ArachneStock bit-identical to upstream" guarantee for the
    // Arachne stock combo. That's the intentional trade-off — the upstream
    // behavior is the source of the blobs.
    const bool neotko_edge_active = (cfg.inner_walls == WallSource::ArachneNeotkoEdge)
                                 || (cfg.outer_wall  == WallSource::ArachneNeotkoEdge);
    params.neotko_edge_enabled      = true;  // always wrap with NeoArachneBeadingStrategy
    params.neotko_edge_cap_widening = true;  // always cap inner bead widths
    if (neotko_edge_active) {
        params.neotko_edge_pin_outer      = cfg.pin_outer_width;
        params.neotko_edge_hysteresis_pct = cfg.bead_count_hysteresis_pct;
    } else {
        // ArachneStock combo: skip pin_outer (let upstream Redistribute do
        // its bead-count 1/2 logic) and skip hysteresis. Cap_widening is the
        // only override.
        params.neotko_edge_pin_outer      = false;
        params.neotko_edge_hysteresis_pct = 0.0;
    }

    Arachne::WallToolPaths wtp(outline_polys,
                               /*bead_width_0=*/perimeter_spacing,
                               /*bead_width_x=*/perimeter_spacing,
                               /*inset_count=*/size_t(inset_count),
                               /*wall_0_inset=*/0,
                               g.layer_height,
                               params);

    const std::vector<Arachne::VariableWidthLines>& perimeters = wtp.getToolPaths();

    // NEOTKO_NEOARACHNE_TAG s94 task#14 — GROUP-BY-ISLAND emission.
    //
    // Arachne::WallToolPaths::getToolPaths() guarantees its output is sorted
    // by inset_idx (outermost → innermost) ACROSS ALL ISLANDS of the input
    // (see WallToolPaths.cpp:569-573 assert). If we just iterate that order
    // and dump everything into one ExtrusionEntityCollection, the printer
    // ends up emitting [inset0 of island A, inset0 of island B, …, inset0 of
    // island Z, inset1 of island A, …]. The chain_extrusion_entities pass in
    // the GCode emitter cannot recover island grouping from that — it'll
    // jump across islands at each inset level, producing the "ring around
    // every letter first, then fill in letter by letter" visual the user
    // reported. Classic process_classic naturally emits per-island
    // (Surfaces are iterated one at a time and each gives its full
    // outer+inner+gap), so the printer goes island A done → island B done →
    // etc — what the user calls "ordered".
    //
    // The fix: bucket each Arachne ExtrusionLine into its containing island
    // via point-in-polygon on the first junction. Each island bucket keeps
    // its internal inset-major order (which IS optimal within one island)
    // and is appended to g.loops as a separate top-level entity with
    // no_sort=true. The outer g.loops collection is sorted normally, so
    // chain_extrusion_entities picks islands by proximity — matching
    // Classic's natural per-island traversal.
    //
    // Lines whose first junction falls in no island (rare numerical edge
    // case at the polygon boundary) go to a shared bucket emitted last.
    ExPolygons islands = union_ex(outline_polys);
    const size_t n_islands = islands.size();
    std::vector<ExtrusionEntityCollection> per_island(n_islands);
    ExtrusionEntityCollection               shared_bucket;

    auto find_island = [&](const Arachne::ExtrusionLine& el) -> size_t {
        if (n_islands == 0 || el.junctions.empty()) return n_islands;
        const Point& p = el.junctions.front().p;
        for (size_t i = 0; i < n_islands; ++i)
            if (islands[i].contains(p)) return i;
        return n_islands;  // sentinel = shared bucket
    };

    size_t lines_emitted = 0;
    size_t odd_lines     = 0;

    for (const Arachne::VariableWidthLines& perimeter : perimeters) {
        for (const Arachne::ExtrusionLine& ext_line : perimeter) {
            if (ext_line.empty())
                continue;

            // NEOTKO_NEOARACHNE_TAG fase3-bugfix-blobs
            //
            // Role discrimination by is_odd — the key fix for the over-extrusion /
            // blob accumulation bug seen in s93 testing. Background:
            //
            // Arachne emits two kinds of ExtrusionLines along each inset:
            //   • is_odd == false → "main" beads that form closed loops, behaving
            //     as conventional inner perimeters (long, predictable, nominal width).
            //   • is_odd == true  → variable-width closure beads (short open
            //     polylines) that fill the medial-axis residual between adjacent
            //     beads. Functionally they're EXACTLY equivalent to Classic's
            //     gap_infill output: short paths of variable width depositing
            //     material in the residual between perimeters.
            //
            // Until s93 we tagged every Arachne path as erPerimeter. That made
            // Slic3r route them through the inner_wall pipeline (200 mm/s in the
            // test profile + the user's PA/retract tuning calibrated for typical
            // 10–30 mm inner-wall polylines). The result: 1–5 mm is_odd polylines
            // at 200 mm/s with full retract+wipe cycles between them produced
            // boluses of material at every start/stop, visible as blobs around
            // the central hole and at bowl↔leg junctions in the threestooges
            // test (see memory/MEMORY.md s93 cascade).
            //
            // The fix is structural and 1 line: tag is_odd beads as erGapFill so
            // Slic3r:
            //   1. Routes them through gap_infill_speed (≈70 mm/s typical) instead
            //      of inner_wall_speed.
            //   2. Applies the gap_infill retract heuristics (separate calibration
            //      tuned for short paths).
            //   3. Emits TYPE:Gap_infill in the gcode so the user's firmware can
            //      apply per-feature PA / acceleration if configured.
            //   4. Reports them under "Gap infill" in Orca's stats and visualizer,
            //      which matches their physical role.
            //
            // is_odd beads from Arachne are always OPEN polylines by construction
            // (closure tails of skeleton transitions), so they never reach the
            // ExtrusionLoop branch below — they all flow through ExtrusionMultiPath.
            const ExtrusionRole role = ext_line.is_odd ? erGapFill : erPerimeter;
            ExtrusionPaths      paths;
            extrusion_paths_append(paths, ext_line, role, g.perimeter_flow);
            if (paths.empty())
                continue;

            const int inset_idx = int(ext_line.inset_idx);
            for (ExtrusionPath& p : paths) {
                p.inset_idx = inset_idx;
                // NEOTKO_NEOARACHNE_TAG s93 — opt every NeoArachne path out of
                // SpiralLift. Auto Lift's overhang detector misreads dense
                // Arachne path clusters (around holes, junctions) as overhang
                // and emits G3 helical lifts whose XY arc smears residual ooze
                // in a circle = the characteristic spiral blob. GCode.cpp
                // reads this flag in needs_retraction and downgrades to
                // LazyLift. Classic paths (with default false) keep Auto Lift
                // behavior intact.
                p.force_no_spiral_lift = true;
            }

            const size_t bucket_idx = find_island(ext_line);
            ExtrusionEntityCollection& dst = (bucket_idx < n_islands)
                                             ? per_island[bucket_idx]
                                             : shared_bucket;

            if (ext_line.is_closed) {
                ExtrusionLoop loop(std::move(paths), elrDefault);
                // NEOTKO_NEOARACHNE_TAG s94 task#13 — preserve hole/contour
                // semantics so the seam placer applies the correct policy.
                // Arachne emits CW for holes and CCW for contours (mirrors
                // the input polygon orientation). Classic tags loops with
                // elrHole vs elrDefault based on this; without it the seam
                // placer treats every loop as a contour → visible artefacts
                // (seam picked from contour-only candidates inside holes,
                // wrong start point on inset 0 walls around holes).
                // make_counter_clockwise() destroys the orientation info,
                // so we sample it BEFORE forcing CCW.
                const bool is_hole = loop.is_clockwise();
                loop.make_counter_clockwise();
                loop.set_loop_role(is_hole ? elrHole : elrDefault);
                loop.inset_idx = inset_idx;
                dst.append(std::move(loop));
            } else {
                // Open ExtrusionLine (typical for is_odd gap-fill segments).
                ExtrusionMultiPath mp;
                mp.paths.reserve(paths.size());
                mp.paths.emplace_back(std::move(paths.front()));
                mp.inset_idx = inset_idx;
                for (auto it = std::next(paths.begin()); it != paths.end(); ++it) {
                    if (mp.paths.back().last_point() != it->first_point()) {
                        dst.append(ExtrusionMultiPath(std::move(mp)));
                        mp = ExtrusionMultiPath();
                        mp.inset_idx = inset_idx;
                    }
                    mp.paths.emplace_back(std::move(*it));
                }
                dst.append(ExtrusionMultiPath(std::move(mp)));
            }

            ++lines_emitted;
            if (ext_line.is_odd)
                ++odd_lines;
        }
    }

    // Append each island bucket as a top-level entity in g.loops with
    // no_sort=true so the internal inset-major order survives
    // chain_extrusion_entities. The OUTER g.loops collection keeps its
    // default sort behaviour — that's what picks islands by proximity at
    // emission time, exactly mirroring Classic's natural per-island chain.
    for (ExtrusionEntityCollection& bucket : per_island) {
        if (bucket.empty()) continue;
        bucket.no_sort = true;
        g.loops->append(bucket);
    }
    if (!shared_bucket.empty()) {
        shared_bucket.no_sort = true;
        g.loops->append(shared_bucket);
    }

    log_dispatch(Runtime::get().cfg, g.layer_id, /*region_id=*/-1,
                 "phase1/interior-arachne-emitted");

    // ── Compute the inner contour the CALLER uses to clip fill_surfaces ─────
    //
    // Arachne's getInnerContour() returns polygons taken from the synthetic
    // "0-width contour walls" SkeletalTrapezoidation injects to mark the
    // inner boundary. In practice it can return EMPTY in legitimate cases
    // (thin regions, low inset_count, variable beading that didn't trigger
    // contour synthesis). Earlier s91 bug: when that happened, Plan::run
    // skipped the fill_surfaces clip and infill ended up overlapping the
    // Arachne walls → catastrophic double extrusion on top/bottom layers.
    //
    // Fix: ALWAYS return a non-empty bound. Prefer Arachne's reported contour
    // (exact when it exists), fall back to a conservative shrink of the
    // residual by total wall thickness. The conservative version may under-
    // report a sliver of infill area in pathological cases but never
    // overlaps walls.
    const Polygons& arachne_reported = wtp.getInnerContour();
    if (!arachne_reported.empty())
        return union_ex(arachne_reported);

    // Fallback: offset residual inward by inset_count × perimeter_spacing.
    // That's the inner edge of the innermost wall assuming uniform spacing,
    // which matches what Arachne does when there's room. Where there isn't
    // room the offset returns empty → fill_surfaces gets cleared and no
    // infill is laid down (matching the "lose a sliver" trade-off).
    return offset_ex(union_ex(outline_polys),
                     -float(inset_count) * float(perimeter_spacing));
}

}} // namespace Slic3r::NeoArachne
