// NEOTKO_CONTACT_TAG s224 — C1 contact detector implementation. See header for design notes.

#include "InstanceContact.hpp"

#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/NeoDebug.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {
namespace InstanceContact {

// Erosion applied to the footprint before sampling, so that Clipper simplification
// noise (RESOLUTION = 0.0125mm) on the contours can never decide the contact result.
static constexpr double FOOTPRINT_EROSION_MM = 0.02;
// Cap on sample points per footprint — keeps the lazy path cheap on dense meshes.
static constexpr size_t MAX_SAMPLE_POINTS = 256;

static void dbg(const std::string &msg)
{
    // Lazy path only (runs when the empty-first-layer check already fired), so an
    // unconditional-when-enabled write is fine.
    if (NeoDebug::enabled(NeoDebug::CONTACT))
        NeoDebug::write(NeoDebug::CONTACT, msg);
}

// Collect up to MAX_SAMPLE_POINTS contour/hole vertices from the eroded footprint.
// Vertices of the eroded polygons are guaranteed to sit >= FOOTPRINT_EROSION_MM inside
// the real silhouette, so containment tests against another object's lslices are not
// polluted by boundary noise.
static Points sample_footprint(const ExPolygons &footprint)
{
    size_t total = 0;
    for (const ExPolygon &ex : footprint) {
        total += ex.contour.points.size();
        for (const Polygon &hole : ex.holes)
            total += hole.points.size();
    }
    const size_t stride = std::max<size_t>(1, total / MAX_SAMPLE_POINTS);
    Points pts;
    pts.reserve(std::min(total, MAX_SAMPLE_POINTS + 1));
    size_t idx = 0;
    auto take = [&](const Points &src) {
        for (const Point &p : src)
            if (idx++ % stride == 0)
                pts.push_back(p);
    };
    for (const ExPolygon &ex : footprint) {
        take(ex.contour.points);
        for (const Polygon &hole : ex.holes)
            take(hole.points);
    }
    return pts;
}

// Highest print_z of `other` (object-local scaled point q) among layers containing q,
// or -inf when no layer of `other` lies under the point.
static double top_z_at(const PrintObject &other, const Point &q)
{
    // other.layers() is a ConstLayerPtrsAdaptor (forward iterators only) — walk by index, top-down.
    for (size_t li = other.layer_count(); li > 0; --li) {
        const Layer *layer = other.get_layer(int(li) - 1);
        if (layer->lslices.empty())
            continue;
        for (size_t i = 0; i < layer->lslices.size(); ++i) {
            if (i < layer->lslices_bboxes.size() && !layer->lslices_bboxes[i].contains(q))
                continue;
            if (layer->lslices[i].contains(q))
                return layer->print_z; // first hit from the top is the exposed roof at q
        }
    }
    return -std::numeric_limits<double>::infinity();
}

ObjectContact analyze_object(const PrintObject &object)
{
    ObjectContact out;
    out.instances.resize(object.instances().size());

    // First non-empty layer of the hanging object = its real lowest geometry.
    const Layer *first_layer = nullptr;
    for (const Layer *layer : object.layers())
        if (!layer->lslices.empty()) { first_layer = layer; break; }
    if (first_layer == nullptr) {
        dbg("analyze_object: object has no non-empty layer, nothing to measure");
        return out;
    }
    const double a_bottom = first_layer->bottom_z();

    // Eroded footprint (fall back to the raw silhouette for features thinner than the
    // erosion — better a slightly noisy sample than none).
    ExPolygons footprint = offset_ex(first_layer->lslices, -scale_(FOOTPRINT_EROSION_MM));
    if (footprint.empty())
        footprint = first_layer->lslices;
    const Points samples = sample_footprint(footprint);

    {
        std::ostringstream ss;
        ss << "analyze_object: obj=" << object.model_object()->name << " instances=" << object.instances().size()
           << " bottom_z=" << a_bottom << " samples=" << samples.size();
        dbg(ss.str());
    }

    for (size_t inst_idx = 0; inst_idx < object.instances().size(); ++inst_idx) {
        const PrintInstance &ai = object.instances()[inst_idx];
        InstanceResult      &res = out.instances[inst_idx];

        // Bed: the plate itself is always "under" the footprint at z=0.
        res.gap_mm = a_bottom;
        if (a_bottom <= CONTACT_GAP_THRESHOLD_MM) {
            res.supported = true;
            continue;
        }

        // Other instances on the plate. Same-object instances are skipped: they share
        // the identical layer stack, so they hang at the same height and cannot support
        // each other.
        for (const PrintObject *other : object.print()->objects()) {
            if (other == &object || other->layers().empty())
                continue;
            for (const PrintInstance &bi : other->instances()) {
                // World coords: p_local_A + shift_A == p_world; q_local_B = p_world - shift_B.
                // (trafo_centered() alone is per-object local — shift is the real bridge.)
                const Point delta = ai.shift - bi.shift;
                for (const Point &p : samples) {
                    const double top = top_z_at(*other, p + delta);
                    if (std::isinf(top))
                        continue; // no geometry of B under this point
                    const double gap = a_bottom - top;
                    res.gap_mm = std::min(res.gap_mm, gap);
                    if (gap <= CONTACT_GAP_THRESHOLD_MM) {
                        // Contact (negative gap = overlapping geometry, physically merged).
                        res.supported      = true;
                        res.support_object = other;
                        break;
                    }
                }
                if (res.supported) break;
            }
            if (res.supported) break;
        }

        {
            std::ostringstream ss;
            ss << "  instance " << inst_idx << ": supported=" << (res.supported ? "yes" : "NO")
               << " gap=" << res.gap_mm << "mm"
               << " on=" << (res.support_object ? res.support_object->model_object()->name : std::string(res.supported ? "bed" : "-"));
            dbg(ss.str());
        }
    }

    out.all_supported = true;
    for (const InstanceResult &r : out.instances) {
        if (!r.supported) {
            out.all_supported = false;
            if (std::isfinite(r.gap_mm))
                out.worst_gap_mm = std::isfinite(out.worst_gap_mm) ? std::max(out.worst_gap_mm, r.gap_mm) : r.gap_mm;
        }
    }
    return out;
}

// NEOTKO_CONTACT_TAG s224 C1.2 — per-island contact check for the sharp-tail detector.
bool island_rests_on_other_object(const PrintObject &object, const ExPolygon &island_local, double island_bottom_z)
{
    if (object.instances().empty())
        return false;

    // Same erosion rationale as analyze_object: island contours come from lslices
    // (Clipper-simplified), keep boundary noise out of the containment tests.
    ExPolygons footprint = offset_ex(ExPolygons{island_local}, -scale_(FOOTPRINT_EROSION_MM));
    if (footprint.empty())
        footprint = ExPolygons{island_local};
    const Points samples = sample_footprint(footprint);
    if (samples.empty())
        return false;

    // Every instance of the hanging object must have this island resting on some other
    // instance — a single unsupported copy still deserves the warning.
    for (const PrintInstance &ai : object.instances()) {
        bool   inst_supported = false;
        double best_gap       = std::numeric_limits<double>::infinity();
        for (const PrintObject *other : object.print()->objects()) {
            if (other == &object || other->layers().empty())
                continue;
            for (const PrintInstance &bi : other->instances()) {
                const Point delta = ai.shift - bi.shift;
                for (const Point &p : samples) {
                    const double top = top_z_at(*other, p + delta);
                    if (std::isinf(top))
                        continue;
                    const double gap = island_bottom_z - top;
                    best_gap = std::min(best_gap, gap);
                    if (gap <= CONTACT_GAP_THRESHOLD_MM) {
                        inst_supported = true;
                        break;
                    }
                }
                if (inst_supported) break;
            }
            if (inst_supported) break;
        }
        {
            std::ostringstream ss;
            ss << "island_check: obj=" << object.model_object()->name
               << " z=" << island_bottom_z << " samples=" << samples.size()
               << " supported=" << (inst_supported ? "yes" : "NO")
               << " best_gap=" << best_gap << "mm";
            dbg(ss.str());
        }
        if (!inst_supported)
            return false;
    }
    return true;
}

// NEOTKO_XOBJ_TAG s225 — A1 cross-object support avoidance: neighbor occupancy builder.

// Pair pruning margin. Perf-only: a pair of instances whose XY bboxes are farther apart
// than this can still, in theory, matter to a wandering tree branch, but the collision
// query there would need a branch leaning >50mm outside its own object — accepting that
// miss keeps plates of well-separated objects at zero cost.
static constexpr double XOBJ_PRUNE_MARGIN_MM = 50.0;

static void dbg_xobj(const std::string &msg)
{
    if (NeoDebug::enabled(NeoDebug::XOBJ))
        NeoDebug::write(NeoDebug::XOBJ, msg);
}

bool cross_object_active(const PrintObject &object)
{
    const Print *print = object.print();
    // Sequential by-object printing: the neighbor may not exist yet at a given Z, and the
    // physical head clearance is already handled elsewhere — gate to by-layer only.
    // NEOTKO_GRAVITY_TAG s226 — Fase 6.4.4: Gravity (True Objects) forces cross-object support
    // avoidance ON regardless of the per-object toggle — "it lives on; what gets saved is the
    // gravity mode". Read the config mirror directly (not Gravity::active(), to avoid a circular
    // dependency: Gravity reuses THIS module). Per-object overrides are never rewritten, so the
    // 3mf stays clean and turning Gravity off restores the saved value.
    return (object.config().support_cross_object_avoidance.value || object.config().neotko_true_objects.value)
        && print != nullptr
        && print->config().print_sequence == PrintSequence::ByLayer;
}

std::vector<Polygons> neighbor_occupancy(const PrintObject &object)
{
    const Print *print = object.print();
    if (print == nullptr || object.layer_count() == 0)
        return {};
    if (!cross_object_active(object))
        return {};

    // 2D bbox of an object's local geometry, accumulated once from the per-island bboxes.
    auto object_bbox = [](const PrintObject &po) {
        BoundingBox bb;
        for (const Layer *layer : po.layers())
            for (const BoundingBox &b : layer->lslices_bboxes)
                bb.merge(b);
        return bb;
    };

    const BoundingBox a_bbox = object_bbox(object);
    if (!a_bbox.defined)
        return {}; // all layers empty — nothing to protect
    const coord_t margin = scale_(XOBJ_PRUNE_MARGIN_MM);

    const size_t          num_layers = object.layer_count();
    std::vector<Polygons> occupancy(num_layers);
    bool                  any = false;

    for (const PrintObject *other : print->objects()) {
        if (other == &object || other->layer_count() == 0)
            continue;

        // NEOTKO_XOBJ_TAG s225 A2 — the neighbor's ALREADY GENERATED support is an
        // obstacle too (tree-vs-tree entanglement). Only present when the neighbor's
        // posSupportMaterial is done; deterministic because opted-in objects generate
        // their support serially in plate order (Print.cpp) after the parallel batch.
        // NOTE: SupportLayer::lslices only carries the brim/skirt-avoidance outline of the
        // first layers — the printed tree geometry lives in base/roof/floor areas (and
        // support_islands for the classic engine), so gather those.
        std::vector<std::pair<const Layer*, Polygons>> b_support;
        if (other->is_step_done(posSupportMaterial))
            for (const SupportLayer *sl : other->support_layers()) {
                Polygons geo = to_polygons(sl->support_islands);
                append(geo, to_polygons(sl->base_areas));
                append(geo, to_polygons(sl->tree_roof_areas()));
                append(geo, to_polygons(sl->tree_roof_1st_layer()));
                append(geo, to_polygons(sl->tree_floor_areas()));
                if (!geo.empty())
                    b_support.emplace_back(sl, std::move(geo));
            }

        // XY deltas (B-local -> A-local frame: local_B + shift_B - shift_A) that survive
        // bbox pruning. Conservative multi-instance union: every copy of A contributes
        // its own relative view of every copy of B, because the support is generated once
        // per object and replicated per instance (preplan §3 A1.3).
        BoundingBox b_bbox = object_bbox(*other);
        for (const auto &sl : b_support)
            b_bbox.merge(get_extents(sl.second));
        if (!b_bbox.defined)
            continue;
        std::vector<Point> deltas;
        for (const PrintInstance &ai : object.instances())
            for (const PrintInstance &bi : other->instances()) {
                const Point delta = bi.shift - ai.shift;
                BoundingBox b_in_a = b_bbox;
                b_in_a.translate(delta.x(), delta.y());
                b_in_a.offset(margin);
                if (b_in_a.overlap(a_bbox))
                    deltas.push_back(delta);
            }
        {
            std::ostringstream ss;
            ss << "neighbor_occupancy: A=" << object.model_object()->name << " B=" << other->model_object()->name
               << " pairs=" << object.instances().size() * other->instances().size()
               << " kept=" << deltas.size() << " b_support_layers=" << b_support.size();
            dbg_xobj(ss.str());
        }
        if (deltas.empty())
            continue;

        // Z mapping by real [bottom_z, print_z) range overlap (adaptive layer heights —
        // never by index). Both stacks ascend, so a single forward cursor over B suffices.
        // Runs once over B's body layers and (A2) once over B's generated support layers;
        // `layer_at`/`geo_at` abstract the per-stack layer access and printed footprint.
        auto accumulate_stack = [&](size_t stack_size, auto &&layer_at, auto &&geo_at) {
            size_t jb = 0;
            for (size_t ia = 0; ia < num_layers; ++ia) {
                const Layer *la       = object.get_layer(int(ia));
                const double a_bottom = la->bottom_z();
                const double a_top    = la->print_z;
                while (jb < stack_size && layer_at(jb)->print_z <= a_bottom + EPSILON)
                    ++jb;
                Polygons b_local;
                for (size_t j = jb; j < stack_size; ++j) {
                    if (layer_at(j)->bottom_z() >= a_top - EPSILON)
                        break;
                    append(b_local, geo_at(j));
                }
                if (b_local.empty())
                    continue;
                b_local = union_(b_local);
                for (const Point &delta : deltas) {
                    Polygons shifted = b_local;
                    for (Polygon &poly : shifted)
                        poly.translate(delta);
                    append(occupancy[ia], std::move(shifted));
                }
                any = true;
            }
        };
        accumulate_stack(other->layer_count(),
            [&](size_t j) { return other->get_layer(int(j)); },
            [&](size_t j) { return to_polygons(other->get_layer(int(j))->lslices); });
        if (!b_support.empty())
            accumulate_stack(b_support.size(),
                [&](size_t j) { return b_support[j].first; },
                [&](size_t j) -> const Polygons & { return b_support[j].second; });
    }

    if (!any)
        return {};
    size_t occupied_layers = 0;
    for (Polygons &polys : occupancy)
        if (!polys.empty()) {
            polys = union_(polys);
            ++occupied_layers;
        }
    {
        std::ostringstream ss;
        ss << "neighbor_occupancy: A=" << object.model_object()->name
           << " occupied_layers=" << occupied_layers << "/" << num_layers;
        dbg_xobj(ss.str());
    }
    return occupancy;
}

} // namespace InstanceContact
} // namespace Slic3r
