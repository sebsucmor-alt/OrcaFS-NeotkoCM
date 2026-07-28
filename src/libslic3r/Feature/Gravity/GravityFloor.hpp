// NEOTKO_GRAVITY_TAG s226 — Fase 0/1: the "real floor" of an object.
//
// Orca's gravity is implicit: "below me = my own previous layer", "my layer 0 is the
// bed". Both are false as soon as an object rests on ANOTHER object (see
// docs/FUTURE/GRAVITY_MASTER_PLAN.md §2.1 for the full inventory of the assumptions).
// This module computes, per object and per layer, what is REALLY underneath — so the
// bottom/bridge classification can be split BY AREA instead of by object.
//
// Design notes (plan §3):
//  - World coordinates need BOTH the object-local frame AND PrintInstance::shift; the
//    frame rule is local_B + shift_B - shift_A, exactly as InstanceContact does.
//  - Z mapping runs over real [bottom_z, print_z) ranges, never layer indices: this fork
//    prints adaptive layer height, so two objects have different layer grids. They DO
//    share the Z origin though (Slicing.cpp: object_print_z_min = 0, the layer stack is
//    bed-referenced and an elevated object simply has empty layers below), which is what
//    makes a direct bottom_z/print_z comparison across objects legitimate.
//  - The contact tolerance is DERIVED, not configured in mm: the mesh is sampled at ONE
//    plane per layer, at mid-height (PrintObjectSlice.cpp: slice_z = 0.5*(lo+hi)), so a
//    separation below half a layer height is invisible to the slicer anyway. Declaring
//    "supported" there invents nothing — it makes the existing quantization explicit.
//    Hence gap <= gravity_contact_gap_ratio * height(A, i), per layer, ALH-safe for free.
//
// Fase 1 is diagnostics only: nothing here changes a single extrusion yet.

#ifndef slic3r_GravityFloor_hpp_
#define slic3r_GravityFloor_hpp_

#include <vector>

#include "../../Polygon.hpp"
#include "../../Surface.hpp"

namespace Slic3r {

class PrintObject;
class Layer;

namespace Gravity {

// True when the gravity model is active for this object: the toggle is on AND the plate
// prints by layer. In by-object sequential mode a neighbour may not be printed yet at a
// given Z, so treating it as floor would bridge onto a void — same gate rationale as
// InstanceContact::cross_object_active(). Single source of truth for every consumer.
bool active(const PrintObject &object);

// Contact tolerance for one layer of `object`, in unscaled mm. See the header note: it is
// a fraction of THAT layer's height (never a global mm value), so adaptive layer height
// needs no special case.
double contact_gap(const PrintObject &object, size_t layer_idx);

// NEOTKO_GRAVITY_TAG s226 — Fase 5. True when this object's base sits on the print bed.
// Unambiguous per PrintObject: every instance of one object shares the same Z (instances
// differ only in XY shift), so an object cannot be half-on-bed / half-stacked — that would
// require two separate objects. The layer stack is bed-referenced (object_print_z_min = 0),
// so an elevated object simply has empty layers below its base; the object rests on the bed
// iff its first NON-empty layer sits at z ~ 0 (within the layer's contact gap). Used to
// suppress elephant-foot compensation on stacked pieces (§Fase 5). Independent of active():
// caller decides when to consult it.
bool rests_on_bed(const PrintObject &object);

// Per-layer footprint of every OTHER object's instances that lies immediately BELOW the
// layer, in the object-local frame of `object`, indexed by layer number. Empty vector when
// the feature is inactive or no neighbour is within reach, so callers no-op at zero cost.
//
// NOT the same query as InstanceContact::neighbor_occupancy(): that one wants Z-band
// OVERLAP (an obstacle at my level, for support avoidance); this one wants foreign solid
// whose TOP sits at or just under my bottom (something to rest on). A neighbour sharing my
// exact Z band is an obstacle but not a floor — within one layer the print order between
// objects is not guaranteed, so it must never be treated as already printed.
std::vector<Polygons> foreign_floor(const PrintObject &object);

// NEOTKO_GRAVITY_TAG — Fase 2: the core reclassification, BY AREA. Given the bottom-facing
// surfaces just built by detect_surfaces_type for one (layer, region) — where Orca has marked
// as stBottomBridge everything not resting on this object's own previous layer — split each
// one against the real floor:
//   - area that rests on the bed OR on another object  -> stBottom  (contact surface)
//   - area over actual air                             -> stBottomBridge (a true bridge)
// `floor` is the object's cached foreign_floor() (object-local, per object-layer index);
// `layer_idx` is the object layer number; `is_object_layer0` is (lower_layer == nullptr), the
// case where Orca declared everything solid because there is no lower layer at all — there the
// split turns unsupported area INTO a bridge (the "missing bridge" half). Rewrites `bottom` in
// place. No-op (byte-identical) unless Gravity is active; safe to call from the per-layer
// parallel_for since it only reads the pre-built `floor`.
void reclassify_bottom_surfaces(const PrintObject &object, size_t layer_idx,
                                bool is_object_layer0,
                                const std::vector<Polygons> &floor, Surfaces &bottom);

// NEOTKO_GRAVITY_TAG — Fase 1: read-only diagnostic pass. Compares the CURRENT surface
// classification (already computed by detect_surfaces_type) against what the real floor
// says, and reports both halves of the bug to the GRAVITY channel:
//   - false bridge : stBottomBridge area that actually rests on a neighbour
//   - missing bridge: layer-0 stBottom area that actually hangs over nothing
// Changes nothing. Call right after detect_surfaces_type(); no-op unless the channel is
// enabled AND the feature is active.
void diagnose(const PrintObject &object);

} // namespace Gravity
} // namespace Slic3r

#endif // slic3r_GravityFloor_hpp_
