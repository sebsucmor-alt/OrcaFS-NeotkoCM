// NEOTKO_SNAPDRAG_TAG s227 — see GravitySnap.hpp and
// docs/FUTURE/GRAVITY_SNAP_AND_DRAG_PLAN.md for the full design.

#include "GravitySnap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "3DScene.hpp"
#include "GUI_App.hpp"
#include "MeshUtils.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace GUI {
namespace GravitySnap {

namespace {

// One instance's world-space footprint (2D convex hull, scaled units) and Z extent
// (unscaled mm), built from every non-modifier, non-wipe-tower GLVolume that belongs to it.
struct InstanceFootprint
{
    Polygon hull;
    double  min_z = 0.0;
    double  max_z = 0.0;
    bool    valid = false;
};

InstanceFootprint compute_footprint(const GLVolumeCollection &volumes, int object_idx, int instance_idx)
{
    InstanceFootprint fp;
    Points pts;
    BoundingBoxf3 bbox;
    bool has_any = false;

    for (const GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->is_wipe_tower || v->is_modifier)
            continue;
        if (v->object_idx() != object_idx || v->instance_idx() != instance_idx)
            continue;

        const TriangleMesh *hull = v->convex_hull();
        if (hull == nullptr || hull->its.vertices.empty())
            continue;

        const Transform3d matrix = v->world_matrix();
        pts.reserve(pts.size() + hull->its.vertices.size());
        for (const auto &vertex : hull->its.vertices) {
            const Vec3d p = matrix * vertex.cast<double>();
            pts.emplace_back(coord_t(scale_(p.x())), coord_t(scale_(p.y())));
        }

        if (has_any)
            bbox.merge(v->transformed_convex_hull_bounding_box());
        else
            bbox = v->transformed_convex_hull_bounding_box();
        has_any = true;
    }

    if (!has_any || pts.empty())
        return fp;

    fp.hull  = Slic3r::Geometry::convex_hull(pts);
    fp.min_z = bbox.min.z();
    fp.max_z = bbox.max.z();
    fp.valid = !fp.hull.empty();
    return fp;
}

// Up to 5 sample points (world mm) inside `poly`: its centroid plus the midpoint between the
// centroid and up to 4 of its own vertices. Using the polygon's OWN vertices (not its bounding
// box) guarantees every point stays inside `poly` for a non-axis-aligned/rotated shape too,
// since `poly` is convex (intersection of two convex hulls) and centroid + any point of a
// convex region stays inside that region.
//
// This exists so a hollow box (tall rim, low interior floor) can be told apart from a solid
// block of the same footprint: the flat convex-hull top used everywhere else in this file
// can't see the hole, only a handful of real raycasts through the actual mesh can.
std::vector<Vec2d> sample_points_mm(const ExPolygon &poly)
{
    std::vector<Vec2d> pts;
    if (poly.contour.points.empty())
        return pts;

    const Point   c_scaled = poly.contour.centroid();
    const Vec2d   c(unscale_(c_scaled.x()), unscale_(c_scaled.y()));
    pts.push_back(c);

    // NEOTKO_SNAPDRAG_TAG s233 — pulled in from 0.5 to 0.25 of the way to each vertex. The points
    // are drawn on screen now (see SnapDragIndicator), and spread wide they read as four unrelated
    // marks; clustered they read as one clear "this is the spot being measured" marker. Detection
    // is unaffected in practice: `poly` here is an intersection of two convex hulls, so it is
    // convex and hole-free, and any point between its centroid and a vertex is equally inside it.
    const Points &verts = poly.contour.points;
    const size_t  step  = std::max<size_t>(1, verts.size() / 4);
    for (size_t i = 0; i < verts.size() && pts.size() < 5; i += step) {
        const Vec2d v(unscale_(verts[i].x()), unscale_(verts[i].y()));
        pts.push_back(c + 0.25 * (v - c));
    }
    return pts;
}

// Real surface Z (world mm) under `pts_mm`, sampled by raycasting straight down through the
// instance's ACTUAL geometry (GLVolume::mesh_raycaster, built from the real mesh — not the
// convex hull used everywhere else in this file). `above_z` must be at or above the instance's
// own flat top so every ray starts outside the mesh.
//
// - If none of the instance's volumes carry a raycaster (defensive: some exotic volume type),
//   falls back to `flat_fallback` so the feature degrades to the old flat-top behaviour instead
//   of silently going inert.
// - If raycasters ARE available but not a single sample point hits anything, returns nullopt:
//   that XY spot is genuinely a hole in the geometry (e.g. the middle of a hollow box) and must
//   NOT be treated as a floor.
// - Otherwise returns the HIGHEST real hit among all sample points and all of the instance's
//   volumes (multi-part instances are the union of their parts).
//
// NEOTKO_SNAPDRAG_TAG s233 — `out_hits`, when non-null, collects every world-mm point that was
// actually hit (one per sample point per volume). The landing overlay draws them so "why did it
// not catch that thin rim" is answerable by looking at the screen. Purely informational: it never
// affects the returned Z.
std::optional<double> sample_real_top_z(const GLVolumeCollection &volumes, int object_idx, int instance_idx,
                                         const std::vector<Vec2d> &pts_mm, double above_z, double flat_fallback,
                                         std::vector<Vec3d> *out_hits = nullptr)
{
    bool   any_raycaster = false;
    bool   found_hit     = false;
    double best          = -std::numeric_limits<double>::infinity();

    for (const GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->is_wipe_tower || v->is_modifier)
            continue;
        if (v->object_idx() != object_idx || v->instance_idx() != instance_idx)
            continue;
        if (!v->mesh_raycaster)
            continue;
        any_raycaster = true;

        const Transform3d world = v->world_matrix();
        const Transform3d inv   = world.inverse();
        const Vec3d local_dir   = (inv.linear() * Vec3d(0.0, 0.0, -1.0)).normalized();
        const AABBMesh &aabb    = v->mesh_raycaster->get_aabb_mesh();

        for (const Vec2d &p : pts_mm) {
            const Vec3d world_source(p.x(), p.y(), above_z);
            const AABBMesh::hit_result hit = aabb.query_ray_hit(inv * world_source, local_dir);
            if (!hit.is_hit())
                continue;

            const Vec3d  world_hit = world * hit.position();
            const double world_z   = world_hit.z();
            if (out_hits != nullptr)
                out_hits->push_back(world_hit);
            if (!found_hit || world_z > best) {
                best      = world_z;
                found_hit = true;
            }
        }
    }

