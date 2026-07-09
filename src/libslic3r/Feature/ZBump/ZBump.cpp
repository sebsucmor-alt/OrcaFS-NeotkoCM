#include "ZBump.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <typeinfo>
#include <unordered_map>

#include "libslic3r/Feature/TextureBump/TextureBump.hpp" // load_texture_image/sample_image_bilinear reuse only (generic PNG utilities, not PathBlend)
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/Print.hpp" // MultiPassSubLayer (reinforcement passes only -- Pass 1 itself needs none of this)
#include "libslic3r/PrintConfig.hpp" // PrintRegionConfig
#include "libslic3r/SurfacePassKind.hpp" // Print.hpp only forward-declares this enum
#include "libslic3r/libslic3r.h" // scale_/scaled/EPSILON

// NEOTKO_ZBUMP_TAG — deterministic image-driven Z relief for the top-fill. Own module: does not
// include or call into Fill.cpp's PathBlend code, and does not share TextureBump's wall-domain
// slope-limiter/table code (only its generic PNG loader is reused). See
// docs/WIP/ZBUMP_TOP_SURFACE_PLAN.md for the design this implements (Pass 1 only).

using namespace Slic3r;

#define ZBUMP_LOG(body) do { if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::ZBUMP)) { \
    std::ostringstream _zbdbg_; _zbdbg_ << body;                                       \
    Slic3r::NeoDebug::write(Slic3r::NeoDebug::ZBUMP, _zbdbg_.str()); } } while (0)

namespace Slic3r::Feature::ZBump {

ZBumpConfig resolve_zbump_config(const PrintRegionConfig& region_config)
{
    ZBumpConfig cfg;
    cfg.enabled        = region_config.zbump_enabled.value;
    cfg.image_path     = region_config.zbump_image_path.value;
    cfg.thickness_mm   = region_config.zbump_thickness.value;
    cfg.scale_mm       = region_config.zbump_scale.value;
    cfg.repeat         = region_config.zbump_repeat.value;
    cfg.offset_x_mm    = region_config.zbump_offset_x.value;
    cfg.offset_y_mm    = region_config.zbump_offset_y.value;
    cfg.edge_margin_mm = region_config.zbump_edge_margin.value;
    cfg.max_slope      = region_config.zbump_max_slope.value;
    cfg.first_layer    = region_config.zbump_first_layer.value;
    cfg.relief_segment_mm = region_config.zbump_relief_segment.value;
    cfg.max_passes        = std::max(1, region_config.zbump_max_passes.value);
    return cfg;
}

// ---------------------------------------------------------------------------------------------
// 2D Gaussian blur — own kernel. Same general Gaussian formula family as Slicing.cpp's
// gauss_kernel (1D, per-layer height smoothing), reimplemented here as a separable 2D pass over
// a height-map grid; that function is a private lambda in an unrelated TU and not exported, so
// there is nothing to reuse even if we wanted to.
// ---------------------------------------------------------------------------------------------
namespace {

std::vector<double> gauss_kernel_1d(unsigned int radius)
{
    const unsigned int  size = 2 * radius + 1;
    std::vector<double> k(size);
    const double sigma          = 0.3 * double(radius - 1) + 0.8;
    const double two_sq_sigma   = 2.0 * sigma * sigma;
    const double norm           = 1.0 / std::sqrt(M_PI * two_sq_sigma);
    for (unsigned int i = 0; i < size; ++i) {
        const double x = double(i) - double(radius);
        k[i] = norm * std::exp(-x * x / two_sq_sigma);
    }
    double sum = 0.0;
    for (double v : k) sum += v;
    if (sum > 1e-9)
        for (double& v : k) v /= sum;
    return k;
}

std::vector<double> gaussian_blur_2d(const std::vector<double>& src, size_t cols, size_t rows, unsigned int radius)
{
    if (radius == 0 || cols == 0 || rows == 0)
        return src;
    const std::vector<double> kernel = gauss_kernel_1d(radius);
    const long                r      = long(radius);

    std::vector<double> tmp(src.size(), 0.0);
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < cols; ++x) {
            double acc = 0.0;
            for (long k = -r; k <= r; ++k) {
                const long xi = std::clamp<long>(long(x) + k, 0, long(cols) - 1);
                acc += src[y * cols + size_t(xi)] * kernel[size_t(k + r)];
            }
            tmp[y * cols + x] = acc;
        }

