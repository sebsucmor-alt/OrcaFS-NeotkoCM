// NEOTKO_ALHCOLOR_TAG_START — Fase 5.0 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md §7bis)
#include "SlopePerimeterRecolor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace ColorSci {

double compute_ledge_depth_mm(double layer_height_mm, double n_cos, double n_sin)
{
    if (layer_height_mm <= 0.0)
        return 0.0;
    // Near-flat facet: d is nominally unbounded — return a value that exposes any realistic
    // wall count instead of dividing by ~0. Callers clamp by wall_loops anyway.
    if (n_sin < 1e-6)
        return 1e3;
    return layer_height_mm * (n_cos / n_sin);
}

int compute_exposed_perimeter_count(double layer_height_mm,
                                    double n_cos,
                                    double n_sin,
                                    double perimeter_width_mm,
                                    int    wall_loops)
{
    if (wall_loops <= 0 || perimeter_width_mm <= 1e-6 || layer_height_mm <= 0.0)
        return 0;

    const double d_mm = compute_ledge_depth_mm(layer_height_mm, n_cos, n_sin);
    const int    n_exposed = int(std::floor(d_mm / perimeter_width_mm));
    return std::clamp(n_exposed, 0, wall_loops);
}

namespace {

// Exact §7bis.b visibility weight of ring k over the ledge [0, d]: fractional overlap of
// the ring's radial band [k*w, (k+1)*w] with [0, d]. Sum over the visible rings is 1.
double ring_visibility(size_t k, double w_mm, double d_mm)
{
    const double lo = double(k) * w_mm;
    const double hi = lo + w_mm;
    const double overlap = std::min(hi, d_mm) - lo;
    return std::max(0.0, overlap) / d_mm;
}

// Area-weighted side-by-side blend of the visible rings (user decision s222): each ring's
// per-channel weight is vis_k * TD opacity. Opaque tools (td ~ 0 -> opacity 1) degrade to
// pure area weighting; translucent tools fold their TD in. Deliberately NOT blend_parallel
// (opacity-only weights — see header).
double delta_e_for_assignment(const std::vector<unsigned int>& assignment,
                              const std::vector<double>&        vis,
                              const Lab&                        target,
                              const Material                    mats[4])
{
    float acc[3] = { 0.f, 0.f, 0.f };
    float wsum[3] = { 0.f, 0.f, 0.f };
    for (size_t k = 0; k < assignment.size(); ++k) {
        const Material& m = mats[std::clamp(int(assignment[k]), 0, 3)];
        float op[3];
        // Optical thickness of the ring = one full reference layer of that material.
        slice_opacity(m, 1.f, op);
        for (int c = 0; c < 3; ++c) {
            const float w = float(vis[k]) * op[c];
            acc[c] += m.rgb[c] * w;
            wsum[c] += w;
        }
    }
    float rgb[3];
    for (int c = 0; c < 3; ++c)
        rgb[c] = wsum[c] > 1e-6f ? std::min(1.f, acc[c] / wsum[c]) : 0.5f;
    return double(delta_e2000(rgb_to_lab(rgb), target));
}

} // namespace

PerimeterColorPlan resolve_perimeter_colors(double                            d_mm,
                                            double                            perimeter_width_mm,
                                            int                               wall_loops,
                                            const Lab&                        target,
                                            const std::vector<unsigned int>&  candidate_tools,
                                            const Material                    mats[4])
{
    PerimeterColorPlan plan;
    if (wall_loops <= 0 || perimeter_width_mm <= 1e-6 || d_mm <= perimeter_width_mm)
        return plan; // clean layer — only the external ring shows, nothing to resolve.

    // Rings with nonzero visible area: external (k=0) + exposed interior ones, capped by the
    // wall count actually printed.
    size_t ring_count = size_t(std::ceil(d_mm / perimeter_width_mm - 1e-9));
    ring_count = std::min(ring_count, size_t(wall_loops));

    std::vector<double> vis(ring_count);
    for (size_t k = 0; k < ring_count; ++k)
        vis[k] = ring_visibility(k, perimeter_width_mm, d_mm);

    const std::vector<unsigned int> candidates =
        candidate_tools.empty() ? std::vector<unsigned int>{ 0 } : candidate_tools;

    // Exhaustive search over candidates^ring_count assignments (odometer-style), both
    // factors small (wall_loops typ. 2-4, candidates <=4) so this is cheap by construction.
    std::vector<unsigned int> assignment(ring_count, candidates[0]);
    std::vector<size_t>       idx(ring_count, 0);

    double best_delta_e = std::numeric_limits<double>::max();
    std::vector<unsigned int> best_assignment = assignment;

    bool done = false;
    while (!done) {
        const double de = delta_e_for_assignment(assignment, vis, target, mats);
        if (de < best_delta_e) {
            best_delta_e = de;
            best_assignment = assignment;
        }

        // Advance the odometer.
        size_t k = 0;
        for (; k < ring_count; ++k) {
            idx[k]++;
            if (idx[k] < candidates.size()) {
                assignment[k] = candidates[idx[k]];
                break;
            }
            idx[k] = 0;
            assignment[k] = candidates[0];
        }
        if (k == ring_count)
            done = true;
    }

    plan.tool_per_perimeter = best_assignment;
    plan.delta_e = best_delta_e;
    return plan;
}

} // namespace ColorSci
} // namespace Slic3r
// NEOTKO_ALHCOLOR_TAG_END
