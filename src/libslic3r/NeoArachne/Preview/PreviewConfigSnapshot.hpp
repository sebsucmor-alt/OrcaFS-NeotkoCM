// NEOTKO_NEOARACHNE_TAG preview-lab PL.5
// Thread-safe snapshot of the configuration the preview worker needs to
// invoke NeoArachne::Plan::run on the W test geometry. Captured from the
// active preset bundle on the GUI thread; consumed from a worker thread
// inside PreviewSlicer. The struct owns deep copies so the preview pipeline
// never reads from a preset that the user may be mutating concurrently.
#ifndef slic3r_NeoArachne_Preview_PreviewConfigSnapshot_hpp_
#define slic3r_NeoArachne_Preview_PreviewConfigSnapshot_hpp_

#include "../../PrintConfig.hpp"
#include "../../Flow.hpp"

namespace Slic3r { namespace NeoArachne { namespace Preview {

struct ConfigSnapshot {
    PrintRegionConfig region;
    PrintObjectConfig object;
    PrintConfig       print;

    double layer_height                = 0.2;
    Flow   perimeter_flow              {};
    Flow   ext_perimeter_flow          {};
    Flow   overhang_flow               {};
    Flow   solid_infill_flow           {};
    Flow   smaller_ext_perimeter_flow  {};

    // ── layer-id heuristics (fix #2) ────────────────────────────────────
    // Until s96 the preview always set layer_id = 5 — a "safe middle" default
    // that avoided raft + only_one_wall_first_layer edge cases. That made the
    // preview diverge from the real slicer the most on TOP layers (where the
    // real pipeline applies top-specific logic and the preview pretended to
    // be a middle layer). These hints let the panel pass the true layer_id
    // derived from the slice_z + mesh bbox so the preview matches reality.
    // Defaults still resolve to "middle layer" for built-in W/Wedge sources
    // which are 2D and have no real layer concept.
    int  effective_layer_id            = 5;
    bool is_top_layer                  = false;
    bool is_bottom_layer               = false;

    // Forces dependencies on neighbouring slices out of the picture so the
    // worker can run on the standalone W without upper/lower layers. Mutates
    // the region copy in place; safe because it is already a deep copy.
    void force_isolated_layer_defaults();
};

}}} // namespace Slic3r::NeoArachne::Preview

#endif
