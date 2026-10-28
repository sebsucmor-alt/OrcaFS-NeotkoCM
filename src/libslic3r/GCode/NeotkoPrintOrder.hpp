#ifndef slic3r_GCode_NeotkoPrintOrder_hpp_
#define slic3r_GCode_NeotkoPrintOrder_hpp_

// NEOTKO_PRINTORDER_TAG — 2_49, «Print order» (Experimental, LibreMode). The user gives each
// object a number (1, 2, 3...) or leaves it Free (0). Within every layer AND every tool pass the
// objects present go in that order; objects that have finished are simply skipped (the stock
// funnel GCode::sort_print_object_instances() already drops what is not in the layer).
// Several materials: Orca's tool loop is outside the object loop, so each material is printed
// whole, in the user's order, before the next one. Nothing here touches ToolOrdering or the tower.
//
// Plan: docs/FUTURE/PRINT_ORDER_GIZMO_2_49.md (study: docs/FUTURE/PRINT_ORDER_CONDUCTOR.md).

#include <string>
#include <vector>

#include "../Point.hpp"

namespace Slic3r {

class Print;
class PrintObject;
class Layer;
struct PrintInstance;

namespace NeotkoPrintOrder {

// The rank of every instance, read from the plate mirror `neotko_print_order_ranks` (one entry
// per Model::objects index, filled by the GUI). 0 = Free. Out of range / missing = Free.
int rank_of(const Print& print, const PrintInstance& instance);

// True when the manual order is on for this print: the mirror key `neotko_print_order_manual`
// is set AND at least one object on this plate has a rank. Off => stock, byte-identical G-code.
bool active(const Print& print);

// Unconditional (whenever the channel is on) first line of every export: the LibreMode flag and
// the ranks the engine received, so "all Free" can be told apart from "numbers lost on the way".
void log_header(const Print& print);

// Pure core, unit-testable. `chain` is the stock nearest-neighbour chain (a complete
// permutation of the live instances); `ranks[i]` is the rank of chain[i].
// Result: ranked instances first, by rank ascending; ties and the Free ones keep their relative
// order from `chain` (so «Free = as Orca does»). Always a permutation of `chain`, deterministic.
std::vector<const PrintInstance*> order(const std::vector<const PrintInstance*>& chain,
                                        const std::vector<int>&                 ranks);

// Builds the plate order: stock nearest chain + ranks + order(). Logs it to PRINTORDER.
std::vector<const PrintInstance*> build(const Print& print);

// How one object is named in the PRINTORDER log: «"name" #2 (x,y)» or «"name" Free (x,y)».
// The name is quoted because people often name objects with plain numbers; (x,y) is the copy's
// position on the plate in mm, so two copies of one object can be told apart.
std::string label(const Print& print, const PrintInstance& instance);

// Inside one object («Feature B, parts», docs/FUTURE/PRINT_ORDER_GIZMO_2_49.md §6c): the order in
// which to emit the islands of `layer` (indices into the G-code island vector, `n_islands` long,
// whose first layer.lslices.size() entries are the lslices). Each lslice is given to the part
// (ModelVolume) whose own slice overlaps it most (PrintObject::firstLayerObjSlice(), which in
// spite of its name holds every layer); numbered parts first by number, Free ones and anything
// unmatched keep Orca's order. Identity when the manual order is off or the object has no
// numbered parts, so stock G-code is untouched. `owner` is the instance's own PrintObject (its
// parts carry the numbers); `layer` may belong to a shared object with identical geometry.
std::vector<size_t> island_order(const Print& print, const PrintObject& owner, const Layer* layer, size_t n_islands);

// True when the manual order is on and this object has at least one numbered part. Gates the
// stricter island matching in GCode::process_layer (see island_for_point below).
bool object_has_part_ranks(const Print& print, const PrintObject* po);

// 16-oct: the island that REALLY contains `p` (contour AND holes), smallest first, or -1.
// Stock matching (GCode.cpp entity_matches_surface) tests the contour only and falls back to the
// centre of the entity's bounding box, smallest island first: the outer walls of a plate with
// text sitting in its holes (base02 in the keychain) have their box centre inside a text island
// («Mellow») and were emitted in that text's turn, while the plate's infill went in its own.
// Harmless while every island prints back to back; visible as soon as islands are reordered.
int island_for_point(const Layer& layer, const std::vector<size_t>& test_order, const Point& p);

// One line for the PRINTORDER log: labels joined by " > ".
std::string describe(const Print& print, const std::vector<const PrintInstance*>& seq);

} // namespace NeotkoPrintOrder
} // namespace Slic3r

#endif
