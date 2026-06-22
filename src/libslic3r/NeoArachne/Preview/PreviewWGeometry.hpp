// NEOTKO_NEOARACHNE_TAG preview-lab PL.4
// Hardcoded W-shaped test object for the NeoArachne Preview Lab. Designed to
// exercise the three width regimes the user cares about: wide (>2 bead),
// transition (1↔2 bead), and thin closures (~min_bead). Output is a
// SurfaceCollection with a single stInternal Surface ready to feed
// PerimeterGenerator from the preview slicer.
#ifndef slic3r_NeoArachne_Preview_PreviewWGeometry_hpp_
#define slic3r_NeoArachne_Preview_PreviewWGeometry_hpp_

namespace Slic3r {
class SurfaceCollection;
namespace NeoArachne { namespace Preview {

// Builds the W as a single-Surface SurfaceCollection in scaled coordinates
// (coord_t units). Real contour from letraW.stl: 4.79 × 4.18 mm, ~0.9 mm
// outer flanks, ~0.5 mm at the V dip — sized for a 0.4 mm nozzle so the
// Arachne 2→3 bead transition and the min_bead floor are both reachable
// within the visible silhouette.
SurfaceCollection build_w_surface_collection();

}}} // namespace Slic3r::NeoArachne::Preview

#endif
