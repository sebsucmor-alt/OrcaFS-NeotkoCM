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
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {

class GLVolumeCollection;

namespace GUI {
namespace GravitySnap {

// True when both "True Objects" (gravity_allow_free_z()) and the "Snap & Drag" sub-option are
// on. Single source of truth for every call site — see docs/FUTURE/GRAVITY_SNAP_AND_DRAG_PLAN.md
// §1 rule 5 (double gate) and §5 (master wall forces the key off).
bool enabled();

// NEOTKO_SNAPDRAG_TAG s233 — "Allow Bed" sub-sub-option (app_config "neotko_snap_drag_bed",
// DEFAULT ON, i.e. absent key reads as true — see the .cpp for why it is not seeded in
// AppConfig::set_defaults). ON = the build plate is one more floor candidate at Z=0, so an object
// dragged over nothing lands on the bed like in a normal slicer. OFF = the s227 behaviour: only
// other objects count as floors and an object over nothing keeps floating.
//
// Only meaningful while enabled() is true; True Objects OFF or Snap & Drag OFF means this module
// is inert regardless. See docs/FUTURE/GRAVITY_SNAP_AND_DRAG_V2_PLAN.md §1.
bool bed_is_floor();

// NEOTKO_SNAPDRAG_TAG s249 — "Move selection as one block" (app_config "neotko_snap_drag_group",
// DEFAULT OFF, i.e. absent key reads as false, so the shipped s233 behaviour is what a user gets
// until they ask for something else).
//
// OFF (s233) = a multi-instance drag is resolved BY STACKS: whoever stands on another member of
// the drag rides with it, everyone else finds their own floor. Two objects picked together land
// at two different heights, which is correct and is what the feature's own gif shows.
//
// ON = the whole selection is ONE rigid body. Nobody changes height RELATIVE to anyone else; the
// selection as a whole falls until the FIRST member touches its floor (the bed included, when
// bed_is_floor() is on) and stops there. This is the "carry this assembly somewhere else without
// it rearranging itself" mode — in particular it is what keeps a loose object dragged together
// with an assembled one from being sent to the plate on its own.
//
// Only meaningful while enabled() is true, and only for drags of more than one instance: a single
// instance is trivially its own rigid body and takes the untouched s227 path either way.
bool move_as_group();

// NEOTKO_SNAPDRAG_TAG s249 — is the Snap & Drag options panel showing? Mutable reference, exactly
// like PhotoMode's photo_mode(): this is transient UI state (no ini key — the panel does not
// survive a restart, only the preferences it edits do), and the two owners are far apart — the
// magnet icon in PartPlate/Plater flips it, GLCanvas3D::_render_overlays reads it.
bool& panel_open();

// NEOTKO_SNAPDRAG_TAG s249 — should the magnet icon exist in the plate column at all?
//
// Gated on LibreMode, NOT on True Objects, even though True Objects is what Snap & Drag actually
// needs. Two reasons, and the second is the load-bearing one:
//  - LibreMode is the master wall above True Objects anyway (MainFrame forces both sub-keys off
//    when it goes down), so nothing reachable is lost;
//  - the plate's picking raycasters are registered when plates are (re)built, not on every
//    app_config change. Keying the icon to a toggle the user flips from the toolbar mid-session
//    would leave it registered-but-invisible or visible-but-dead until something else happened to
//    rebuild the plate. This is the same gate Photo Mode's camera icon uses, for the same reason.
// True Objects being off is instead handled INSIDE the panel, which says so and dims its controls.
bool plate_icon_available();

// NEOTKO_SNAPDRAG_TAG s233 — result of a floor query. Was a bare std::optional<double> in s227;
// it now carries WHY/WHERE the answer came from, because three call sites need that and none of
// them may recompute it: the drag hysteresis must distinguish "resting on an object" from
// "resting on the bed" (a bed hit never fails, so treating it as engaged would pin the engage
// ratio at its low value forever and kill the hysteresis), and the landing overlay draws the
// actual contact zone and raycast hits rather than a decorative guess.
struct FloorHit
{
    // Floor height in world mm — the Z the queried instance's bottom should sit at.
    double z = 0.0;
    // True when the winner is the build plate (z == 0.0), false when it is another instance.
    bool is_bed = false;
    // The winning candidate instance, or (-1, -1) for the bed.
    int obj_idx  = -1;
    int inst_idx = -1;
    // Zone actually recognised as the floor, world XY, SCALED clipper units: the footprint
    // intersection that won for an object floor, or the queried instance's own footprint for the
    // bed. This is the shape to highlight — it is what the decision was made on.
    ExPolygon contact;
    // World-mm raycast hits inside `contact` that produced `z` (empty for the bed, and for the
    // defensive flat-top fallback). Drawn as-is by the overlay: when a user cannot "aim" at a
    // thin rim, seeing which sample points hit and which fell through the hole explains it.
    std::vector<Vec3d> samples;
};

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
//
// NEOTKO_SNAPDRAG_TAG s233 — with bed_is_floor() ON that "nothing underneath" case DOES resolve
// to the bed, but as a last-place CANDIDATE (FloorHit::is_bed), not as a nullopt→0 mapping: the
// bed is a floor of height 0 and the aggregation below is "highest wins", so it can never
// outrank a real object and no s227 case changes. nullopt still means "leave it floating", and
// is what you get with Allow Bed off, or always with Snap & Drag off (this module inert).
std::optional<FloorHit> floor_z_for_instance(const GLVolumeCollection &volumes,
                                             int object_idx, int instance_idx,
                                             const std::set<std::pair<int, int>> &moving,
                                             double engage_ratio);

// NEOTKO_SNAPDRAG_TAG s233 — which OTHER MEMBER OF THE SAME DRAG is instance (object_idx,
// instance_idx) currently resting on, if any.
//
// A multi-object drag may not be resolved member by member: every member excludes the others as
// floor candidates (a drag must never rest on itself), so the upper half of a dragged stack finds
// nothing under it and drops to the bed — the stack flattens. But it must not be resolved as one
// rigid block either, or two unrelated objects picked together stop falling independently. The
// answer is this relation: members that are stacked on each other travel together, members that
// have nothing of their own underneath fall on their own. See GLCanvas3D's use of it.
//
// Returns the HIGHEST group member that both overlaps my footprint by >= engage_ratio and has its
// top within `max_gap` below my bottom — i.e. one I am actually standing on, not merely one that
// happens to be somewhere below me. Uses the flat convex-hull top, NOT the raycast sampling
// floor_z_for_instance does: this only answers "are these two stacked", where a millimetre of
// slack is irrelevant and the exact surface is not the question.
std::optional<std::pair<int, int>> support_in_group(const GLVolumeCollection &volumes,
                                                    int object_idx, int instance_idx,
                                                    const std::set<std::pair<int, int>> &group,
                                                    double engage_ratio, double max_gap);

// World-space 2D convex-hull footprint (scaled units) of instance (object_idx, instance_idx) —
// the exact same shape floor_z_for_instance uses internally to test overlap. Exposed read-only
// for GUI rendering (the Snap & Drag landing-shadow overlay); NOT used by the gating logic
// above, which stays self-contained. Returns an empty Polygon if the instance has no volumes.
Polygon instance_footprint(const GLVolumeCollection &volumes, int object_idx, int instance_idx);

} // namespace GravitySnap
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GravitySnap_hpp_