    std::vector<double> out(src.size(), 0.0);
    for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < cols; ++x) {
            double acc = 0.0;
            for (long k = -r; k <= r; ++k) {
                const long yi = std::clamp<long>(long(y) + k, 0, long(rows) - 1);
                acc += tmp[size_t(yi) * cols + x] * kernel[size_t(k + r)];
            }
            out[y * cols + x] = acc;
        }
    return out;
}

} // namespace

void ZBumpHeightMap::build(const ZBumpConfig& cfg, const BoundingBoxf3& object_bounds_mm)
{
    m_config    = cfg;
    m_bounds_mm = object_bounds_mm;
    m_cols = m_rows = 0;
    m_values.clear();

    if (!cfg.enabled || cfg.image_path.empty() || cfg.thickness_mm <= 0.0)
        return;

    std::shared_ptr<const png::ImageGreyscale> img = Feature::TextureBump::load_texture_image(cfg.image_path);
    if (!img || img->rows == 0 || img->cols == 0) {
        ZBUMP_LOG("build: image load failed path='" << cfg.image_path << "'");
        return;
    }

    const double width_mm  = std::max(0.01, object_bounds_mm.max.x() - object_bounds_mm.min.x());
    const double height_mm = std::max(0.01, object_bounds_mm.max.y() - object_bounds_mm.min.y());
    const double tile_mm   = std::max(0.1, cfg.scale_mm);
    const int    repeat    = std::max(1, cfg.repeat);

    // Grid resolution: driven by the OBJECT's own physical size at a fixed target density
    // (samples/mm), NOT by tile_mm -- the previous width_mm/tile_mm*4 formula degenerated to a
    // handful of samples (e.g. 6x6) whenever a tile was set larger than, or comparable to, the
    // object, since a bigger tile divided the sample count down instead of up (confirmed via
    // ORCA_DEBUG_ZBUMP log on a real print: a brick texture with tile_mm=20 on a ~30mm object
    // came out as a smooth blob, no bricks -- the underlying map never had the resolution to
    // show them, no matter how finely paths were sampled along it). `repeat` still multiplies
    // density: packing N copies of the image into the same physical span (see the `u`/`v` fmod
    // below) raises the effective spatial frequency by N, so it needs N times the samples.
    constexpr double kSamplesPerMm = 4.0;
    m_cols = size_t(std::clamp(width_mm  * kSamplesPerMm * double(repeat), 2.0, 512.0));
    m_rows = size_t(std::clamp(height_mm * kSamplesPerMm * double(repeat), 2.0, 512.0));

    std::vector<double> raw(m_cols * m_rows, 0.0);
    for (size_t r = 0; r < m_rows; ++r) {
        const double y_mm = object_bounds_mm.min.y() + (double(r) + 0.5) / double(m_rows) * height_mm;
        for (size_t c = 0; c < m_cols; ++c) {
            const double x_mm = object_bounds_mm.min.x() + (double(c) + 0.5) / double(m_cols) * width_mm;
            double u = std::fmod((x_mm - cfg.offset_x_mm) / tile_mm * double(repeat), 1.0);
            double v = std::fmod((y_mm - cfg.offset_y_mm) / tile_mm * double(repeat), 1.0);
            if (u < 0.0) u += 1.0;
            if (v < 0.0) v += 1.0;
            const double gray = Feature::TextureBump::sample_image_bilinear(*img, u, v); // [0,1]
            raw[r * m_cols + c] = gray * cfg.thickness_mm;
        }
    }

    // Slope limiter: derive the blur radius (grid cells) from max_slope so post-blur no cell step
    // implies a steeper local slope than configured (own derivation, not TextureBump's per-layer
    // max_angle_rad clamp).
    const double cell_mm         = std::min(width_mm / double(m_cols), height_mm / double(m_rows));
    const double needed_ramp_mm  = (cfg.max_slope > 1e-6) ? (cfg.thickness_mm / cfg.max_slope) : 0.0;
    const unsigned int radius    = (unsigned int)std::clamp(std::round(needed_ramp_mm / std::max(0.01, cell_mm)), 0.0, 32.0);

    m_values = (radius > 0) ? gaussian_blur_2d(raw, m_cols, m_rows, radius) : std::move(raw);

    // NEOTKO_ZBUMP_TAG -- calibration probe (GUI/overlay vs. real slice cross-check). Same 5
    // center-relative test points as GLGizmoTextureBump.cpp's own probe log -- diff the two
    // ORCA_DEBUG_ZBUMP log lines directly: if object_bounds_center or any uv_probe pair disagrees
    // between the two sides, that's the exact quantity/point still out of sync. Uses the identical
    // u/v formula as the grid loop above (not a separate reimplementation) so this can't itself
    // drift from what actually gets baked into the height map.
    if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::ZBUMP)) {
        const Vec3d c = object_bounds_mm.center();
        auto probe_uv = [&](double dx, double dy) {
            double u = std::fmod(((c.x() + dx) - cfg.offset_x_mm) / tile_mm * double(repeat), 1.0);
            double v = std::fmod(((c.y() + dy) - cfg.offset_y_mm) / tile_mm * double(repeat), 1.0);
            if (u < 0.0) u += 1.0;
            if (v < 0.0) v += 1.0;
            ZBUMP_LOG("uv_probe side=engine dx=" << dx << " dy=" << dy
                << " world_x=" << (c.x() + dx) << " world_y=" << (c.y() + dy)
                << " u=" << u << " v=" << v);
        };
        ZBUMP_LOG("overlay_build side=engine object_bounds_min=(" << object_bounds_mm.min.x() << "," << object_bounds_mm.min.y() << ")"
            << " object_bounds_max=(" << object_bounds_mm.max.x() << "," << object_bounds_mm.max.y() << ")"
            << " object_bounds_center=(" << c.x() << "," << c.y() << ")"
            << " tile_mm=" << tile_mm << " repeat=" << repeat
            << " offset_x_mm=" << cfg.offset_x_mm << " offset_y_mm=" << cfg.offset_y_mm);
        probe_uv(0, 0);
        probe_uv(10, 0);
        probe_uv(-10, 0);
        probe_uv(0, 10);
        probe_uv(0, -10);
    }

    ZBUMP_LOG("build image='" << cfg.image_path << "' cols=" << m_cols << " rows=" << m_rows
        << " cell_mm=" << cell_mm << " blur_radius=" << radius);
}

