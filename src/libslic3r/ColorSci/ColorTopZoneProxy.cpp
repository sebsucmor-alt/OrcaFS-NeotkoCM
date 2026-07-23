// NEOTKO_ALHCOLOR_TAG_START — replanteo TD-vs-slope, Frente 1 (+ Fase 5.1 slope scan)
#include "ColorTopZoneProxy.hpp"

#include "Model.hpp"
#include "TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r {
namespace ColorSci {

ObjectZoneScan compute_object_zone_scan(const ModelObject& object,
                                        double              band_depth_mm,
                                        double              min_tan_alpha,
                                        float               flat_cos_threshold)
{
    ObjectZoneScan out;
    if (object.instances.empty())
        return out;

    // Same simplification as SlicingAdaptive::prepare() (SlicingAdaptive.cpp): raw_mesh() +
    // first instance's transform only. Not reused via inheritance/friend — SlicingAdaptive's
    // m_faces/prepare() are protected instance state, widening that surface for an unrelated
    // consumer would grow engine API more than duplicating this ~10-line facet loop.
    TriangleMesh mesh = object.raw_mesh();
    const ModelInstance& first_instance = *object.instances.front();
    mesh.transform(first_instance.get_matrix(), first_instance.is_left_handed());

    // World Z of the object's own bottom -> converts transformed-mesh Z (world space) into the
    // same object-relative frame ALHPoint::z_mm uses (0 at the object's bottom).
    const double z0 = object.instance_bounding_box(0).min.z();

    const bool want_slopes = min_tan_alpha > 0.0;

    std::vector<std::pair<double, double>> top_intervals;
    top_intervals.reserve(mesh.its.indices.size() / 8); // most facets aren't flat-up; rough guess
    struct SlopeInterval { double z_lo, z_hi, tan_alpha; };
    std::vector<SlopeInterval> slope_intervals;

    for (const stl_triangle_vertex_indices& face : mesh.its.indices) {
        const Vec3f n = its_face_normal(mesh.its, face);
        const stl_vertex& v0 = mesh.its.vertices[face[0]];
        const stl_vertex& v1 = mesh.its.vertices[face[1]];
        const stl_vertex& v2 = mesh.its.vertices[face[2]];

        if (n.z() > flat_cos_threshold) {
            const double z = (double(v0.z()) + double(v1.z()) + double(v2.z())) / 3.0 - z0;
            top_intervals.emplace_back(z, z + band_depth_mm);
        }

        if (want_slopes) {
            // Slope severity per §7bis.a: n_cos = |n_z|, n_sin = sqrt(nx^2+ny^2),
            // tan_alpha = n_cos/n_sin. Dead-flat facets (n_sin ~ 0) don't staircase — they
            // are top/bottom surfaces, not slopes. Direction-agnostic on purpose: shallow
            // top slopes and overhang undersides both expose interior rings.
            const double n_cos = std::abs(double(n.z()));
            const double n_sin = std::sqrt(std::max(0.0, 1.0 - n_cos * n_cos));
            if (n_sin > 1e-6) {
                const double tan_alpha = n_cos / n_sin;
                if (tan_alpha >= min_tan_alpha) {
                    const double fz_lo = double(std::min({ v0.z(), v1.z(), v2.z() })) - z0;
                    const double fz_hi = double(std::max({ v0.z(), v1.z(), v2.z() })) - z0;
                    slope_intervals.push_back({ fz_lo, fz_hi, tan_alpha });
                }
            }
        }
    }

    if (!top_intervals.empty()) {
        std::sort(top_intervals.begin(), top_intervals.end());
        out.top_bands.push_back(TopZoneBand{ top_intervals.front().first, top_intervals.front().second });
        for (size_t i = 1; i < top_intervals.size(); ++i) {
            if (top_intervals[i].first <= out.top_bands.back().z_hi_mm)
                out.top_bands.back().z_hi_mm = std::max(out.top_bands.back().z_hi_mm, top_intervals[i].second);
            else
                out.top_bands.push_back(TopZoneBand{ top_intervals[i].first, top_intervals[i].second });
        }
    }

    if (!slope_intervals.empty()) {
        std::sort(slope_intervals.begin(), slope_intervals.end(),
                  [](const SlopeInterval& a, const SlopeInterval& b) { return a.z_lo < b.z_lo; });
        out.slope_bands.push_back(SlopeZoneBand{ slope_intervals.front().z_lo,
                                                 slope_intervals.front().z_hi,
                                                 slope_intervals.front().tan_alpha });
        for (size_t i = 1; i < slope_intervals.size(); ++i) {
            SlopeZoneBand& last = out.slope_bands.back();
            if (slope_intervals[i].z_lo <= last.z_hi_mm) {
                last.z_hi_mm       = std::max(last.z_hi_mm, slope_intervals[i].z_hi);
                last.tan_alpha_max = std::max(last.tan_alpha_max, slope_intervals[i].tan_alpha);
            } else {
                out.slope_bands.push_back(SlopeZoneBand{ slope_intervals[i].z_lo,
                                                         slope_intervals[i].z_hi,
                                                         slope_intervals[i].tan_alpha });
            }
        }
    }

    return out;
}

std::vector<TopZoneBand> compute_top_zone_bands(const ModelObject& object,
                                                double              band_depth_mm,
                                                float               flat_cos_threshold)
{
    // min_tan_alpha <= 0 disables the slope half of the pass entirely.
    return compute_object_zone_scan(object, band_depth_mm, 0.0, flat_cos_threshold).top_bands;
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_ALHCOLOR_TAG_END