    if (found_hit)
        return best;
    return any_raycaster ? std::nullopt : std::optional<double>(flat_fallback);
}

} // namespace

bool enabled()
{
    if (!gravity_allow_free_z())
        return false;
    const AppConfig *ac = wxGetApp().app_config;
    return ac != nullptr && ac->get_bool("neotko_snap_drag");
}

// NEOTKO_SNAPDRAG_TAG s233 — see the header. DEFAULT ON via the has() guard rather than a seed in
// AppConfig::set_defaults(): set_defaults() also runs on empty storage from the constructor,
// BEFORE the ini is merged, so a seeded key there is indistinguishable from a user-written one
// later (this is exactly the trap documented around the neotko_true_objects migration). Reading
// "absent == true" right here needs no migration and cannot be raced by the load order.
bool bed_is_floor()
{
    const AppConfig *ac = wxGetApp().app_config;
    if (ac == nullptr)
        return true;
    return ac->has("neotko_snap_drag_bed") ? ac->get_bool("neotko_snap_drag_bed") : true;
}

// NEOTKO_SNAPDRAG_TAG s249 — see the header. Default OFF, so a plain get_bool() (absent == false)
// is already the right default and no has() guard is needed here, unlike bed_is_floor() above.
bool move_as_group()
{
    const AppConfig *ac = wxGetApp().app_config;
    return ac != nullptr && ac->get_bool("neotko_snap_drag_group");
}

// NEOTKO_SNAPDRAG_TAG s249 — see the header.
bool& panel_open()
{
    static bool s_open = false;
    return s_open;
}

// NEOTKO_SNAPDRAG_TAG s249 — see the header for why this is LibreMode and not True Objects.
bool plate_icon_available()
{
    // app_config is null very early in startup and again during teardown, and the render paths
    // that ask this run in both windows.
    const AppConfig *ac = wxGetApp().app_config;
    return ac != nullptr && ac->get_bool("neotko_libre_mode");
}