double ZBumpHeightMap::sample_xy(double x_mm, double y_mm) const
{
    if (empty())
        return 0.0;

    const double width_mm  = std::max(0.01, m_bounds_mm.max.x() - m_bounds_mm.min.x());
    const double height_mm = std::max(0.01, m_bounds_mm.max.y() - m_bounds_mm.min.y());

    double fx = (x_mm - m_bounds_mm.min.x()) / width_mm  * double(m_cols) - 0.5;
    double fy = (y_mm - m_bounds_mm.min.y()) / height_mm * double(m_rows) - 0.5;
    fx = std::clamp(fx, 0.0, double(m_cols - 1));
    fy = std::clamp(fy, 0.0, double(m_rows - 1));

    const size_t x0 = size_t(fx), y0 = size_t(fy);
    const size_t x1 = std::min(x0 + 1, m_cols - 1), y1 = std::min(y0 + 1, m_rows - 1);
    const double tx = fx - double(x0), ty = fy - double(y0);

    const double v00 = m_values[y0 * m_cols + x0], v10 = m_values[y0 * m_cols + x1];
    const double v01 = m_values[y1 * m_cols + x0], v11 = m_values[y1 * m_cols + x1];
    const double v0 = v00 * (1.0 - tx) + v10 * tx;
    const double v1 = v01 * (1.0 - tx) + v11 * tx;
    return v0 * (1.0 - ty) + v1 * ty;
}

