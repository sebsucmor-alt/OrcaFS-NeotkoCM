// NEOTKO_CONSTRUCT_TAG_START s363 — ver NeotkoSurfaceExtrude.hpp
#include "NeotkoSurfaceExtrude.hpp"

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

std::vector<Vec2d> sample_curve(const std::vector<CurveNode> &curve, int per_segment)
{
    std::vector<Vec2d> out;
    if (curve.empty())
        return out;
    per_segment = std::max(1, per_segment);
    out.reserve((curve.size() - 1) * per_segment + 1);
    for (size_t i = 0; i + 1 < curve.size(); ++i) {
        const CurveNode &a = curve[i];
        const CurveNode &b = curve[i + 1];
        for (int s = 0; s < per_segment; ++s) {
            const double u  = double(s) / double(per_segment);
            const double w0 = (1. - u) * (1. - u), w1 = 2. * u * (1. - u), w2 = u * u;
            out.emplace_back(w0 * a.t + w1 * a.ht + w2 * b.t, w0 * a.e + w1 * a.he + w2 * b.e);
        }
    }
    out.emplace_back(curve.back().t, curve.back().e);
    return out;
}

double curve_value(const std::vector<Vec2d> &s, double t)
{
    if (s.empty())
        return 0.;
    if (t <= s.front().x())
        return s.front().y();
    for (size_t i = 1; i < s.size(); ++i)
        if (t <= s[i].x()) {
            const double dt = s[i].x() - s[i - 1].x();
            if (dt < 1e-9)
                return s[i].y();
            const double k = (t - s[i - 1].x()) / dt;
            return s[i - 1].y() + k * (s[i].y() - s[i - 1].y());
        }
    return s.back().y();
}

indexed_triangle_set extrude_outline_curved(const ExPolygons             &outline,
                                            double                        offset_mm,
                                            double                        z0,
                                            double                        height,
                                            const std::vector<CurveNode> &curve,
                                            bool                          grow_down,
                                            double                        max_step_mm)
{
    if (outline.empty() || height <= 0.)
        return {};
    if (curve.size() < 2)
        return extrude_outline(outline, offset_mm, z0, height);

    const std::vector<Vec2d> samples = sample_curve(curve);
    const int n_steps = std::clamp(int(std::ceil(height / std::max(max_step_mm, 1e-3))), 1, 400);

    // One slab per step, read at its mid height; consecutive steps with the same offset merge.
    std::vector<ExPolygons> slices;
    std::vector<float>      tops;
    double last_off = 0.;
    bool   any      = false;
    for (int k = 0; k < n_steps; ++k) {
        const double tm  = (double(k) + 0.5) / double(n_steps);
        const double off = offset_mm + curve_value(samples, grow_down ? 1. - tm : tm);
        const float  top = float(z0 + height * double(k + 1) / double(n_steps));
        if (! slices.empty() && std::abs(off - last_off) < 1e-4) {
            tops.back() = top;
            continue;
        }
        slices.emplace_back(std::abs(off) > 1e-6 ? offset_ex(outline, float(scale_(off))) : outline);
        tops.emplace_back(top);
        last_off = off;
        any |= ! slices.back().empty();
    }
    if (! any)
        return {};
    return slices_to_mesh(slices, z0, tops);
}

}} // namespace Slic3r::SurfaceExtrude
// NEOTKO_CONSTRUCT_TAG_END
