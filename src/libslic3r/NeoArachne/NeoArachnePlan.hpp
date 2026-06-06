// NEOTKO_NEOARACHNE_TAG fase1
// NeoArachne orchestration plan for the "Neotko Hybrid v2" architecture:
//   1) Run PerimeterGenerator::process_classic() with wall_loops forced to 1
//      and gap_infill_speed forced to 0 → Classic emits a single outer
//      perimeter; no medial-axis gap fill is generated.
//   2) Compute the residual interior outline (offset of slice by
//      ext_perimeter_spacing) and hand it to NeoArachne::Interior, which
//      drives Arachne::WallToolPaths and emits inner walls + integrated
//      gap-fill to g.loops.
//
// process_classic is NEVER modified — we only swap g.config to point at a
// local PrintRegionConfig copy with the two values mutated, then restore.
// This honors the "facade only" filosofía of the canonical plan.
#ifndef slic3r_NeoArachnePlan_hpp_
#define slic3r_NeoArachnePlan_hpp_

namespace Slic3r {
class PerimeterGenerator;
namespace NeoArachne {

class Plan {
public:
    static void run(PerimeterGenerator& g);
};

}} // namespace Slic3r::NeoArachne

#endif