const ZBumpHeightMap& get_or_build_height_map(const void* object_key, const ZBumpConfig& cfg,
                                               const BoundingBoxf3& object_bounds_mm)
{
    static std::mutex                                  s_mutex;
    static std::unordered_map<const void*, ZBumpHeightMap> s_cache;

    std::lock_guard<std::mutex> lock(s_mutex);
    ZBumpHeightMap& m = s_cache[object_key]; // default-constructed (empty()) on first use
    if (m.config() != cfg)
        m.build(cfg, object_bounds_mm);
    return m;
}

double compute_edge_ramp_factor(double dist_to_edge_mm, double margin_mm)
{
    if (margin_mm <= 1e-6)
        return 1.0;
    const double t = std::clamp(dist_to_edge_mm / margin_mm, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t); // smoothstep
}

double distance_to_expolygon_boundary_mm(const ExPolygon& expoly, const Vec2d& pt_mm)
{
    const Point pt(scaled<coord_t>(pt_mm.x()), scaled<coord_t>(pt_mm.y()));
    double      min_d_scaled = std::numeric_limits<double>::max();
    auto        scan         = [&](const Polygon& poly) {
        for (const Line& l : poly.lines())
            min_d_scaled = std::min(min_d_scaled, l.distance_to(pt));
    };
    scan(expoly.contour);
    for (const Polygon& hole : expoly.holes) scan(hole);
    if (min_d_scaled == std::numeric_limits<double>::max())
        return 0.0;
    return min_d_scaled * SCALING_FACTOR;
}

double compute_pass_cap_mm(double nozzle_diameter_mm, double layer_height_mm)
{
    return std::max(0.0, 0.8 * nozzle_diameter_mm - layer_height_mm);
}

namespace {

// Splits a polyline's point list so no resulting segment exceeds max_segment_scaled (scaled
// units). Every original point is preserved exactly; new points are inserted evenly along
// whichever original segments are too long. Own implementation -- same problem `add_slop`
// (ExtrusionEntity.cpp, scarf-seam) solves via recursive bisection, independently reimplemented
// here rather than shared, same "duplicate rather than reuse across features" rule Pass 1 already
// applied to its own Z-grouping.
Points split_for_relief(const Points& src, double max_segment_scaled)
{
    if (src.size() < 2 || max_segment_scaled <= 1.0)
        return src;
    Points out;
    out.reserve(src.size());
    out.push_back(src.front());
    for (size_t i = 1; i < src.size(); ++i) {
        const Point& a = src[i - 1];
        const Point& b = src[i];
        const double seg_len = (b - a).cast<double>().norm();
        const int    n_sub   = std::max(1, int(std::ceil(seg_len / max_segment_scaled)));
        for (int k = 1; k <= n_sub; ++k) {
            const double t = double(k) / double(n_sub);
            out.emplace_back(coord_t(std::round(double(a.x()) + t * double(b.x() - a.x()))),
                              coord_t(std::round(double(a.y()) + t * double(b.y() - a.y()))));
        }
    }
    return out;
}

} // namespace

