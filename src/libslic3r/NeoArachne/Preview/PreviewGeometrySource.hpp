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
#include <vector>

#include "../../ExPolygon.hpp"
#include "../../Point.hpp"

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

    // NEOTKO_NEOSTROKE_TAG s335 — ISLAS ELEGIDAS. Vacío = todas (el comportamiento de siempre).
    // Una isla se conserva si CONTIENE alguno de estos puntos.
    // 🚨 En coordenadas ESCALADAS DE LA MALLA, tal como salen de `mesh.slice()`, NO en las del
    //    lienzo: `build_from_mesh` traslada el corte para que su caja XY empiece en (0,0), y esa
    //    traslación cambia con la Z. Guardar la elección en el marco trasladado haría que al mover
    //    el deslizador se estuvieran mirando otras islas sin enterarse.
    // 🚨 Y por PUNTO, nunca por índice: las islas se recalculan en cada corte y su orden cambia.
    std::vector<Point>                    island_picks;

    // Convenience factories — keep call sites short and self-documenting.
    static PreviewGeometrySource w();
    static PreviewGeometrySource wedge();
    static PreviewGeometrySource from_mesh(std::shared_ptr<const TriangleMesh> m, double slice_z_mm);
};

struct GeometryBuildResult {
    std::unique_ptr<SurfaceCollection>  surfaces;   // null on failure
    std::string                         error;      // empty on success

    // NEOTKO_NEOSTROKE_TAG s335 — las islas del corte ANTES de filtrar y SIN trasladar, o sea en el
    // mismo marco que `island_picks`. Se rellenan siempre que se haya llegado a cortar, incluso
    // cuando el resultado es un fallo por exceso de islas: son justo lo que el panel necesita
    // dibujar para que se pueda elegir. Vacío en las geometrías W/Wedge, que no tienen que elegir.
    ExPolygons                          islands_all;
    // true = hay más islas de las que se pueden laminar y NO se ha elegido ninguna todavía. El panel
    // lo lee para entrar en modo elección en vez de limitarse a enseñar el error.
    bool                                needs_pick = false;
};

// Builds the SurfaceCollection for the requested source. Pure compute,
// thread-safe, never throws — internal failures surface via the error
// string and a null surfaces pointer.
GeometryBuildResult build_surface_collection(const PreviewGeometrySource& src);

}}} // namespace Slic3r::NeoArachne::Preview

#endif
