// NEOTKO_NEOARACHNE_TAG preview-lab PL.7 — geometry source factory
#include "PreviewGeometrySource.hpp"
#include "PreviewWGeometry.hpp"

#include "../../SurfaceCollection.hpp"
#include "../../Surface.hpp"
#include "../../Polygon.hpp"
#include "../../ExPolygon.hpp"
#include "../../BoundingBox.hpp"
#include "../../TriangleMesh.hpp"
#include "../../libslic3r.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <vector>

namespace Slic3r { namespace NeoArachne { namespace Preview {

// ─── factories ──────────────────────────────────────────────────────────────

PreviewGeometrySource PreviewGeometrySource::w()
{
    PreviewGeometrySource s;
    s.kind = GeometryKind::W;
    return s;
}

PreviewGeometrySource PreviewGeometrySource::wedge()
{
    PreviewGeometrySource s;
    s.kind = GeometryKind::Wedge;
    return s;
}

PreviewGeometrySource PreviewGeometrySource::from_mesh(std::shared_ptr<const TriangleMesh> m, double slice_z_mm)
{
    PreviewGeometrySource s;
    s.kind       = GeometryKind::FromMesh;
    s.mesh       = std::move(m);
    s.slice_z_mm = slice_z_mm;
    return s;
}

namespace {

// Triangular wedge 6 × 4 mm — narrow tip at the top. Picked as a second
// built-in shape because it stresses different regimes than the W: a single
// continuous outer perimeter that smoothly narrows from base→tip, with no
// internal junctions. Useful for visualising how Arachne's bead-count
// transition behaves along a clean, unbranching contour.
constexpr double WEDGE_BASE_MM   = 6.0;
constexpr double WEDGE_HEIGHT_MM = 4.0;

SurfaceCollection build_wedge_surface_collection()
{
    Polygon contour;
    contour.points.reserve(3);
    contour.points.emplace_back(Point(scaled<coord_t>(0.0),                  scaled<coord_t>(0.0)));
    contour.points.emplace_back(Point(scaled<coord_t>(WEDGE_BASE_MM),        scaled<coord_t>(0.0)));
    contour.points.emplace_back(Point(scaled<coord_t>(WEDGE_BASE_MM / 2.0),  scaled<coord_t>(WEDGE_HEIGHT_MM)));

    ExPolygon ex;
    ex.contour = std::move(contour);

    SurfaceCollection sc;
    sc.surfaces.emplace_back(stInternal, std::move(ex));
    return sc;
}

// ─── FromMesh guards ────────────────────────────────────────────────────────
//
// These limits are deliberately tight. The Preview Lab runs the full
// NeoArachne::Plan pipeline on whatever it's given, on the GUI thread's
// debounce. A user-selected ModelVolume with 50 000 vertices and 30 disjoint
// islands per layer would saturate the worker and stall the panel for
// seconds at every slider tick. The guards reject such inputs upfront with
// a clear error string in the canvas, leaving the panel responsive.
constexpr double kMaxBboxMm        = 100.0;   // any axis
constexpr size_t kMaxExPolysLayer  = 8;
constexpr size_t kMaxVertsLayer    = 2000;    // total across all islands + holes
// Douglas-Peucker tolerance starts here and doubles up until we're under the
// vertex cap. 5 µm preserves nozzle-scale detail; 1 mm is the upper bound
// before geometry loses its identity.
constexpr double kSimplifyTolStartMm = 0.005;
constexpr double kSimplifyTolMaxMm   = 1.0;

size_t count_verts(const ExPolygons& polys)
{
    size_t n = 0;
    for (const ExPolygon& e : polys) {
        n += e.contour.points.size();
        for (const Polygon& h : e.holes) n += h.points.size();
    }
    return n;
}

// Slices the snapshotted mesh at the requested Z (mid-Z fallback if the
// request is out of range or unset). Applies the three guards and a
// progressive Douglas-Peucker pass; returns null + error on failure.
std::unique_ptr<SurfaceCollection> build_from_mesh(const TriangleMesh& mesh, double slice_z_mm, std::string& err)
{
    if (mesh.empty()) {
        err = "preview: selected object has no geometry";
        return nullptr;
    }

    const BoundingBoxf3 bb = mesh.bounding_box();
    const Vec3d         sz = bb.size();
    if (sz.x() > kMaxBboxMm || sz.y() > kMaxBboxMm || sz.z() > kMaxBboxMm) {
        err = "preview: object exceeds 100 mm in some axis — pick a smaller object";
        return nullptr;
    }
    if (sz.x() <= 0.0 || sz.y() <= 0.0 || sz.z() <= 0.0) {
        err = "preview: object is degenerate (zero extent)";
        return nullptr;
    }

    // Z resolution: honour the request if it falls inside the mesh, else fall
    // back to mid-Z. The mesh slicer wants the absolute Z of the mesh's own
    // coordinate frame — same frame as bb.min.z()..bb.max.z().
    double z = slice_z_mm;
    if (!(z > bb.min.z() && z < bb.max.z()))
        z = 0.5 * (bb.min.z() + bb.max.z());

    std::vector<ExPolygons> layers = mesh.slice(std::vector<double>{z});
    if (layers.empty() || layers.front().empty()) {
        err = "preview: slice plane missed the mesh — try a different layer";
        return nullptr;
    }
    ExPolygons polys = std::move(layers.front());

    if (polys.size() > kMaxExPolysLayer) {
        err = "preview: slice has " + std::to_string(polys.size()) +
              " islands (>" + std::to_string(kMaxExPolysLayer) + " not supported)";
        return nullptr;
    }

    // Progressive simplification until the vertex budget is met.
    size_t verts  = count_verts(polys);
    double tol_mm = kSimplifyTolStartMm;
    while (verts > kMaxVertsLayer && tol_mm <= kSimplifyTolMaxMm) {
        const double tol_scaled = scaled<double>(tol_mm);
        for (ExPolygon& e : polys) e.douglas_peucker(tol_scaled);
        verts   = count_verts(polys);
        tol_mm *= 2.0;
    }
    if (verts > kMaxVertsLayer) {
        err = "preview: slice has " + std::to_string(verts) +
              " vertices after simplification — too detailed for preview";
        return nullptr;
    }

    // Normalise: translate so the slice's XY bbox starts at (0,0). Keeps the
    // canvas zoom/offset logic identical to the W/Wedge sources (which are
    // already author-positioned at the origin).
    const BoundingBox xy_bb = get_extents(polys);
    const Point       offset(-xy_bb.min.x(), -xy_bb.min.y());
    Slic3r::translate(polys, offset);

    auto sc = std::make_unique<SurfaceCollection>();
    sc->surfaces.reserve(polys.size());
    for (ExPolygon& e : polys)
        sc->surfaces.emplace_back(stInternal, std::move(e));
    return sc;
}

} // namespace

// ─── dispatch ───────────────────────────────────────────────────────────────

GeometryBuildResult build_surface_collection(const PreviewGeometrySource& src)
{
    GeometryBuildResult out;

    try {
        switch (src.kind) {
            case GeometryKind::W: {
                out.surfaces = std::make_unique<SurfaceCollection>(build_w_surface_collection());
                break;
            }
            case GeometryKind::Wedge: {
                out.surfaces = std::make_unique<SurfaceCollection>(build_wedge_surface_collection());
                break;
            }
            case GeometryKind::FromMesh: {
                if (!src.mesh) {
                    out.error = "preview: FromMesh source has no mesh snapshot";
                    break;
                }
                out.surfaces = build_from_mesh(*src.mesh, src.slice_z_mm, out.error);
                break;
            }
        }
    } catch (const std::exception& ex) {
        out.surfaces.reset();
        out.error = std::string("preview: geometry build exception — ") + ex.what();
    } catch (...) {
        out.surfaces.reset();
        out.error = "preview: geometry build unknown exception";
    }

    if (out.surfaces && out.surfaces->empty()) {
        out.surfaces.reset();
        if (out.error.empty())
            out.error = "preview: geometry source produced empty surface";
    }

    return out;
}

}}} // namespace Slic3r::NeoArachne::Preview