void apply_zbump_to_top_fill(
    const ExPolygon& top_surface, ExtrusionEntityCollection& fill_entities,
    const ZBumpHeightMap& height_map, const ZBumpConfig& cfg, double pass_cap_mm)
{
    if (!cfg.enabled || height_map.empty())
        return;

    // NEOTKO_ZBUMP_TAG — Pass 1's own ceiling is the SMALLER of the user's total target and
    // what one physical pass can safely reach; the rest (if any) is left for
    // apply_zbump_reinforcement_passes() to build on top, not silently dropped.
    const double pass1_cap_mm       = std::min(cfg.thickness_mm, std::max(0.0, pass_cap_mm));
    const double max_segment_scaled = scale_(std::max(0.1, cfg.relief_segment_mm));

    // NEOTKO_ZBUMP_TAG — same diagnostic-counters-first habit that found Pass 1's real bugs
    // (s184): a one-log-line answer to "which stage discarded everything" instead of a guess.
    size_t dbg_total = 0, dbg_path_type = 0, dbg_multipath_type = 0, dbg_loop_type = 0,
           dbg_collection_type = 0, dbg_other_type = 0, dbg_paths_seen = 0, dbg_paths_bumped = 0,
           dbg_points_out = 0;

    // Top-level entries of a top solid-infill fill vary by pattern: a plain ExtrusionPath, a
    // connected zigzag bundled as one ExtrusionMultiPath (rectilinear/monotonic-style patterns),
    // an ExtrusionLoop (concentric-style patterns -- same `ExtrusionPaths paths` shape as
    // MultiPath, just a different wrapper class), or a nested ExtrusionEntityCollection. All four
    // are unwrapped/recursed into. Splits `path` into relief-sized sub-segments and samples the
    // height map at every resulting point, writing the offsets straight onto `path` in place --
    // no clone, no move between collections, no erase. Left untouched (empty zbump_z_offset) if
    // nothing along it clears the flush threshold, same "don't touch what isn't bumped" behavior
    // Pass 1 had.
    auto bump_one_path = [&](ExtrusionPath& path) {
        ++dbg_paths_seen;
        if (path.polyline.points.size() < 2)
            return;

        Points new_points = split_for_relief(path.polyline.points, max_segment_scaled);

        std::vector<double> h_mm(new_points.size(), 0.0);
        bool any_bumped = false;
        for (size_t i = 0; i < new_points.size(); ++i) {
            const Vec2d  pt_mm = unscale(new_points[i]);
            const double raw_h = height_map.sample_xy(pt_mm.x(), pt_mm.y());
            if (raw_h <= 1e-6)
                continue;
            const double dist_mm = distance_to_expolygon_boundary_mm(top_surface, pt_mm);
            const double ramp    = compute_edge_ramp_factor(dist_mm, cfg.edge_margin_mm);
            const double h_p     = std::clamp(raw_h * ramp, 0.0, pass1_cap_mm);
            if (h_p > 1e-6) {
                h_mm[i]    = h_p;
                any_bumped = true;
            }
        }
        if (!any_bumped)
            return;

        ++dbg_paths_bumped;
        dbg_points_out += new_points.size();
        path.polyline.points = std::move(new_points);
        path.zbump_z_offset.resize(h_mm.size());
        for (size_t i = 0; i < h_mm.size(); ++i)
            path.zbump_z_offset[i] = scaled<coord_t>(h_mm[i]);
    };

    // Shared by ExtrusionMultiPath and ExtrusionLoop -- both hold an `ExtrusionPaths paths` member
    // (plain std::vector<ExtrusionPath>), identical shape, different wrapper class.
    auto process_subpaths = [&](ExtrusionPaths& paths) {
        for (ExtrusionPath& p : paths)
            bump_one_path(p);
    };

    // Recursive walk over one container's top-level entities. Nothing is ever erased, cloned, or
    // moved -- every leaf keeps its identity and position in the tree, only its polyline/
    // zbump_z_offset may change.
    std::function<void(ExtrusionEntityCollection&)> walk = [&](ExtrusionEntityCollection& container) {
        for (ExtrusionEntity* e : container.entities) {
            ++dbg_total;
            if (auto* p = dynamic_cast<ExtrusionPath*>(e)) {
                ++dbg_path_type;
                bump_one_path(*p);
            } else if (auto* mp = dynamic_cast<ExtrusionMultiPath*>(e)) {
                ++dbg_multipath_type;
                process_subpaths(mp->paths);
            } else if (auto* lp = dynamic_cast<ExtrusionLoop*>(e)) {
                ++dbg_loop_type;
                process_subpaths(lp->paths);
            } else if (auto* nested = dynamic_cast<ExtrusionEntityCollection*>(e)) {
                ++dbg_collection_type;
                walk(*nested);
            } else {
                ++dbg_other_type;
                ZBUMP_LOG("apply_zbump_to_top_fill: unhandled entity type '" << typeid(*e).name() << "' -- left unbumped");
            }
        }
    };
    walk(fill_entities);

    ZBUMP_LOG("apply_zbump_to_top_fill total_entities=" << dbg_total
        << " path_type=" << dbg_path_type << " multipath_type=" << dbg_multipath_type
        << " loop_type=" << dbg_loop_type << " collection_type=" << dbg_collection_type
        << " other_type=" << dbg_other_type << " paths_seen=" << dbg_paths_seen
        << " paths_bumped=" << dbg_paths_bumped << " points_out=" << dbg_points_out
        << " thickness_mm=" << cfg.thickness_mm << " edge_margin_mm=" << cfg.edge_margin_mm
        << " relief_segment_mm=" << cfg.relief_segment_mm);
}

