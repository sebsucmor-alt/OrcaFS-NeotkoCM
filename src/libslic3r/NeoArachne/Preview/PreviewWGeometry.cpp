// NEOTKO_NEOARACHNE_TAG preview-lab PL.4
#include "PreviewWGeometry.hpp"

#include "../../SurfaceCollection.hpp"
#include "../../Surface.hpp"
#include "../../Polygon.hpp"
#include "../../ExPolygon.hpp"
#include "../../libslic3r.h"

namespace Slic3r { namespace NeoArachne { namespace Preview {

// Real-world W contour extracted from `letraW.stl` (the test object the user
// has been printing during NeoArachne development). Bounding box 4.79 × 4.18 mm,
// 26 vertices. Cross-section at Z = 0.4 mm, normalised to origin (0,0) and
// rewound counter-clockwise so the Polygon is a positive (exterior) loop.
//
// Why this size matters for the preview:
//   * Outer flanks are ~0.9 mm thick → with a 0.4 mm nozzle that is exactly
//     in the 2-bead → 3-bead transition where Arachne's beading strategy
//     becomes interesting.
//   * The central V dip narrows to ~0.5 mm → exercises the min_bead and
//     min_feature_size floors the Edge Closure controls expose.
//   * The two inner peaks where flanks meet drop to ~0.3 mm closures — the
//     classic feature that gets lost in Classic+gap-fill but should survive
//     under NeoArachne with `keep_short_tails`.
namespace {
struct PointMM { double x; double y; };
constexpr PointMM W_CONTOUR_MM[] = {
    {1.3125, 0.0000}, {0.9915, 0.0000}, {0.4958, 2.0895}, {0.0000, 4.1790},
    {0.3933, 4.1790}, {0.7867, 4.1790}, {1.0707, 2.9827}, {1.3548, 1.7863},
    {1.6416, 2.9827}, {1.9284, 4.1790}, {2.3968, 4.1790}, {2.8652, 4.1790},
    {3.1534, 2.9731}, {3.4416, 1.7672}, {3.7229, 2.9731}, {4.0042, 4.1790},
    {4.3989, 4.1790}, {4.7936, 4.1790}, {4.2992, 2.0895}, {3.8048, 0.0000},
    {3.4825, 0.0000}, {3.1602, 0.0000}, {2.7819, 1.5951}, {2.4036, 3.1903},
    {2.0185, 1.5951}, {1.6334, 0.0000},
};
} // namespace

SurfaceCollection build_w_surface_collection()
{
    Polygon contour;
    contour.points.reserve(sizeof(W_CONTOUR_MM) / sizeof(PointMM));
    for (const PointMM& p : W_CONTOUR_MM)
        contour.points.emplace_back(Point(scaled<coord_t>(p.x), scaled<coord_t>(p.y)));

    ExPolygon ex;
    ex.contour = std::move(contour);

    SurfaceCollection sc;
    sc.surfaces.emplace_back(stInternal, std::move(ex));
    return sc;
}

}}} // namespace Slic3r::NeoArachne::Preview
