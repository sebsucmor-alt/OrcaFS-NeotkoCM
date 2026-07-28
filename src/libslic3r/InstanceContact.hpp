// NEOTKO_CONTACT_TAG s224 — C1: real contact/floating detection between print instances.
//
// Replaces the blind "first layer is empty => floating" heuristic with an actual
// measurement: for an object whose first layer is empty, check whether each of its
// instances is resting (gap <= CONTACT_GAP_THRESHOLD_MM) on the build plate or on the
// top surface of ANOTHER instance on the plate. Design notes (see
// docs/FUTURE/SURFACE_ANCHOR_AND_CONTACT_DETECTION_RESEARCH.md §3):
//  - World coordinates require BOTH the object-local frame AND PrintInstance::shift —
//    trafo_centered() alone is a per-object local frame and must never be compared
//    across two different objects.
//  - The gap is measured in Z from layer print_z/bottom_z (exact values); XY containment
//    runs on lslices, whose Clipper simplification noise (RESOLUTION = 0.0125mm) is the
//    same order as the 0.01mm threshold — mitigated by eroding the footprint before
//    sampling, so edge points never decide the result.
//  - Runs lazily (only when the empty-first-layer check fires), so it adds zero cost to
//    a normal slice. Iterates per PrintInstance, never per PrintObject: two copies of
//    the same object have different shifts and must be judged independently.

#ifndef slic3r_InstanceContact_hpp_
#define slic3r_InstanceContact_hpp_

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "Polygon.hpp"

namespace Slic3r {

class Print;
class PrintObject;
class ExPolygon;

namespace InstanceContact {

// Gap at or below this value counts as contact. 100x EPSILON (float-noise safe),
// 10x SINKING_Z_THRESHOLD magnitude; same order as Clipper RESOLUTION — see the
// erosion note above.
constexpr double CONTACT_GAP_THRESHOLD_MM = 0.01;

struct InstanceResult {
    bool               supported      = false;   // resting on bed or another instance
    double             gap_mm         = std::numeric_limits<double>::infinity(); // smallest gap found
    const PrintObject* support_object = nullptr; // what it rests on (nullptr = bed / nothing)
};

struct ObjectContact {
    std::vector<InstanceResult> instances;       // same order as object.instances()
    bool   all_supported   = false;              // every instance rests on something
    double worst_gap_mm    = std::numeric_limits<double>::infinity(); // largest gap among unsupported instances
};

// Measure contact for every instance of `object` against the bed and against all
// instances of all OTHER PrintObjects of the same Print. Read-only; safe to call any
// time after lslices exist (posSlice done) for all objects.
ObjectContact analyze_object(const PrintObject &object);

// NEOTKO_CONTACT_TAG s224 C1.2 — per-island variant, for the sharp-tail detector.
// `island_local` is a single ExPolygon in the object-local scaled frame (an lslices
// entry), whose bottom sits at `island_bottom_z` (unscaled mm). Returns true when, for
// EVERY instance of `object`, some sample point of the (eroded) island rests on the top
// surface of another object's instance with gap <= CONTACT_GAP_THRESHOLD_MM (negative
// gap = overlap = contact). The bed is NOT considered here — an island right above the
// bed is not what the sharp-tail seed check produces. Read-only and thread-safe against
// concurrent reads (call only while other objects' lslices are stable).
bool island_rests_on_other_object(const PrintObject &object, const ExPolygon &island_local, double island_bottom_z);

// NEOTKO_XOBJ_TAG s225 — A1: cross-object support avoidance occupancy.
// Per-layer footprint (object-local frame of `object`, indexed by object layer number)
// of every OTHER PrintObject's instances on the plate, for injection into the tree
// support collision volumes. Returns an EMPTY vector when the feature is inactive
// (support_cross_object_avoidance off, sequential by-object print, or no neighbor within
// range) so callers can no-op at zero cost. Design (docs/FUTURE/CROSS_OBJECT_SUPPORT_PREPLAN.md §8):
//  - Z mapping by real [bottom_z, print_z) range overlap, never by layer index — the two
//    stacks may use different (adaptive) layer heights.
//  - Multi-instance conservative union: if `object` has several copies, each copy sees the
//    neighbors at a different relative offset; the occupancy is the union over all copies,
//    since the support is generated once per object and replicated per instance.
//  - Same world-frame rule as the contact detector: local_B + shift_B - shift_A, never
//    trafo_centered() alone.
// Read-only; requires lslices of all objects (posSlice done).
std::vector<Polygons> neighbor_occupancy(const PrintObject &object);

// NEOTKO_XOBJ_TAG s225 — true when cross-object support avoidance is effectively active
// for this object: the toggle is on AND the plate prints by layer (in by-object mode the
// neighbors may not exist yet, so avoidance is inert). Single source of truth for the
// several places that must react to the feature (occupancy builder + the first-layer
// expansion clamps that keep uncollided raft/brim offsets from spilling across objects).
bool cross_object_active(const PrintObject &object);

} // namespace InstanceContact
} // namespace Slic3r

#endif // slic3r_InstanceContact_hpp_
