// NEOTKO_NEOARACHNE_TAG preview-lab PL.5
#include "PreviewSlicer.hpp"
#include "PreviewConfigSnapshot.hpp"
#include "PreviewGeometrySource.hpp"

#include "../NeoArachnePlan.hpp"

#include "../../PerimeterGenerator.hpp"
#include "../../SurfaceCollection.hpp"
#include "../../Surface.hpp"
#include "../../ExtrusionEntityCollection.hpp"
#include "../../ExtrusionEntity.hpp"
#include "../../ClipperUtils.hpp"
#include "../../Layer.hpp"
#include "../../Polygon.hpp"
#include "../../ExPolygon.hpp"
#include "../../ShortestPath.hpp"
#include "../../libslic3r.h"

#include <exception>
#include <functional>
#include <iterator>
#include <set>
#include <utility>

namespace Slic3r { namespace NeoArachne { namespace Preview {

namespace {

// Walks a (possibly nested) ExtrusionEntityCollection and accumulates the
// metrics the overlay reports. Closures are tallied as the count of
// ExtrusionMultiPath instances — NeoArachne::Interior emits one per is_odd
// Arachne line, so the proxy is exact for the Hybrid v2 path.
struct WalkAccum {
    double          total_scaled_length = 0.0;
    size_t          multipath_count     = 0;
    std::set<int>   inset_indices;
};

void walk(const ExtrusionEntityCollection& coll, WalkAccum& a);

void walk_entity(const ExtrusionEntity& e, WalkAccum& a)
{
    // inset_idx lives on the polymorphic base — no cast needed for it.
    a.inset_indices.insert(e.inset_idx);
    if (const auto* sub = dynamic_cast<const ExtrusionEntityCollection*>(&e)) {
        walk(*sub, a);
        return;
    }
    a.total_scaled_length += e.length();
    if (dynamic_cast<const ExtrusionMultiPath*>(&e) != nullptr)
        ++a.multipath_count;
}

void walk(const ExtrusionEntityCollection& coll, WalkAccum& a)
{
    for (const ExtrusionEntity* e : coll.entities)
        if (e != nullptr) walk_entity(*e, a);
}

// Inline reimplementation of PrintObject::_shrink_contour_holes for the
// preview pipeline. The original lives on PrintObject so we can't reuse it
// (no PrintObject exists during a preview slice). We apply the FULL
// compensation here — unlike the real pipeline which splits positive vs.
// negative deltas (positive already baked in during slicing), the preview
// builds geometry from a 2D contour (W/Wedge) or a raw mesh slice that has
// no compensation applied yet, so the entire delta must come through here.
ExPolygons apply_xy_compensation(const ExPolygons& polys, float contour_scaled, float hole_scaled)
{
    if (contour_scaled == 0.f && hole_scaled == 0.f) return polys;

    ExPolygons out;
    for (const ExPolygon& ex : polys) {
        Polygons contours;
        if (contour_scaled != 0.f) {
            Polygons nc = offset(ex.contour, contour_scaled);
            if (nc.empty()) continue;  // contour collapsed under negative compensation
            contours.insert(contours.end(),
                            std::make_move_iterator(nc.begin()),
                            std::make_move_iterator(nc.end()));
        } else {
            contours.push_back(ex.contour);
        }
        Polygons holes;
        for (const Polygon& h : ex.holes) {
            if (hole_scaled != 0.f) {
                for (Polygon& nh : offset(h, -hole_scaled)) {
                    nh.make_counter_clockwise();
                    holes.emplace_back(std::move(nh));
                }
            } else {
                holes.push_back(h);
                holes.back().make_counter_clockwise();
            }
        }
        ExPolygons piece = diff_ex(union_(contours), union_(holes));
        out.insert(out.end(),
                   std::make_move_iterator(piece.begin()),
                   std::make_move_iterator(piece.end()));
    }
    return out;
}

Metrics compute_metrics(const PreviewResult& r)
{
    WalkAccum a;
    walk(r.loops, a);

    Metrics m;
    m.total_wall_mm  = unscale<double>(coord_t(a.total_scaled_length));
    m.closures_count = a.multipath_count;
    m.bead_count_avg = double(a.inset_indices.size());
    for (const Surface& s : r.fill_surfaces.surfaces)
        m.total_fill_mm2 += unscale<double>(unscale<double>(s.expolygon.area()));
    return m;
}

// v3 — compute the print-order chain of OrderedSegments. Replicates what the
// real GCode emit pipeline does (chain optimizer at the top-level entity level
// then walk per-entity in stored order) so the head-animation/travel preview
// matches what the printer will actually do — modulo the seam_placer which
// requires Print/Layer state we don't reconstruct in the preview. The seam
// dots therefore mark the FIRST POINT OF EACH LOOP AS EMITTED by NeoArachne;
// the real seam_placer may rotate that start in the final G-code. Close
// enough for "does the seam land in a reasonable spot" diagnostics — explicit
// real-seam integration is a future task that needs Print-mock work.
void compute_print_order(PreviewResult& r)
{
    // Top-level entities from both loops and gap_fill, chained as a single
    // optimisation problem. The real pipeline interleaves them per-region;
    // for preview purposes concatenating then re-chaining is a good proxy.
    std::vector<ExtrusionEntity*> chain;
    chain.reserve(r.loops.entities.size() + r.gap_fill.entities.size());
    for (ExtrusionEntity* e : r.loops.entities)    if (e) chain.push_back(e);
    for (ExtrusionEntity* e : r.gap_fill.entities) if (e) chain.push_back(e);
    if (chain.empty()) return;

    const Point origin(0, 0);
    chain_and_reorder_extrusion_entities(chain, &origin);

    Point  cursor    = origin;
    double cum       = 0.0;

    auto push_seg = [&](const Point& a, const Point& b, bool travel,
                        ExtrusionRole role, float width, bool from_mp) {
        if (a == b) return;
        OrderedSegment seg;
        seg.from           = a;
        seg.to             = b;
        seg.length_scaled  = (b - a).cast<double>().norm();
        seg.cum_start      = cum;
        seg.is_travel      = travel;
        seg.role           = role;
        seg.path_width     = width;
        seg.from_multipath = from_mp;
        cum += seg.length_scaled;
        r.total_chain_scaled += seg.length_scaled;
        if (!travel) r.total_length_scaled += seg.length_scaled;
        r.ordered_segments.push_back(std::move(seg));
    };

    // Recursive walk: ExtrusionEntityCollections may be nested (per-island
    // wrappers with no_sort=true post-s94 task #14). Within a collection we
    // honour the stored order.
    std::function<void(const ExtrusionEntity&)> walk_chain = [&](const ExtrusionEntity& e) {
        if (const auto* coll = dynamic_cast<const ExtrusionEntityCollection*>(&e)) {
            for (const ExtrusionEntity* c : coll->entities)
                if (c) walk_chain(*c);
            return;
        }

        // Leaf: assemble its polyline points sequence + role/width.
        Points        pts;
        ExtrusionRole role     = e.role();
        float         width    = 0.f;
        bool          from_mp  = false;
        bool          is_loop  = false;

        if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&e)) {
            for (const ExtrusionPath& p : loop->paths) {
                const Points& pp = p.polyline.points;
                if (pp.empty()) continue;
                if (pts.empty()) pts = pp;
                else for (size_t i = 1; i < pp.size(); ++i) pts.push_back(pp[i]);
                if (p.width > width) width = p.width;
            }
            // Close the loop visually (the real emit also draws back to start
            // for closed loops; the GCode emitter inserts the closing move
            // even if the paths vector doesn't already wrap around).
            if (pts.size() >= 2 && pts.front() != pts.back())
                pts.push_back(pts.front());
            is_loop = true;
        } else if (const auto* mp = dynamic_cast<const ExtrusionMultiPath*>(&e)) {
            for (const ExtrusionPath& p : mp->paths) {
                const Points& pp = p.polyline.points;
                if (pp.empty()) continue;
                if (pts.empty()) pts = pp;
                else for (size_t i = 1; i < pp.size(); ++i) pts.push_back(pp[i]);
                if (p.width > width) width = p.width;
            }
            from_mp = true;
        } else if (const auto* path = dynamic_cast<const ExtrusionPath*>(&e)) {
            pts   = path->polyline.points;
            width = path->width;
        }