std::optional<FloorHit> floor_z_for_instance(const GLVolumeCollection &volumes,
                                             int object_idx, int instance_idx,
                                             const std::set<std::pair<int, int>> &moving,
                                             double engage_ratio)
{
    const InstanceFootprint mine = compute_footprint(volumes, object_idx, instance_idx);
    if (!mine.valid)
        return std::nullopt;

    const double mine_area = std::abs(mine.hull.area());
    if (mine_area <= 0.0)
        return std::nullopt;

    std::set<std::pair<int, int>> seen;
    bool     found = false;
    FloorHit best;

    for (const GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->is_wipe_tower || v->is_modifier)
            continue;

        const std::pair<int, int> id(v->object_idx(), v->instance_idx());
        if (id.first == object_idx && id.second == instance_idx)
            continue; // never my own instance
        if (moving.count(id) != 0)
            continue; // part of the same drag — plan header, never rest on your own group
        if (!seen.insert(id).second)
            continue; // multi-volume instance already evaluated

        const InstanceFootprint cand = compute_footprint(volumes, id.first, id.second);
        if (!cand.valid)
            continue;

        const ExPolygons inter = intersection_ex(Polygons{ mine.hull }, Polygons{ cand.hull });
        double inter_area = 0.0;
        for (const ExPolygon &ep : inter)
            inter_area += ep.area();

        const double overlap_ratio = inter_area / mine_area;
        if (overlap_ratio < engage_ratio) {
            if (NeoDebug::enabled(NeoDebug::GRAVITY))
                NeoDebug::write(NeoDebug::GRAVITY,
                    "SNAPDRAG rejected obj=" + std::to_string(id.first) + " inst=" + std::to_string(id.second) +
                    " top_z=" + std::to_string(cand.max_z) + " overlap=" + std::to_string(overlap_ratio) +
                    " (< engage_ratio=" + std::to_string(engage_ratio) + ")");
            continue;
        }

        // Margin detection (s227 follow-up): don't trust the flat convex-hull top for this
        // candidate — sample its REAL geometry so a hollow box (tall rim, low interior floor)
        // resolves to the actual surface under the overlap, not the rim height everywhere.
        // Sample inside the LARGEST overlap region (usually the only one).
        const ExPolygon *biggest_inter = nullptr;
        double biggest_inter_area = 0.0;
        for (const ExPolygon &ep : inter) {
            const double a = ep.area();
            if (biggest_inter == nullptr || a > biggest_inter_area) {
                biggest_inter = &ep;
                biggest_inter_area = a;
            }
        }

        std::vector<Vec3d>          cand_hits;
        const std::optional<double> cand_top = (biggest_inter != nullptr)
            ? sample_real_top_z(volumes, id.first, id.second, sample_points_mm(*biggest_inter), cand.max_z + 1.0, cand.max_z, &cand_hits)
            : std::optional<double>(cand.max_z); // defensive: overlap_ratio passed, so `inter` can't be empty here

        if (!cand_top.has_value()) {
            // Real geometry has a hole exactly under the overlap (e.g. dragging over the open
            // middle of a hollow box) — this candidate is not a floor at this position.
            if (NeoDebug::enabled(NeoDebug::GRAVITY))
                NeoDebug::write(NeoDebug::GRAVITY,
                    "SNAPDRAG rejected obj=" + std::to_string(id.first) + " inst=" + std::to_string(id.second) +
                    " (real geometry has no surface under the overlap — hollow interior)");
            continue;
        }

        // Highest overlapping surface wins: dropping something over a stack rests it on the
        // TALLEST thing under its footprint, never sinks it into a shorter neighbour that also
        // happens to overlap (this is the "find the highest surface nearby" the feature exists
        // for). NOTE: this is a different aggregation level from the multi-instance case in
        // do_move/GLCanvas3D, where the lowest of several PILLARS is deliberately used instead —
        // that one is about not over-lifting a whole object because one of its instances landed
        // on a taller pillar than the others (explicit user call).
        if (!found || *cand_top > best.z) {
            best.z        = *cand_top;
            best.is_bed   = false;
            best.obj_idx  = id.first;
            best.inst_idx = id.second;
            best.contact  = (biggest_inter != nullptr) ? *biggest_inter : ExPolygon(mine.hull);
            best.samples  = std::move(cand_hits);
            found         = true;
            if (NeoDebug::enabled(NeoDebug::GRAVITY))
                NeoDebug::write(NeoDebug::GRAVITY,
                    "SNAPDRAG candidate obj=" + std::to_string(id.first) + " inst=" + std::to_string(id.second) +
                    " real_top_z=" + std::to_string(*cand_top) + " flat_top_z=" + std::to_string(cand.max_z) +
                    " overlap=" + std::to_string(overlap_ratio));
        }
    }

    // NEOTKO_SNAPDRAG_TAG s233 — the bed as last-place candidate (only when Allow Bed is on).
    // Deliberately evaluated AFTER the loop and never compared against the others: its Z is 0 and
    // the aggregation above is "highest wins", so it could never outrank a real object anyway.
    // Doing it this way keeps every s227 case byte-identical and makes the fallback one branch.
    // The recognised zone is the instance's whole footprint — on the bed, all of it is supported.
    //
    // No plate-boundary test on purpose: an object dragged off the plate still lands on Z=0
    // rather than floating, which is what anyone coming from a normal slicer expects, and
    // out-of-plate is already flagged as unprintable through its own channel. See
    // docs/FUTURE/GRAVITY_SNAP_AND_DRAG_V2_PLAN.md §1.1.
    if (!found && bed_is_floor()) {
        best.z        = 0.0;
        best.is_bed   = true;
        best.obj_idx  = -1;
        best.inst_idx = -1;
        best.contact  = ExPolygon(mine.hull);
        best.samples.clear();
        found = true;
    }

    if (NeoDebug::enabled(NeoDebug::GRAVITY))
        NeoDebug::write(NeoDebug::GRAVITY,
            "SNAPDRAG result obj=" + std::to_string(object_idx) + " inst=" + std::to_string(instance_idx) +
            " my_min_z=" + std::to_string(mine.min_z) +
            (found ? (" target_z=" + std::to_string(best.z) +
                      (best.is_bed ? " (BED)" : (" on obj=" + std::to_string(best.obj_idx) +
                                                 " inst=" + std::to_string(best.inst_idx) +
                                                 " ray_hits=" + std::to_string(best.samples.size()))))
                   : std::string(" (no candidate, leave floating)")));

    if (!found)
        return std::nullopt;
    best.z = std::max(best.z, 0.0);
    return best;
}