void apply_zbump_reinforcement_passes(
    const ExPolygon& top_surface, ExtrusionEntityCollection& fill_entities,
    double top_nominal_z, int tool_id, ExtrusionRole role,
    const ZBumpHeightMap& height_map, const ZBumpConfig& cfg, double pass_cap_mm,
    int& global_pass, std::vector<MultiPassSubLayer>& out_sublayers)
{
    if (!cfg.enabled || height_map.empty() || cfg.max_passes <= 1 || pass_cap_mm <= 1e-6)
        return;

    for (int p = 1; p < cfg.max_passes; ++p) {
        const double base_height_so_far_mm = double(p) * pass_cap_mm;
        const double budget_left_mm        = cfg.thickness_mm - base_height_so_far_mm;
        if (budget_left_mm <= 1e-6)
            break; // earlier passes already cover the user's whole target -- nothing more to add
        const double this_pass_cap_mm = std::min(pass_cap_mm, budget_left_mm);

        ExtrusionEntityCollection pass_fills;
        size_t dbg_paths_seen = 0, dbg_runs = 0, dbg_points_out = 0;

        // Re-samples the SAME height map/edge ramp Pass 1 used (own recomputation, not shared
        // state -- these are pure functions, cheap to call again) to find how much MORE height
        // this specific pass still owes each point, then slices out contiguous runs of "needs
        // this pass" points as their own small ExtrusionPath clones -- each run is, by design, an
        // isolated island with its own travel/retract (see the plan's cost note on fine texture).
        auto process_path = [&](const ExtrusionPath& path) {
            ++dbg_paths_seen;
            const size_t n = path.polyline.points.size();
            if (n < 2 || path.zbump_z_offset.size() != n)
                return; // defensive: only true for paths Pass 1 actually bumped

            std::vector<double> remaining_mm(n, 0.0);
            bool any_needed = false;
            for (size_t i = 0; i < n; ++i) {
                const Vec2d  pt_mm = unscale(path.polyline.points[i]);
                const double raw_h = height_map.sample_xy(pt_mm.x(), pt_mm.y());
                if (raw_h <= 1e-6)
                    continue;
                const double dist_mm   = distance_to_expolygon_boundary_mm(top_surface, pt_mm);
                const double edge_ramp = compute_edge_ramp_factor(dist_mm, cfg.edge_margin_mm);
                const double desired_h = raw_h * edge_ramp;
                const double r = std::clamp(desired_h - base_height_so_far_mm, 0.0, this_pass_cap_mm);
                if (r > 1e-6) {
                    remaining_mm[i] = r;
                    any_needed      = true;
                }
            }
            if (!any_needed)
                return;

            size_t i = 0;
            while (i < n) {
                if (remaining_mm[i] <= 1e-6) {
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < n && remaining_mm[j] > 1e-6) ++j;

                // Run is [i, j). Own cumulative arc length so this reinforcement patch fades to
                // 0 at its OWN start/end (not the top surface's contour) -- reuses
                // compute_edge_ramp_factor's smoothstep, it is already generic over "distance"
                // and "margin", no need for a second implementation.
                const size_t run_len = j - i;
                std::vector<double> cum_mm(run_len, 0.0);
                for (size_t k = 1; k < run_len; ++k) {
                    const Vec2d a = unscale(path.polyline.points[i + k - 1]);
                    const Vec2d b = unscale(path.polyline.points[i + k]);
                    cum_mm[k]     = cum_mm[k - 1] + (b - a).norm();
                }
                const double run_total_mm = cum_mm.back();

                ExtrusionPath run_path(path);
                run_path.polyline.points.assign(path.polyline.points.begin() + long(i),
                                                 path.polyline.points.begin() + long(j));
                run_path.polyline.fitting_result.clear();
                run_path.zbump_z_offset.assign(run_len, coord_t(0));
                bool any_final = false;
                for (size_t k = 0; k < run_len; ++k) {
                    const double dist_to_run_edge = std::min(cum_mm[k], run_total_mm - cum_mm[k]);
                    const double run_ramp         = compute_edge_ramp_factor(dist_to_run_edge, cfg.edge_margin_mm);
                    const double final_h          = remaining_mm[i + k] * run_ramp;
                    if (final_h > 1e-6)
                        any_final = true;
                    run_path.zbump_z_offset[k] = scaled<coord_t>(final_h);
                }
                if (any_final) {
                    ++dbg_runs;
                    dbg_points_out += run_len;
                    pass_fills.entities.push_back(new ExtrusionPath(std::move(run_path)));
                }
                i = j;
            }
        };

        // Same 4-shape walk as apply_zbump_to_top_fill's (own copy, not shared -- see that
        // function's comment on why -- read-only here, we never touch Pass 1's own paths).
        std::function<void(const ExtrusionEntityCollection&)> walk_bumped =
            [&](const ExtrusionEntityCollection& container) {
            for (const ExtrusionEntity* e : container.entities) {
                if (auto* pp = dynamic_cast<const ExtrusionPath*>(e)) {
                    if (!pp->zbump_z_offset.empty()) process_path(*pp);
                } else if (auto* mp = dynamic_cast<const ExtrusionMultiPath*>(e)) {
                    for (const ExtrusionPath& sp : mp->paths)
                        if (!sp.zbump_z_offset.empty()) process_path(sp);
                } else if (auto* lp = dynamic_cast<const ExtrusionLoop*>(e)) {
                    for (const ExtrusionPath& sp : lp->paths)
                        if (!sp.zbump_z_offset.empty()) process_path(sp);
                } else if (auto* nested = dynamic_cast<const ExtrusionEntityCollection*>(e)) {
                    walk_bumped(*nested);
                }
            }
        };
        walk_bumped(fill_entities);

        ZBUMP_LOG("apply_zbump_reinforcement_passes pass=" << p << " base_height_mm=" << base_height_so_far_mm
            << " this_pass_cap_mm=" << this_pass_cap_mm << " paths_seen=" << dbg_paths_seen
            << " runs=" << dbg_runs << " points_out=" << dbg_points_out);

        if (pass_fills.entities.empty())
            break; // this pass needed nothing anywhere -- higher passes need even more, so stop

        MultiPassSubLayer sub;
        // NEOTKO_ZBUMP_TAG — fix (real bug, print-confirmed): PathBlend schedules its ramp/cap
        // BELOW canonical (Fill.cpp, `bottom_z()+height-2*EPSILON`) because its sublayers REPLACE
        // that layer's own top-fill content -- nothing else needs to print before them at that Z.
        // Ours is different: the real layer (perimeters + Pass 1's own inline relief) must print
        // FIRST, with each reinforcement pass strictly after it. collect_layers_to_print()
        // (GCode.cpp:2107-2121) globally sorts every real layer AND sublayer by ascending
        // print_z, merging entries within `+EPSILON` of each other's bucket into one group -- so
        // a NEGATIVE offset here schedules us BEFORE the real layer (confirmed print: passes
        // came out MED-HIGH-LOW instead of LOW-MED-HIGH), and an offset smaller than EPSILON
        // between two passes would merge them into the same bucket (the same "MIXED GROUP" bug
        // PathBlend's offset avoids, just via a different failure direction). Fix: schedule
        // ABOVE canonical instead, `2*EPSILON` apart per pass -- comfortably separates every
        // pass from the real layer AND from each other, while staying far below the next real
        // layer up (layer_height is always orders of magnitude bigger than a few EPSILON).
        sub.print_z        = top_nominal_z + double(p) * 2.0 * EPSILON;
        sub.height          = static_cast<const ExtrusionPath*>(pass_fills.entities.front())->height;
        sub.real_extrude_z  = top_nominal_z + base_height_so_far_mm;
        sub.pass_idx        = global_pass++;
        sub.role            = role;
        sub.effect          = SurfacePassKind::ZBump;
        sub.tool_id         = tool_id;
        sub.speed_pct       = 100;
        sub.pathblend_pass  = -1; // explicitly not a PathBlend pass
        sub.fills           = std::move(pass_fills);
        out_sublayers.push_back(std::move(sub));
    }
}

} // namespace Slic3r::Feature::ZBump
