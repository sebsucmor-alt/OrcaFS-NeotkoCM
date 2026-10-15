#include "SurfaceExtrude.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace SurfaceExtrude {

std::vector<int> coplanar_region(const std::vector<Vec3f>   &normals,
                                 const std::vector<Vec3i32> &neighbors,
                                 int                         seed,
                                 float                       cos_thresh,
                                 size_t                      max_facets)
{
    std::vector<int> region;
    const int n_facets = (int)normals.size();
    if (seed < 0 || seed >= n_facets || (int)neighbors.size() != n_facets)
        return region;

    const Vec3f seed_n = normals[seed].normalized();
    region.reserve(256);
    std::vector<char> visited(n_facets, 0);
    std::vector<int>  stack { seed };
    visited[seed] = 1;
    while (!stack.empty() && region.size() < max_facets) {
        const int f = stack.back();
        stack.pop_back();
        region.push_back(f);
        for (int e = 0; e < 3; ++e) {
            const int nb = neighbors[f][e];
            if (nb < 0 || nb >= n_facets || visited[nb])
                continue;
            if (normals[nb].normalized().dot(seed_n) >= cos_thresh) {
                visited[nb] = 1;
                stack.push_back(nb);
            }
        }
    }
    return region;
}

ExPolygons region_outline(const indexed_triangle_set &its,
                          const std::vector<int>     &region,
                          const Transform3d          &trafo,
                          double                     *mean_z)
{
    Polygons tris;
    tris.reserve(region.size());
    double z_sum = 0.;
    size_t z_cnt = 0;
    for (int f : region) {
        if (f < 0 || f >= (int)its.indices.size())
            continue;
        const Vec3i32 &tri = its.indices[f];
        Polygon p;
        p.points.reserve(3);
        for (int k = 0; k < 3; ++k) {
            const Vec3d v = trafo * its.vertices[tri[k]].cast<double>();
            p.points.emplace_back(Point::new_scale(v.x(), v.y()));
            z_sum += v.z();
            ++z_cnt;
        }
        if (std::abs(p.area()) < 1.)
            continue; // degenerate once projected
        p.make_counter_clockwise();
        tris.emplace_back(std::move(p));
    }
    if (mean_z != nullptr)
        *mean_z = z_cnt > 0 ? z_sum / double(z_cnt) : 0.;
    if (tris.empty())
        return {};
    // The tiny closing welds hairline seams left between neighbouring triangles.
    return closing_ex(union_ex(tris), float(scale_(0.01)));
}

ExPolygons outline_holes(const ExPolygons &outline)
{
    ExPolygons out;
    for (const ExPolygon &ex : outline)
        for (const Polygon &hole : ex.holes) {
            Polygon p = hole;
            p.make_counter_clockwise();
            out.emplace_back(std::move(p));
        }
    return out;
}

indexed_triangle_set extrude_outline(const ExPolygons &outline,
                                     double            offset_mm,
                                     double            z0,
                                     double            height)
{
    if (outline.empty() || height <= 0.)
        return {};
    const ExPolygons shape = std::abs(offset_mm) > 1e-6
        ? offset_ex(outline, float(scale_(offset_mm)))
        : outline;
    if (shape.empty())
        return {};
    // One slice = one slab: bottom cap at z0, walls, top cap at z0 + height.
    return slices_to_mesh(std::vector<ExPolygons>{ shape }, z0, height, height);
}

}} // namespace Slic3r::SurfaceExtrude