        if (pts.size() < 2) return;

        // Travel from where we are now to the start of this leaf.
        if (cursor != pts.front())
            push_seg(cursor, pts.front(), /*travel=*/true, role, 0.f, false);

        // Loops contribute one seam dot at their start point (post-emit, pre-
        // seam_placer — approximation; see compute_print_order docs above).
        if (is_loop)
            r.seam_points.push_back(pts.front());

        // Extrusion segments in stored order.
        for (size_t i = 1; i < pts.size(); ++i)
            push_seg(pts[i-1], pts[i], /*travel=*/false, role, width, from_mp);

        cursor = pts.back();
    };

    for (const ExtrusionEntity* e : chain)
        if (e) walk_chain(*e);
}

} // namespace

PreviewResult preview_slice(const ConfigSnapshot& snap, const PreviewGeometrySource& src)
{
    PreviewResult r;

    try {
        GeometryBuildResult geom = build_surface_collection(src);
        if (!geom.surfaces) {
            r.error = geom.error.empty() ? "preview: geometry source returned empty" : geom.error;
            return r;
        }
        SurfaceCollection slices = std::move(*geom.surfaces);
        if (slices.empty()) {
            r.error = "preview: geometry source produced empty surfaces";
            return r;
        }

        // Apply xy_contour_compensation + xy_hole_compensation BEFORE feeding
        // the slices to PerimeterGenerator — that's where the real pipeline
        // applies them too, in PrintObjectSlice's make_slices(). Otherwise
        // the preview diverges from the actual slice whenever the user has
        // non-zero compensation set.
        {
            const float xy_c = scaled<float>(snap.object.xy_contour_compensation.value);
            const float xy_h = scaled<float>(snap.object.xy_hole_compensation.value);
            if (xy_c != 0.f || xy_h != 0.f) {
                ExPolygons in;
                in.reserve(slices.surfaces.size());
                for (const Surface& s : slices.surfaces) in.push_back(s.expolygon);
                ExPolygons compd = apply_xy_compensation(in, xy_c, xy_h);
                if (compd.empty()) {
                    r.error = "preview: XY compensation collapsed the slice";
                    return r;
                }
                slices.surfaces.clear();
                slices.surfaces.reserve(compd.size());
                for (ExPolygon& ex : compd)
                    slices.surfaces.emplace_back(stInternal, std::move(ex));
            }
        }

        // fix #4B (s96): the offset() inside apply_xy_compensation (Clipper
        // arc rounding) sometimes returns polygons with co-located consecutive
        // vertices. The real slicer's make_slices() does a final simplify
        // pass on the assembled slice that eliminates them. Without that,
        // Arachne's SkeletalTrapezoidation can see "two edges sharing an
        // endpoint that's actually one point" as a degenerate junction and
        // produce different bead emission than the real pipeline. Apply
        // douglas_peucker with a sub-resolution tolerance (1µm) — this
        // removes near-duplicates without altering the geometry the user
        // sees. Cheap (O(n)) per slice.
        for (Surface& s : slices.surfaces)
            s.expolygon.douglas_peucker(scaled<double>(0.001));

        LayerRegionPtrs           empty_compatible;  // PerimeterGenerator stores a pointer; empty vector is safe.
        ExtrusionEntityCollection loops, gap_fill;
        SurfaceCollection         fill_surfaces;
        ExPolygons                fill_no_overlap;

        // Seed fill_surfaces from the (post-compensation) slices as internal
        // — mirrors what LayerRegion::prepare_fill_surfaces hands to
        // PerimeterGenerator in the real pipeline. process_classic will
        // trim and reshape it.
        for (const Surface& s : slices.surfaces)
            fill_surfaces.surfaces.emplace_back(stInternal, s.expolygon);

        PerimeterGenerator g(
            &slices, &empty_compatible,
            snap.layer_height, /*slice_z=*/coordf_t(1.0),
            snap.perimeter_flow,
            &snap.region, &snap.object, &snap.print,
            /*spiral_mode=*/false,
            &loops, &gap_fill, &fill_surfaces, &fill_no_overlap);

        // fix #2 (s96): honour the panel's heuristic. For W/Wedge built-ins
        // this stays at the legacy 5 (safe middle). For FromMesh snapshots
        // the panel computes int(slice_z/layer_height) so top-layer behaviour
        // (only_one_wall_top, top-layer fill detection) finally matches
        // what the real slicer does at that Z. upper_slices/lower_slices
        // remain nullptr because reconstructing them would require full
        // PrintObject context — that limits overhang detection accuracy,
        // but not the wall-emission logic that gated on layer_id.
        g.layer_id                   = std::max(0, snap.effective_layer_id);
        g.ext_perimeter_flow         = snap.ext_perimeter_flow;
        g.overhang_flow              = snap.overhang_flow;
        g.solid_infill_flow          = snap.solid_infill_flow;
        g.smaller_ext_perimeter_flow = snap.smaller_ext_perimeter_flow;
        g.upper_slices               = nullptr;
        g.lower_slices               = nullptr;
        g.upper_slices_same_region   = nullptr;

        NeoArachne::Plan::run(g);

        r.bbox            = get_extents(slices.surfaces);
        r.loops           = loops;
        r.gap_fill        = gap_fill;
        r.fill_surfaces   = fill_surfaces;
        r.fill_no_overlap = fill_no_overlap;
        // fix #3 (s96): capture the post-XY-compensation slices so the dump
        // can show what Arachne actually sees. Cheap copy — typically ≤4 ExPolys
        // for a single-region preview.
        r.input_slices.reserve(slices.surfaces.size());
        for (const Surface& s : slices.surfaces)
            r.input_slices.push_back(s.expolygon);
        r.metrics         = compute_metrics(r);
        compute_print_order(r);   // v3 — fills ordered_segments / seam_points / total_*_scaled
        r.ok              = true;
    } catch (const std::exception& ex) {
        r.error = std::string("preview: exception — ") + ex.what();
        r.ok    = false;
    } catch (...) {
        r.error = "preview: unknown exception";
        r.ok    = false;
    }

    return r;
}

}}} // namespace Slic3r::NeoArachne::Preview
