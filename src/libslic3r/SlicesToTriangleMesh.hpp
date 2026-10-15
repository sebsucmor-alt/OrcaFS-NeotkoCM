#ifndef SLICESTOTRIANGLEMESH_HPP
#define SLICESTOTRIANGLEMESH_HPP

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ExPolygon.hpp"

namespace Slic3r {

// NEOTKO_CONSTRUCT_TAG s363c — ya existía en el .cpp (rebanadas con techos arbitrarios: `grid[i]`
// es el techo de slices[i], la primera nace en zmin); sólo se declara aquí para poder usarla.
indexed_triangle_set slices_to_mesh(const std::vector<ExPolygons> &slices,
                                    double                         zmin,
                                    const std::vector<float> &     grid);

void slices_to_mesh(indexed_triangle_set &         mesh,
                    const std::vector<ExPolygons> &slices,
                    double                         zmin,
                    double                         lh,
                    double                         ilh);

inline indexed_triangle_set slices_to_mesh(
    const std::vector<ExPolygons> &slices, double zmin, double lh, double ilh)
{
    indexed_triangle_set out;
    slices_to_mesh(out, slices, zmin, lh, ilh);

    return out;
}

} // namespace Slic3r

#endif // SLICESTOTRIANGLEMESH_HPP
