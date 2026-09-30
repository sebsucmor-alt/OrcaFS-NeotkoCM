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
    // NEOTKO_NEOSTROKE_TAG s337 — el gizmo: el corte ya hecho, en coordenadas de PLACA (escaladas) y SIN
    // trasladar. Cada objeto de la placa se dibuja donde está, así que aquí no se mueve nada al origen.
    FromSlices,
};

struct PreviewGeometrySource {
    GeometryKind                          kind        = GeometryKind::W;
    std::shared_ptr<const TriangleMesh>   mesh;            // only used when kind == FromMesh
    double                                slice_z_mm  = -1.0;  // only honoured by FromMesh; <0 means "use mid-Z"
    // 🚨 s340 — FromMesh cortaba con `TriangleMesh::slice()` (cierre 0.0004 mm) y el laminado con
    //    `slice_closing_radius` (0.049 por defecto) + `resolution`. El cierre aplana los picos agudos de
    //    los agujeros (la A de NeoStroke-TEST: vértice del agujero ~0.1 mm más bajo) y con eso el plan de
    //    NeoStroke cambiaba (pares=2 en el laminado, 0 en el visor). Lo rellena el panel desde la config.
    double                                closing_radius_mm = 0.049;
    double                                resolution_mm     = 0.0025;

    // NEOTKO_NEOSTROKE_TAG s335 — ISLAS ELEGIDAS. Vacío = todas (el comportamiento de siempre).
    // Una isla se conserva si CONTIENE alguno de estos puntos.
    // 🚨 En coordenadas ESCALADAS DE LA MALLA, tal como salen de `mesh.slice()`, NO en las del
    //    lienzo: `build_from_mesh` traslada el corte para que su caja XY empiece en (0,0), y esa
    //    traslación cambia con la Z. Guardar la elección en el marco trasladado haría que al mover
    //    el deslizador se estuvieran mirando otras islas sin enterarse.
    // 🚨 Y por PUNTO, nunca por índice: las islas se recalculan en cada corte y su orden cambia.
    std::vector<Point>                    island_picks;

    // NEOTKO_NEOSTROKE_TAG s342 — AJUSTES POR ISLA: objeto → marco del corte (XY afín, como
    // `PerimeterGenerator::ns_obj_to_slice`). El gizmo corta en coordenadas de PLACA, así que aquí va la XY de la
    // matriz de la instancia. Identidad = el visor no sabe dónde caen las anclas (las islas usan los del objeto).
    double                                obj_to_slice[6] = { 1., 0., 0., 0., 1., 0. };

    // NEOTKO_NEOSTROKE_TAG s337 — sólo FromSlices: el corte y sus topes. Los topes del panel viejo
    // (8 islas, 2000 vértices) eran para no colgar un panel wx que relaminaba en cada tic; el gizmo lamina
    // cuando se le pide, así que los sube. Por encima del tope de vértices se simplifica igual que antes.
    ExPolygons                            slices;
    size_t                                max_islands = 8;
    size_t                                max_verts   = 2000;

    // Convenience factories — keep call sites short and self-documenting.
    static PreviewGeometrySource w();
    static PreviewGeometrySource wedge();
    static PreviewGeometrySource from_mesh(std::shared_ptr<const TriangleMesh> m, double slice_z_mm);
    static PreviewGeometrySource from_slices(ExPolygons slices, size_t max_islands, size_t max_verts);
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
