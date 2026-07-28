#ifndef slic3r_GravitySnap_hpp_
#define slic3r_GravitySnap_hpp_

// NEOTKO_SNAPDRAG_TAG s227 — "Snap & Drag": with True Objects ON, dragging an object in the
// viewport rests it on the top surface of whatever it is really above, computed by 2D
// footprint overlap (not raycast under the cursor — a footprint that only partially overlaps
// a pillar must NOT be treated as resting on it). See docs/FUTURE/GRAVITY_SNAP_AND_DRAG_PLAN.md.
//
// GUI-only: this module never touches libslic3r/Print — it only decides where a ModelInstance
// visually lands. What it decides is honoured later by Slic3r::Gravity::GravityFloor at slice
// time (same "foreign solid below me" concept, resolved at footprint precision here and at
// polygon precision there).

#include <optional>
#include <set>
#include <utility>

#include "libslic3r/Polygon.hpp"

namespace Slic3r {

class GLVolumeCollection;

namespace GUI {
namespace GravitySnap {

// True when both "True Objects" (gravity_allow_free_z()) and the "Snap & Drag" sub-option are
// on. Single source of truth for every call site — see docs/FUTURE/GRAVITY_SNAP_AND_DRAG_PLAN.md
// §1 rule 5 (double gate) and §5 (master wall forces the key off).
bool enabled();

// Real floor Z (world, unscaled mm) under instance (object_idx, instance_idx), given every
// GLVolume currently in the scene.
//
// `moving` lists every (object_idx, instance_idx) pair being dragged together right now —
// excluded as candidate floors so a dragged group never rests on itself (plan §1 rule 3: no
// chaining, but a group must also not self-intersect).
//
// `engage_ratio` in (0, 1]: minimum fraction of the queried instance's OWN 2D footprint area
// that must overlap a candidate's footprint for that candidate to count as "resting on it".
// Callers pass a higher ratio to engage a new floor and a lower one to keep an already-engaged
// floor (hysteresis, plan §2) — this function itself is stateless.
//
// Every non-self, non-moving instance whose footprint overlaps mine by >= engage_ratio is a
// candidate, regardless of its current Z relative to mine: during a live drag the dragged
// instance is still sitting at its OLD (pre-snap) Z while its XY has already moved over a
// taller neighbour, so filtering candidates by "must be below my current bottom" rejects the
// exact case the feature exists for (s227 field bug — first cut of this function had that
// filter and two cubes never stacked). Footprint overlap is the only signal; there is no
// "obstacle beside me" case to special-case, because two DISJOINT footprints simply never
// pass the overlap check in the first place.
//
// Returns the HIGHEST qualifying candidate top — resting on the tallest thing actually under
// the footprint, never sinking into a shorter one that also happens to overlap — or
// std::nullopt when nothing qualifies. (Not the same aggregation as the multi-instance case in
// do_move/GLCanvas3D, which deliberately takes the LOWEST of several pillars across one
// object's instances — explicit user call, a different question: "which of MY OWN instances'
// targets do I use" vs. this function's "which candidate UNDER ME wins".)
//
// The candidate's "top" is NOT its flat convex-hull bbox — it is sampled by a few real raycasts
// through the candidate's actual mesh, so a hollow box (tall rim, low interior floor) resolves
// correctly depending on exactly where the overlap lands, instead of always reporting rim
// height. See sample_real_top_z()/sample_points_mm() in the .cpp.
//
// IMPORTANT — nullopt means "leave it exactly where it is", NOT "drop it to the bed". True
// Objects' whole promise is that nothing auto-drops; Snap & Drag only ever pulls an instance
// DOWN onto a floor it actually detects. A caller that maps nullopt to Z=0 breaks free floating
// entirely (every move with nothing underneath would slam to the bed) — revised after s227
// user testing found exactly that regression.
std::optional<double> floor_z_for_instance(const GLVolumeCollection &volumes,
                                            int object_idx, int instance_idx,
                                            const std::set<std::pair<int, int>> &moving,
                                            double engage_ratio);

// World-space 2D convex-hull footprint (scaled units) of instance (object_idx, instance_idx) —
// the exact same shape floor_z_for_instance uses internally to test overlap. Exposed read-only
// for GUI rendering (the Snap & Drag landing-shadow overlay); NOT used by the gating logic
// above, which stays self-contained. Returns an empty Polygon if the instance has no volumes.
Polygon instance_footprint(const GLVolumeCollection &volumes, int object_idx, int instance_idx);

} // namespace GravitySnap
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GravitySnap_hpp_
