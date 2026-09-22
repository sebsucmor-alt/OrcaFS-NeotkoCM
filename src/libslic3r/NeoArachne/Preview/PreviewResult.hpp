// NEOTKO_NEOARACHNE_TAG preview-lab PL.5 + v3 (s96)
// Plain-data result of slicing the W test geometry through NeoArachne::Plan.
// Owns deep copies of everything the renderer needs so the worker thread can
// hand it across thread boundaries without aliasing the PerimeterGenerator
// that produced it (which is destroyed at return).
//
// v3 (task #9): in addition to the static structural view, captures the
// real print-order chain computed by chain_and_reorder_extrusion_entities,
// flattened into a linear sequence of OrderedSegments (extrusion + travel)
// so the renderer can draw travel polylines, seam dots, and a head-position
// animated dot without re-computing chain order on the GUI thread.
#ifndef slic3r_NeoArachne_Preview_PreviewResult_hpp_
#define slic3r_NeoArachne_Preview_PreviewResult_hpp_

#include "../../ExtrusionEntityCollection.hpp"
#include "../../ExtrusionEntity.hpp"  // ExtrusionRole
#include "../../SurfaceCollection.hpp"
#include "../../ExPolygon.hpp"
#include "../../BoundingBox.hpp"
#include "../../Point.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace NeoArachne { namespace Preview {

// NEOTKO_NEOSTROKE_TAG s335 — EL ANCHO QUE DE VERDAD CAE, deducido del caudal.
// `ExtrusionPath::width` es una etiqueta: es lo que se escribe en `;WIDTH:` y lo que el visor venía
// dibujando. Pero el plástico lo decide `mm3_per_mm`, y hay mandos que lo cambian SIN tocar el ancho
// (en NeoStroke, `neostroke_curve_overlap` hace `mm3_per_mm *= f`). Con eso, subir el overlap no
// movía un pixel en pantalla aunque sí cambiara la pieza.
// Se invierte la sección del cordón de Slic3r (rectángulo con los lados redondeados):
//     mm3_per_mm = h·w − h²·(1 − π/4)   ⇒   w = (mm3_per_mm + h²·(1 − π/4)) / h
// Si no hay caudal o altura utilizables se devuelve el ancho nominal, que es lo que había antes.
inline float flow_equivalent_width(double mm3_per_mm, float width, float height)
{
    if (height <= 0.f || mm3_per_mm <= 0.0)
        return width;
    const double h = double(height);
    const double w = (mm3_per_mm + h * h * (1.0 - 0.25 * PI)) / h;
    return (w > 0.0 && w < 100.0) ? float(w) : width;
}

struct Metrics {
    double bead_count_avg = 0.0;
    size_t closures_count = 0;     // VariableWidthLines / ExtrusionLine is_odd count
    double total_wall_mm  = 0.0;
    double total_fill_mm2 = 0.0;
};

// v3 — one ordered segment in print order. Extrusion segments derive from
// the chained ExtrusionPath polylines; travel segments are synthesised between
// consecutive extrusions (end of path i → start of path i+1). All Points are
// in scaled coordinates (same unit system as bbox).
struct OrderedSegment {
    Point         from;
    Point         to;
    double        length_scaled = 0.0;   // = (to - from).cast<double>().norm()
    double        cum_start     = 0.0;   // cumulative length up to `from`
    float         path_width    = 0.f;   // mm, 0 for travel — el NOMINAL, el que sale en `;WIDTH:`
    // s335 — el ancho deducido del caudal: lo que de verdad se deposita. Es el que dibuja el lienzo.
    float         path_width_flow = 0.f;
    ExtrusionRole role          = erNone;
    bool          is_travel     = false;
    bool          from_multipath= false; // for color parity with static view
};

struct PreviewResult {
    ExtrusionEntityCollection loops;
    ExtrusionEntityCollection gap_fill;
    SurfaceCollection         fill_surfaces;
    ExPolygons                fill_no_overlap;
    BoundingBox               bbox;            // scaled coordinates of the original slice
    Metrics                   metrics;
    bool                      ok = false;
    std::string               error;

    // ── v3: real print-order info ────────────────────────────────────────
    std::vector<OrderedSegment> ordered_segments;  // extrusions + travels, in print order
    std::vector<Point>          seam_points;       // first point of each ExtrusionLoop in chain order
    double                      total_length_scaled = 0.0;   // sum of extrusion lengths only
    double                      total_chain_scaled  = 0.0;   // sum of all segment lengths (extrusion + travel) — used to advance the head animation

    // ── fix #3 (s96): input geometry passed to PerimeterGenerator ────────
    // Captured AFTER XY compensation has been applied — this is the exact
    // ExPolygon set that NeoArachne::Plan::run sees. Lets a debug dump diff
    // the preview's view of the slice against what the real slicer emits
    // (export 3MF + inspect Layer's lslices). If these diverge → upstream
    // problem (mesh prep, slicing tolerance, hole compensation). If they
    // match but the toolpaths still diverge → downstream problem
    // (NeoArachne::Plan, WallToolPaths). Cheap diagnostic split.
    ExPolygons                  input_slices;

    // NEOTKO_NEOSTROKE_TAG s335 — elección de islas. `islands_all` son las islas del corte SIN
    // trasladar (el mismo marco que `PreviewGeometrySource::island_picks`), y se rellenan también
    // cuando el corte se rechaza por tener demasiadas: son lo que el panel dibuja para elegir.
    // `needs_pick` = hay que elegir antes de poder laminar.
    ExPolygons                  islands_all;
    bool                        needs_pick = false;
};

}}} // namespace Slic3r::NeoArachne::Preview

#endif
