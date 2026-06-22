// NEOTKO_NEOARACHNE_TAG fase1
// Interior wall generator: hands a residual outline to Arachne WallToolPaths and
// converts the resulting VariableWidthLines into ExtrusionEntities appended to
// PerimeterGenerator.loops. Inner walls only — never produces outer perimeters
// (the outer is owned by Classic in the Neotko Hybrid v2 architecture).
//
// All "inner" beads use perimeter_flow (NOT ext_perimeter_flow). The variable-
// width gap-fill segments Arachne emits as is_odd are kept in the same output
// stream — no filtering — because in this hybrid the gap-fill is integrated.
//
// Overhang detection is intentionally NOT applied here: the interior walls are
// shielded by the outer Classic perimeter which already handled overhangs at
// the slice boundary. Keeping this stripped lets us reuse extrusion_paths_append
// directly without depending on traverse_extrusions' static helpers.
#ifndef slic3r_NeoArachneInterior_hpp_
#define slic3r_NeoArachneInterior_hpp_

#include "../ExPolygon.hpp"
#include "../Polygon.hpp"
#include "NeoArachneConfig.hpp"

namespace Slic3r {
class PerimeterGenerator;
namespace NeoArachne {

class Interior {
public:
    // Run Arachne over outline_polys, emit walls to g.loops. Returns Arachne's
    // inner contour (region still available for infill after walls consumed
    // their area). Caller is responsible for using this to update fill_surfaces
    // if it wants infill to avoid the wall region.
    //
    // inset_count: number of interior beads requested (typically wall_loops-1).
    //              0 → no walls generated, returns outline_polys as inner_contour.
    //
    // cfg: drives Edge Closure params — min_bead_width_pct, min_feature_size_pct,
    //      keep_short_tails — passed through to Arachne's WallToolPathsParams.
    static ExPolygons run(PerimeterGenerator& g,
                          const Polygons&     outline_polys,
                          int                 inset_count,
                          const Config&       cfg);
};

}} // namespace Slic3r::NeoArachne

#endif
