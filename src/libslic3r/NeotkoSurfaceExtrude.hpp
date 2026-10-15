// NEOTKO_CONSTRUCT_TAG_START s363 — «Construct» (Align & Construct): sacar una lamina extruida de
// la cara plana elegida de un objeto. Aqui solo la geometria, sin nada de interfaz.
#ifndef slic3r_NeotkoSurfaceExtrude_hpp_
#define slic3r_NeotkoSurfaceExtrude_hpp_

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <vector>

namespace Slic3r { namespace SurfaceExtrude {

// Facets connected to `seed` whose normal stays within acos(cos_thresh) of the seed's normal.
// `normals` / `neighbors` come from its_face_normals() / its_face_neighbors().
std::vector<int> coplanar_region(const std::vector<Vec3f>   &normals,
                                 const std::vector<Vec3i32> &neighbors,
                                 int                         seed,
                                 float                       cos_thresh,
                                 size_t                      max_facets = 40000);

// The region projected onto XY after `trafo` (scaled coordinates), holes kept.
// mean_z receives the average Z of the region's vertices after `trafo`.
ExPolygons region_outline(const indexed_triangle_set &its,
                          const std::vector<int>     &region,
                          const Transform3d          &trafo,
                          double                     *mean_z = nullptr);

// s367 — the holes of `outline`, each one as a filled shape (counter-clockwise, no holes of its own).
ExPolygons outline_holes(const ExPolygons &outline);

// Closed prism of `outline` grown/shrunk by offset_mm, from z0 to z0 + height.
// Empty when the offset eats the whole shape or height <= 0.
indexed_triangle_set extrude_outline(const ExPolygons &outline,
                                     double            offset_mm,
                                     double            z0,
                                     double            height);

// --- s363c — extrusión con curva (oculta, sólo modo depuración; sin documentar a propósito) ----
// Un nodo es una «línea de tensión»: una altura (t = fracción 0..1 de la altura total) por la que
// la curva pasa obligada, con su desplazamiento e en mm (positivo crece, negativo encoge). (ht, he)
// es el «punto de tensión» del tramo que va de este nodo al siguiente (Bézier cuadrática).
struct CurveNode {
    double t  = 0.;
    double e  = 0.;
    double ht = 0.;
    double he = 0.;
};

// The curve as a polyline of (t, e), t ascending.
std::vector<Vec2d> sample_curve(const std::vector<CurveNode> &curve, int per_segment = 24);
// e at height fraction t, read off a sample_curve() polyline.
double curve_value(const std::vector<Vec2d> &samples, double t);

// Like extrude_outline(), but the outline is offset by offset_mm + curve(t) at each height.
// t = 0 is the picked face: with grow_down the solid hangs below z0 + height instead of rising
// from z0. The solid is built in thin steps of at most max_step_mm.
indexed_triangle_set extrude_outline_curved(const ExPolygons             &outline,
                                            double                        offset_mm,
                                            double                        z0,
                                            double                        height,
                                            const std::vector<CurveNode> &curve,
                                            bool                          grow_down,
                                            double                        max_step_mm = 0.025);

}} // namespace Slic3r::SurfaceExtrude

#endif
// NEOTKO_CONSTRUCT_TAG_END
