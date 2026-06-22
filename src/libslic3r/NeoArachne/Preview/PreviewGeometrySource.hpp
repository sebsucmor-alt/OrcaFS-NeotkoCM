// NEOTKO_NEOARACHNE_TAG preview-lab PL.7 — geometry source factory
//
// Abstracts the source of the Preview Lab's input geometry so the panel can
// pick between the hardcoded W (legacy default), a hardcoded wedge (a
// secondary built-in test shape), or a frozen snapshot of a ModelVolume the
// user has selected in the bed. The slicer used to call build_w_surface_collection()
// directly; now it consumes a PreviewGeometrySource and dispatches.
//
// Snapshot semantics for FromMesh: the caller hands in a shared_ptr<const
// TriangleMesh> that has already had any object transform baked in. The
// source OWNS that snapshot — the user can move/rotate the original
// ModelVolume in the bed without invalidating the preview (snapshot
// freezing, per the s93 plan).
#ifndef slic3r_NeoArachne_Preview_PreviewGeometrySource_hpp_
#define slic3r_NeoArachne_Preview_PreviewGeometrySource_hpp_

#include <memory>
#include <string>

namespace Slic3r {
class TriangleMesh;
class SurfaceCollection;
namespace NeoArachne { namespace Preview {

enum class GeometryKind {
    W,           // hardcoded letraW.stl cross-section, 2D contour (Z ignored)
    Wedge,       // hardcoded triangular wedge, 2D contour (Z ignored)
    FromMesh,    // frozen ModelVolume snapshot, sliced at slice_z_mm
};

struct PreviewGeometrySource {
    GeometryKind                          kind        = GeometryKind::W;
    std::shared_ptr<const TriangleMesh>   mesh;            // only used when kind == FromMesh
    double                                slice_z_mm  = -1.0;  // only honoured by FromMesh; <0 means "use mid-Z"

    // Convenience factories — keep call sites short and self-documenting.
    static PreviewGeometrySource w();
    static PreviewGeometrySource wedge();
    static PreviewGeometrySource from_mesh(std::shared_ptr<const TriangleMesh> m, double slice_z_mm);
};

struct GeometryBuildResult {
    std::unique_ptr<SurfaceCollection>  surfaces;   // null on failure
    std::string                         error;      // empty on success
};

// Builds the SurfaceCollection for the requested source. Pure compute,
// thread-safe, never throws — internal failures surface via the error
// string and a null surfaces pointer.
GeometryBuildResult build_surface_collection(const PreviewGeometrySource& src);

}}} // namespace Slic3r::NeoArachne::Preview

#endif