std::optional<std::pair<int, int>> support_in_group(const GLVolumeCollection &volumes,
                                                    int object_idx, int instance_idx,
                                                    const std::set<std::pair<int, int>> &group,
                                                    double engage_ratio, double max_gap)
{
    const InstanceFootprint mine = compute_footprint(volumes, object_idx, instance_idx);
    if (!mine.valid)
        return std::nullopt;
    const double mine_area = std::abs(mine.hull.area());
    if (mine_area <= 0.0)
        return std::nullopt;

    std::optional<std::pair<int, int>> best;
    double best_top = -std::numeric_limits<double>::infinity();

    for (const std::pair<int, int> &id : group) {
        if (id.first == object_idx && id.second == instance_idx)
            continue;

        const InstanceFootprint cand = compute_footprint(volumes, id.first, id.second);
        if (!cand.valid)
            continue;

        // Must be UNDER me and close enough to count as contact. EPS_ABOVE tolerates the sub-micron
        // overshoot a previous snap leaves behind; max_gap covers hand-placed stacks that were never
        // perfectly seated.
        constexpr double EPS_ABOVE = 0.05;
        const double gap = mine.min_z - cand.max_z;
        if (cand.max_z > mine.min_z + EPS_ABOVE || gap > max_gap)
            continue;

        const ExPolygons inter = intersection_ex(Polygons{ mine.hull }, Polygons{ cand.hull });
        double inter_area = 0.0;
        for (const ExPolygon &ep : inter)
            inter_area += ep.area();
        if (inter_area / mine_area < engage_ratio)
            continue;

        if (!best.has_value() || cand.max_z > best_top) {
            best_top = cand.max_z;
            best     = id;
        }
    }

    if (best.has_value() && NeoDebug::enabled(NeoDebug::GRAVITY))
        NeoDebug::write(NeoDebug::GRAVITY,
            "SNAPDRAG group-support obj=" + std::to_string(object_idx) + " inst=" + std::to_string(instance_idx) +
            " rests on obj=" + std::to_string(best->first) + " inst=" + std::to_string(best->second) +
            " (top_z=" + std::to_string(best_top) + ")");

    return best;
}

Polygon instance_footprint(const GLVolumeCollection &volumes, int object_idx, int instance_idx)
{
    return compute_footprint(volumes, object_idx, instance_idx).hull;
}

} // namespace GravitySnap
} // namespace GUI
} // namespace Slic3r
