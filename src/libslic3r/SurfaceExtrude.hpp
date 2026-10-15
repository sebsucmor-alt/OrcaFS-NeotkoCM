// Construct (Align & Construct gizmo): turn a flat face of a mesh into a solid. Geometry only.
#ifndef slic3r_SurfaceExtrude_hpp_
#define slic3r_SurfaceExtrude_hpp_

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

// The holes of `outline`, each one as a filled shape (counter-clockwise, no holes of its own).
ExPolygons outline_holes(const ExPolygons &outline);

// Closed prism of `outline` grown/shrunk by offset_mm, from z0 to z0 + height.
// Empty when the offset eats the whole shape or height <= 0.
indexed_triangle_set extrude_outline(const ExPolygons &outline,
                                     double            offset_mm,
                                     double            z0,
                                     double            height);

}} // namespace Slic3r::SurfaceExtrude

#endif
