// NEOTKO_GRAVITY_TAG s226 — Fase 0/1 implementation. See header for the design notes and
// docs/FUTURE/GRAVITY_MASTER_PLAN.md for the whole plan.

#include "GravityFloor.hpp"

#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/NeoDebug.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {
namespace Gravity {

// XY bbox slack when pruning neighbour pairs. Generous on purpose: pruning must never be
// the reason a real contact is missed, and a false positive only costs one clipper op.
static constexpr double PRUNE_MARGIN_MM = 1.0;

static void dbg(const std::string &msg)
{
    if (NeoDebug::enabled(NeoDebug::GRAVITY))
        NeoDebug::write(NeoDebug::GRAVITY, msg);
}

static const std::string &object_name(const PrintObject &po)
{
    static const std::string unnamed = "?";
    return po.model_object() ? po.model_object()->name : unnamed;
}

// 2D bbox of an object's local geometry, accumulated from the per-island bboxes.
static BoundingBox object_bbox(const PrintObject &po)
{
    BoundingBox bb;
    for (const Layer *layer : po.layers())
        for (const BoundingBox &b : layer->lslices_bboxes)
            bb.merge(b);
    return bb;
}

// The half of the gate that is NOT the user toggle: in by-object sequential printing a
// neighbour may not be printed yet at a given Z, so nothing may be treated as floor. This
// holds even for the read-only diagnostic — reporting a floor that will not exist at print
// time would be reporting a lie.
static bool sequence_allows_floor(const PrintObject &object)
{
    const Print *print = object.print();
    return print != nullptr && print->config().print_sequence == PrintSequence::ByLayer;
}

// NEOTKO_GRAVITY_TAG s226 — interim activation until the Gravity SideButton exists (Fase 6).
// The config toggle `neotko_true_objects` is comDevelop (hidden), so there is no way to flip it
// from the UI yet. This dedicated env var lets the real reclassification be tested on real
// plates. It is DELIBERATELY NOT the debug channel (ORCA_DEBUG_GRAVITY stays read-only): a
// debug flag must never change geometry. Remove this branch when Fase 6 wires the button.
static bool force_env()
{
    static const bool v = [] {
        const char *e = ::getenv("NEOTKO_GRAVITY_FORCE");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return v;
}

bool active(const PrintObject &object)
{
    return (object.config().neotko_true_objects.value || force_env())
        && sequence_allows_floor(object);
}

// NEOTKO_GRAVITY_TAG s226 — Fase 1 only: the diagnostic pass is read-only by construction,
// so it also runs with the toggle off when the debug channel is explicitly enabled. That
// makes the detector testable on real plates before the Gravity button exists (Fase 6) and
// before anything consumes the floor (Fase 2). `active()` stays strict — every future
// consumer that CHANGES geometry must go through it, never through this.
static bool diagnostics_allowed(const PrintObject &object)
{
    return sequence_allows_floor(object)
        && (object.config().neotko_true_objects.value || NeoDebug::enabled(NeoDebug::GRAVITY));
}

double contact_gap(const PrintObject &object, size_t layer_idx)
{
    if (layer_idx >= size_t(object.layer_count()))
        return 0.;
    const double ratio = object.config().gravity_contact_gap_ratio.value;
    return object.get_layer(int(layer_idx))->height * ratio;
}

bool rests_on_bed(const PrintObject &object)
{
    // First layer that actually carries geometry. Its bottom_z tells us the object's base
    // height in the bed-referenced stack: ~0 means it sits on the bed, > gap means it is
    // elevated (stacked on another piece, or floating). An object with no geometry at all
    // has nothing to compensate — report bed so callers keep stock behaviour.
    for (const Layer *layer : object.layers())
        if (! layer->lslices.empty())
            return layer->bottom_z() <= contact_gap(object, size_t(layer->id())) + EPSILON;
    return true;
}

// `allow` decouples the geometry from the gate so the Fase 1 diagnostic can reuse it under
// its own (looser, read-only) condition without duplicating a line of the builder.
static std::vector<Polygons> build_floor(const PrintObject &object, bool allow)
{
    const Print *print = object.print();
    if (print == nullptr || object.layer_count() == 0 || !allow)
        return {};

    const BoundingBox a_bbox = object_bbox(object);
    if (!a_bbox.defined)
        return {}; // every layer empty — nothing rests on anything
    const coord_t margin = scale_(PRUNE_MARGIN_MM);

    const size_t          num_layers = size_t(object.layer_count());
    std::vector<Polygons> floor(num_layers);
    bool                  any = false;

    for (const PrintObject *other : print->objects()) {
        if (other == &object || other->layer_count() == 0)
            continue;

        const BoundingBox b_bbox = object_bbox(*other);
        if (!b_bbox.defined)
            continue;

        // XY deltas (B-local -> A-local: local_B + shift_B - shift_A) surviving bbox
        // pruning. Conservative multi-instance union, same rationale as the support
        // occupancy builder: the classification is computed once per object and shared by
        // every copy, so every copy's relative view of every neighbour must be included.
        std::vector<Point> deltas;
        for (const PrintInstance &ai : object.instances())
            for (const PrintInstance &bi : other->instances()) {
                const Point delta  = bi.shift - ai.shift;
                BoundingBox b_in_a = b_bbox;
                b_in_a.translate(delta.x(), delta.y());
                b_in_a.offset(margin);
                if (b_in_a.overlap(a_bbox))
                    deltas.push_back(delta);
            }
        if (deltas.empty())
            continue;

        // Z selection — the query that makes this a FLOOR and not an obstacle.
        // B's layer j supports A's layer i when it sits at or just under A's bottom:
        //     print_z(j) >  a_bottom - gap      (its top reaches close enough)
        //  && bottom_z(j) <= a_bottom + EPSILON (it starts at or below me, i.e. it is
        //                                        below rather than merely coplanar)
        // A neighbour spanning across a_bottom (interpenetration, negative gap) satisfies
        // both and correctly counts as material underneath. A neighbour sharing my exact
        // band from above does not.
        const size_t b_layers = size_t(other->layer_count());
        for (size_t ia = 0; ia < num_layers; ++ia) {
            const Layer *la       = object.get_layer(int(ia));
            const double a_bottom = la->bottom_z();
            const double gap      = contact_gap(object, ia);

            Polygons b_local;
            for (size_t j = 0; j < b_layers; ++j) {
                const Layer *lb = other->get_layer(int(j));
                if (lb->bottom_z() > a_bottom + EPSILON)
                    break; // B ascends: everything further up starts above me
                if (lb->print_z > a_bottom - gap)
                    append(b_local, to_polygons(lb->lslices));
            }
            if (b_local.empty())
                continue;

            b_local = union_(b_local);
            for (const Point &delta : deltas) {
                Polygons shifted = b_local;
                for (Polygon &poly : shifted)
                    poly.translate(delta);
                append(floor[ia], std::move(shifted));
            }
            any = true;
        }
    }

    if (!any)
        return {};
    size_t covered = 0;
    for (Polygons &polys : floor)
        if (!polys.empty()) {
            polys = union_(polys);
            ++covered;
        }
    {
        std::ostringstream ss;
        ss << "foreign_floor: A='" << object_name(object) << "' layers_with_floor=" << covered
           << "/" << num_layers;
        dbg(ss.str());
    }
    return floor;
}

std::vector<Polygons> foreign_floor(const PrintObject &object)
{
    return build_floor(object, active(object));
}

// ---------------------------------------------------------------------------------------
// Fase 2 — reclassification by area. The real behaviour change.
// ---------------------------------------------------------------------------------------

void reclassify_bottom_surfaces(const PrintObject &object, size_t layer_idx,
                                bool is_object_layer0,
                                const std::vector<Polygons> &floor, Surfaces &bottom)
{
    if (!active(object) || bottom.empty())
        return;

    const Layer *layer  = object.get_layer(int(layer_idx));
    const bool   on_bed = layer->bottom_z() <= contact_gap(object, layer_idx) + EPSILON;

    // On the bed the whole layer is supported — a face there is never a bridge. Leave the
    // stBottom classification exactly as Orca produced it.
    if (on_bed)
        return;

    // The foreign floor for this layer, object-local.
    const ExPolygons foreign = (layer_idx < floor.size()) ? union_ex(floor[layer_idx]) : ExPolygons{};

    Surfaces out;
    out.reserve(bottom.size());
    for (Surface &s : bottom) {
        // Which surfaces do we touch?
        //  - stBottomBridge: always a candidate — part of it may rest on a neighbour.
        //  - stBottom on the object's own layer 0: Orca made it solid for lack of a lower
        //    layer; the part over air must become a bridge (the missing-bridge half).
        //  - stBottom otherwise: it rests on this object's own SUPPORT material
        //    (fully_supported). Leave it — that support is real floor Orca already knew about.
        const bool is_bridge = (s.surface_type == stBottomBridge);
        const bool candidate = is_bridge || (s.surface_type == stBottom && is_object_layer0);
        if (!candidate) {
            out.emplace_back(std::move(s));
            continue;
        }
        // Fast path: a bridge with no floor under it (and not on the bed) is a genuine bridge —
        // leave it untouched, no clipper ops. The layer-0 stBottom missing-bridge case is NOT
        // skipped even with an empty floor: it must still turn into a bridge.
        if (foreign.empty() && is_bridge) {
            out.emplace_back(std::move(s));
            continue;
        }

        const ExPolygons supported = foreign.empty()
            ? ExPolygons{} : intersection_ex(ExPolygons{ s.expolygon }, foreign);
        const ExPolygons air = foreign.empty()
            ? ExPolygons{ s.expolygon } : diff_ex(ExPolygons{ s.expolygon }, foreign);

        for (const ExPolygon &ex : supported) {
            Surface solid = s;                 // inherit thickness/attributes
            solid.surface_type = stBottom;     // contact surface — not a bridge
            solid.expolygon = ex;
            out.emplace_back(std::move(solid));
        }
        for (const ExPolygon &ex : air) {
            Surface bridge = s;
            bridge.surface_type = stBottomBridge;
            bridge.expolygon = ex;
            out.emplace_back(std::move(bridge));
        }
    }
    bottom = std::move(out);
}

// ---------------------------------------------------------------------------------------
// Fase 1 — diagnostics. Read-only: reports what WOULD be reclassified, changes nothing.
// ---------------------------------------------------------------------------------------

static double area_mm2(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &ex : expolys)
        a += ex.area();
    // ExPolygon::area() is in scaled units squared; SCALING_FACTOR^2 brings it to mm^2
    // (unscale() only takes an integral coord_t, so it cannot be nested on a double area).
    return a * (SCALING_FACTOR * SCALING_FACTOR);
}

void diagnose(const PrintObject &object)
{
    if (!NeoDebug::enabled(NeoDebug::GRAVITY) || !diagnostics_allowed(object))
        return;

    const std::vector<Polygons> floor = build_floor(object, true);
    const std::string           name  = object_name(object);

    // Reported even when the floor is empty: "this object touches nothing" is itself the
    // answer to "why did my stacked piece still come out as a bridge?".
    if (floor.empty()) {
        std::ostringstream ss;
        ss << "GRAVITY_DIAG obj='" << name << "' no_foreign_floor=1 (nothing underneath, or"
              " pruned by distance)";
        dbg(ss.str());
        return;
    }

    double total_false_bridge = 0., total_missing_bridge = 0.;

    for (size_t i = 0; i < size_t(object.layer_count()); ++i) {
        const Layer *layer = object.get_layer(int(i));
        // Typed slices are what detect_surfaces_type has just written; fill_surfaces are
        // only derived from them afterwards, so this reads the raw classification.
        ExPolygons bridge_area, bottom_area;
        for (const LayerRegion *region : layer->regions())
            for (const Surface &s : region->slices.surfaces) {
                if (s.surface_type == stBottomBridge)
                    bridge_area.emplace_back(s.expolygon);
                else if (s.surface_type == stBottom)
                    bottom_area.emplace_back(s.expolygon);
            }

        const ExPolygons floor_ex = union_ex(floor[i]);

        // Half 1 — FALSE BRIDGE: classified as bridge, but a neighbour is underneath.
        const ExPolygons false_bridge = floor_ex.empty() || bridge_area.empty()
            ? ExPolygons{} : intersection_ex(bridge_area, floor_ex);
        // REAL BRIDGE: the part of the bridge that stays a bridge (genuinely over air). For a
        // partial rest this must be > 0 in the SAME layer as false_bridge — that coexistence
        // is the proof that the by-area split works (P2). The whole-underside bridge area is
        // reported too, so false+real should add up to it.
        const ExPolygons real_bridge = bridge_area.empty() ? ExPolygons{}
            : (floor_ex.empty() ? bridge_area : diff_ex(bridge_area, floor_ex));

        // Half 2 — MISSING BRIDGE: only ever at the object's own layer 0, where Orca
        // declares every face solid because there is no lower layer at all. Anything there
        // that neither touches the bed nor rests on a neighbour is hanging in the air.
        ExPolygons missing_bridge;
        if (layer->lower_layer == nullptr && !bottom_area.empty()) {
            const bool on_bed = layer->bottom_z() <= contact_gap(object, i) + EPSILON;
            if (!on_bed)
                missing_bridge = floor_ex.empty() ? bottom_area
                                                  : diff_ex(bottom_area, floor_ex);
        }

        if (false_bridge.empty() && missing_bridge.empty())
            continue;

        const double a_false   = area_mm2(false_bridge);
        const double a_missing = area_mm2(missing_bridge);
        const double a_real    = area_mm2(real_bridge);
        total_false_bridge   += a_false;
        total_missing_bridge += a_missing;

        std::ostringstream ss;
        ss << "GRAVITY_DIAG obj='" << name << "' layer=" << i
           << " print_z=" << layer->print_z
           << " bottom_z=" << layer->bottom_z()
           << " h=" << layer->height
           << " gap=" << contact_gap(object, i)
           << " false_bridge_mm2=" << a_false        // -> becomes stBottom (contact surface)
           << " real_bridge_mm2=" << a_real           // -> stays stBottomBridge (true bridge)
           << " missing_bridge_mm2=" << a_missing;
        dbg(ss.str());
    }

    std::ostringstream ss;
    ss << "GRAVITY_DIAG_TOTAL obj='" << name
       << "' false_bridge_mm2=" << total_false_bridge
       << " missing_bridge_mm2=" << total_missing_bridge
       << "  (false_bridge = bridge that really rests on a neighbour;"
          " missing_bridge = layer-0 solid that really hangs over air)";
    dbg(ss.str());
}

} // namespace Gravity
} // namespace Slic3r
