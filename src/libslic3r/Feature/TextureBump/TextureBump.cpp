#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "libslic3r/Algorithm/LineSplit.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

#include "TextureBump.hpp"

// NEOTKO_TEXTUREBUMP_TAG — deterministic image-driven relief. Prior art (idea + 45-deg artifact
// diagnosis, NOT copied code) from the PrusaSlicer/OrcaSlicer community; the slope-limiter and the
// seam-blend implementation below are this fork's own design. See docs/ATTRIBUTION_TEXTURE_BUMP.md.

using namespace Slic3r;

namespace Slic3r::Feature::TextureBump {

// ---------------------------------------------------------------------------------------------
// Image loading (cached by path -- decoding the same PNG on every layer/object would be wasteful).
// ---------------------------------------------------------------------------------------------

std::shared_ptr<const png::ImageGreyscale> load_texture_image(const std::string& path)
{
    static std::mutex                                                          s_mutex;
    static std::unordered_map<std::string, std::shared_ptr<const png::ImageGreyscale>> s_cache;

    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(path);
    if (it != s_cache.end())
        return it->second;

    std::shared_ptr<const png::ImageGreyscale> result;
    std::ifstream in(path, std::ios::binary);
    if (in.is_open()) {
        std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!buf.empty()) {
            png::ReadBuf rbuf{ buf.data(), buf.size() };
            auto img = std::make_shared<png::ImageGreyscale>();
            if (png::decode_png(rbuf, *img) && img->rows > 0 && img->cols > 0)
                result = img;
        }
    }

    NEOTKO_LOG(TEXTUREBUMP, "load_texture_image path='" << path << "' ok=" << (result ? 1 : 0)
        << (result ? (" rows=" + std::to_string(result->rows) + " cols=" + std::to_string(result->cols)) : std::string()));

    s_cache.emplace(path, result);
    return result;
}

double sample_image_bilinear(const png::ImageGreyscale& img, double u, double v)
{
    u = u - std::floor(u);
    v = v - std::floor(v);
    const double fx = u * double(img.cols);
    const double fy = v * double(img.rows);
    const size_t x0 = size_t(std::floor(fx)) % img.cols;
    const size_t y0 = size_t(std::floor(fy)) % img.rows;
    const size_t x1 = (x0 + 1) % img.cols;
    const size_t y1 = (y0 + 1) % img.rows;
    const double tx = fx - std::floor(fx);
    const double ty = fy - std::floor(fy);

    const double v00 = double(img.get(y0, x0)) / 255.0;
    const double v10 = double(img.get(y0, x1)) / 255.0;
    const double v01 = double(img.get(y1, x0)) / 255.0;
    const double v11 = double(img.get(y1, x1)) / 255.0;

    const double top    = v00 * (1.0 - tx) + v10 * tx;
    const double bottom = v01 * (1.0 - tx) + v11 * tx;
    return top * (1.0 - ty) + bottom * ty;
}

// ---------------------------------------------------------------------------------------------
// Seam blending. See docs/ATTRIBUTION_TEXTURE_BUMP.md #2: the root cause of the historical "~45
// deg wall artifact" was a hard cut between cube-map faces with no blend at all. What actually
// matters for print quality is that the DISPLACEMENT VALUE never jumps discontinuously -- it does
// not need to look "seamless" as a texture, it needs to be continuous as a number. Blending both
// sides of every period boundary toward the SAME anchor sample (the image's own u=0 pixel column)
// guarantees that continuity by construction, regardless of whether the source PNG tiles cleanly.
// ---------------------------------------------------------------------------------------------

double smoothstep_seam_blend(double dist, double seam_half_width)
{
    const double t = std::clamp(dist / std::max(seam_half_width, 1e-9), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double sample_seam_blended(const png::ImageGreyscale& img, double u_canonical, int num_periods, double seam_half_width, double v)
{
    num_periods = std::max(num_periods, 1);
    double u_local = std::fmod(u_canonical, 1.0);
    if (u_local < 0.0)
        u_local += 1.0;
    const double phase = std::fmod(u_local * double(num_periods), 1.0); // position within this period, [0,1)
    const double raw   = sample_image_bilinear(img, phase, v) * 2.0 - 1.0;
    if (seam_half_width <= 0.0)
        return raw;

    const double dist = std::min(phase, 1.0 - phase);
    if (dist >= seam_half_width)
        return raw;

    const double t      = smoothstep_seam_blend(dist, seam_half_width); // 0 at the seam, 1 at the band edge
    const double anchor = sample_image_bilinear(img, 0.0, v) * 2.0 - 1.0; // every period's own u=0 sample
    return anchor * (1.0 - t) + raw * t;
}

// ---------------------------------------------------------------------------------------------
// Projection math. See docs/ATTRIBUTION_TEXTURE_BUMP.md #2 for why Cubic is XY-only (this feature
// only ever bumps vertical wall perimeters, never top/bottom surfaces, so the cap/pole cases that
// plague full mesh-displacement cube mapping never arise here) and why Spherical currently reuses
// the Cylindrical angular formula (true polar re-parametrization of V needs the object's per-layer
// silhouette radius, not just its bounding box -- left as future work).
// ---------------------------------------------------------------------------------------------

namespace {

struct PlaneComponents { double a, b; };

PlaneComponents plane_components(const Vec3d& p, TextureProjectionAxis axis, const Vec3d& center)
{
    switch (axis) {
        case TextureProjectionAxis::X: return { p.y() - center.y(), p.z() - center.z() };
        case TextureProjectionAxis::Y: return { p.x() - center.x(), p.z() - center.z() };
        case TextureProjectionAxis::Z:
        default:                        return { p.x() - center.x(), p.y() - center.y() };
    }
}

} // anonymous namespace

double compute_u(const Vec3d& point_mm, const Vec2d& perp_dir_normalized, TextureProjectionMode mode,
                  TextureProjectionAxis axis, const BoundingBoxf3& object_bounds_mm)
{
    const Vec3d center = object_bounds_mm.center();

    switch (mode) {
        case TextureProjectionMode::Cylindrical:
        case TextureProjectionMode::Spherical: {
            const PlaneComponents c = plane_components(point_mm, axis, center);
            const double theta = std::atan2(c.b, c.a);
            return theta / (2.0 * M_PI) + 0.5;
        }
        case TextureProjectionMode::Cubic: {
            // Poikilos' original 4-side dominant-axis selection (see attribution doc), always in
            // the XY plane. `raw_u` is the position along that face's own local axis; the caller
            // (TextureBumpTable) re-wraps it into the [0,1) canonical range with its own period.
            const double normal_radians = std::atan2(perp_dir_normalized.y(), perp_dir_normalized.x());
            const double x = point_mm.x() - center.x();
            const double y = point_mm.y() - center.y();
            const double half_x = std::max(object_bounds_mm.size().x() * 0.5, 1e-6);
            const double half_y = std::max(object_bounds_mm.size().y() * 0.5, 1e-6);
            double face_u;
            int    face_idx;
            if (normal_radians >= -M_PI_4 && normal_radians < M_PI_4) {
                face_idx = 0; face_u = 0.5 + 0.5 * (y / half_y); // East (+X)
            } else if (normal_radians >= M_PI_4 && normal_radians < 3.0 * M_PI_4) {
                face_idx = 1; face_u = 0.5 - 0.5 * (x / half_x); // North (+Y)
            } else if (normal_radians >= -3.0 * M_PI_4 && normal_radians < -M_PI_4) {
                face_idx = 3; face_u = 0.5 + 0.5 * (x / half_x); // South (-Y)
            } else {
                face_idx = 2; face_u = 0.5 - 0.5 * (y / half_y); // West (-X)
            }
            return (double(face_idx) + std::clamp(face_u, 0.0, 1.0)) / 4.0;
        }
        case TextureProjectionMode::Planar:
        default: {
            switch (axis) {
                case TextureProjectionAxis::X:
                    return (point_mm.y() - object_bounds_mm.min.y()) / std::max(object_bounds_mm.size().y(), 1e-6);
                case TextureProjectionAxis::Y:
                    return (point_mm.x() - object_bounds_mm.min.x()) / std::max(object_bounds_mm.size().x(), 1e-6);
                case TextureProjectionAxis::Z:
                default:
                    return (point_mm.x() - object_bounds_mm.min.x()) / std::max(object_bounds_mm.size().x(), 1e-6);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Slope-limiter. This is the piece with no prior art (see docs/ATTRIBUTION_TEXTURE_BUMP.md #2):
// detect, per column, where the texture-driven displacement would change faster between two
// consecutive layers than `max_angle_rad` allows, then smooth that Z window with a localized
// blur (so unaffected regions keep full texture detail) and finish with a hard clamp so the
// guarantee holds even where blur alone isn't enough.
// ---------------------------------------------------------------------------------------------

namespace {

void apply_slope_limiter(const TextureBumpConfig& cfg, const std::vector<coordf_t>& layer_z,
                          size_t num_columns, std::vector<double>& values)
{
    const size_t num_layers = layer_z.size();
    if (num_layers < 2 || num_columns == 0)
        return;

    size_t total_violations = 0;

    for (size_t col = 0; col < num_columns; ++col) {
        std::vector<bool> violated(num_layers, false);
        bool any_violation = false;
        for (size_t li = 1; li < num_layers; ++li) {
            const double dz = std::max(layer_z[li] - layer_z[li - 1], 1e-6);
            const double dr = values[li * num_columns + col] - values[(li - 1) * num_columns + col];
            const double angle = std::atan2(std::abs(dr), dz);
            if (angle > cfg.max_angle_rad) {
                violated[li] = true;
                violated[li - 1] = true;
                any_violation = true;
            }
        }
        if (!any_violation)
            continue;
        ++total_violations;

        if (cfg.blur_strength > 0.0) {
            constexpr int kBlurRadius = 2;
            std::vector<double> blurred(num_layers);
            for (size_t li = 0; li < num_layers; ++li)
                blurred[li] = values[li * num_columns + col];
            for (size_t li = 0; li < num_layers; ++li) {
                if (!violated[li])
                    continue;
                double sum = 0.0;
                int    count = 0;
                for (int off = -kBlurRadius; off <= kBlurRadius; ++off) {
                    const long idx = long(li) + off;
                    if (idx < 0 || idx >= long(num_layers))
                        continue;
                    sum += values[size_t(idx) * num_columns + col];
                    ++count;
                }
                if (count > 0) {
                    const double avg = sum / double(count);
                    blurred[li] = values[li * num_columns + col] * (1.0 - cfg.blur_strength) + avg * cfg.blur_strength;
                }
            }
            for (size_t li = 0; li < num_layers; ++li)
                values[li * num_columns + col] = blurred[li];
        }

        // Hard clamp fallback: guarantees the max angle even where blur alone wasn't enough.
        for (size_t li = 1; li < num_layers; ++li) {
            const double dz     = std::max(layer_z[li] - layer_z[li - 1], 1e-6);
            const double max_dr = std::tan(cfg.max_angle_rad) * dz;
            double&      prev   = values[(li - 1) * num_columns + col];
            double&      cur    = values[li * num_columns + col];
            const double dr     = cur - prev;
            if (std::abs(dr) > max_dr)
                cur = prev + std::copysign(max_dr, dr);
        }
    }

    NEOTKO_LOG(TEXTUREBUMP, "slope_limiter columns=" << num_columns << " layers=" << num_layers
        << " violated_columns=" << total_violations << " max_angle_deg=" << (cfg.max_angle_rad * 180.0 / M_PI)
        << " blur_strength=" << cfg.blur_strength);
}

} // anonymous namespace

// ---------------------------------------------------------------------------------------------
// TextureBumpTable
// ---------------------------------------------------------------------------------------------

void TextureBumpTable::build(const TextureBumpConfig& cfg, const BoundingBoxf3& object_bounds_mm, const std::vector<coordf_t>& layer_z)
{
    m_values.clear();
    m_num_columns = 0;
    m_num_layers  = 0;
    m_bounds_mm   = object_bounds_mm;
    m_config      = cfg;

    if (cfg.type == TextureBumpType::None || cfg.image_path.empty() || layer_z.empty())
        return;

    std::shared_ptr<const png::ImageGreyscale> img = load_texture_image(cfg.image_path);
    if (!img)
        return;

    constexpr size_t kNumColumns   = 256;
    constexpr double kSeamHalfWidth = 0.03; // fraction of one period's u-span blended near each boundary

    m_num_columns = kNumColumns;
    m_num_layers  = layer_z.size();
    m_values.assign(m_num_layers * m_num_columns, 0.0);

    const bool has_seam    = cfg.projection_mode != TextureProjectionMode::Planar;
    const int  num_periods = cfg.projection_mode == TextureProjectionMode::Cubic ? 4 : 1;
    const double scale_mm  = std::max(cfg.scale, 1e-6);

    for (size_t li = 0; li < m_num_layers; ++li) {
        const double v = std::max(layer_z[li], 0.0) / scale_mm;
        for (size_t ci = 0; ci < m_num_columns; ++ci) {
            const double u_canonical = double(ci) / double(m_num_columns);
            const double raw = has_seam
                ? sample_seam_blended(*img, u_canonical, num_periods, kSeamHalfWidth, v)
                : (sample_image_bilinear(*img, u_canonical, v) * 2.0 - 1.0);
            // Stored in mm (not scaled coord_t units) so apply_slope_limiter() can compare
            // directly against layer_z (also mm) when computing the inter-layer angle.
            m_values[li * m_num_columns + ci] = raw * unscale_(cfg.thickness);
        }
    }

    apply_slope_limiter(cfg, layer_z, m_num_columns, m_values);

    NEOTKO_LOG(TEXTUREBUMP, "build image='" << cfg.image_path << "' columns=" << m_num_columns
        << " layers=" << m_num_layers << " mode=" << int(cfg.projection_mode) << " axis=" << int(cfg.axis));
}

double TextureBumpTable::sample(double u_canonical, int layer_idx) const
{
    if (empty() || layer_idx < 0 || size_t(layer_idx) >= m_num_layers)
        return 0.0;

    double u = std::fmod(u_canonical, 1.0);
    if (u < 0.0)
        u += 1.0;
    const double fx = u * double(m_num_columns);
    const size_t x0 = size_t(std::floor(fx)) % m_num_columns;
    const size_t x1 = (x0 + 1) % m_num_columns;
    const double tx = fx - std::floor(fx);

    const double v0 = m_values[size_t(layer_idx) * m_num_columns + x0];
    const double v1 = m_values[size_t(layer_idx) * m_num_columns + x1];
    return v0 * (1.0 - tx) + v1 * tx;
}

// ---------------------------------------------------------------------------------------------
// Per-layer application. Mirrors Slic3r::Feature::FuzzySkin's group_region_by_fuzzify /
// should_fuzzify / apply_fuzzy_skin shape exactly (see FuzzySkin.cpp), but samples the precomputed
// TextureBumpTable instead of a noise module, and this fork's config/struct instead of
// FuzzySkinConfig -- kept fully separate so this feature can never affect Fuzzy Skin's behaviour.
// ---------------------------------------------------------------------------------------------

void group_region_by_texture_bump(PerimeterGenerator& g)
{
    g.regions_by_texture_bump.clear();
    g.has_texture_bump      = false;
    g.has_texture_bump_hole = false;

    std::unordered_map<TextureBumpConfig, SurfacesPtr> regions;
    for (auto region : *g.compatible_regions) {
        const auto&             region_config = region->region().config();
        const TextureBumpConfig cfg{region_config.texture_bump,
                                     scaled<coord_t>(region_config.texture_bump_thickness.value),
                                     scaled<coord_t>(region_config.texture_bump_point_distance.value),
                                     region_config.texture_bump_first_layer,
                                     region_config.texture_bump_projection_mode,
                                     region_config.texture_bump_axis,
                                     region_config.texture_bump_scale.value,
                                     region_config.texture_bump_max_angle.value * M_PI / 180.0,
                                     region_config.texture_bump_blur_strength.value,
                                     region_config.texture_bump_image_path.value};
        auto&                    surfaces = regions[cfg];
        for (const auto& surface : region->slices.surfaces) {
            surfaces.push_back(&surface);
        }

        if (cfg.type != TextureBumpType::None) {
            g.has_texture_bump = true;
            if (cfg.type != TextureBumpType::External) {
                g.has_texture_bump_hole = true;
            }
        }
    }

    if (regions.size() == 1) { // optimization, mirrors group_region_by_fuzzify
        g.regions_by_texture_bump[regions.begin()->first] = {};
        return;
    }

    for (auto& it : regions) {
        g.regions_by_texture_bump[it.first] = offset_ex(it.second, ClipperSafetyOffset);
    }
}

bool should_apply_texture_bump(const TextureBumpConfig& config, const int layer_id, const size_t loop_idx, const bool is_contour)
{
    const auto type = config.type;

    if (type == TextureBumpType::None)
        return false;
    if (!config.first_layer && layer_id <= 0)
        return false;

    const bool apply_contours = loop_idx == 0 || type == TextureBumpType::AllWalls;
    const bool apply_holes    = apply_contours && (type == TextureBumpType::All || type == TextureBumpType::AllWalls);

    return is_contour ? apply_contours : apply_holes;
}

namespace {

// Same interpolated-point walk as FuzzySkin::fuzzy_extrusion_line, but the perpendicular offset
// comes from the precomputed table instead of a noise module. Displacement-only for v1 (no
// Extrusion/Combined modes, unlike Fuzzy Skin -- a texture relief is inherently about visible
// shape, width-only modulation doesn't serve the same purpose).
void texture_bump_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, const PerimeterGenerator& perimeter_generator, const TextureBumpConfig& cfg)
{
    if (!perimeter_generator.texture_bump_table || perimeter_generator.texture_bump_table->empty())
        return;

    const TextureBumpTable& table = *perimeter_generator.texture_bump_table;
    const double            min_dist_between_points = double(cfg.point_distance) * 3.0 / 4.0;
    double                  dist_left_over = min_dist_between_points / 2.0;

    auto* p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());
    for (auto& p1 : ext_lines) {
        if (p0->p == p1.p) { // Connect endpoints.
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            continue;
        }

        Vec2d  p0p1      = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points) {
            Point       pa    = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            const Vec2d perp_dir = perp(p0p1).cast<double>().normalized();
            const Vec3d point_mm(unscale_(pa.x()), unscale_(pa.y()), perimeter_generator.slice_z);
            const double u = compute_u(point_mm, perp_dir, cfg.projection_mode, cfg.axis, table.bounds());
            const double r_mm = table.sample(u, perimeter_generator.layer_id);
            out.emplace_back(pa + (perp_dir * scale_(r_mm)).cast<coord_t>(), p1.w, p1.perimeter_index);
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
        --point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) { // Connect endpoints (same fix as FuzzySkin, see PrusaSlicer #8793).
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }

    if (out.size() >= 3)
        ext_lines = std::move(out);
}

} // anonymous namespace

Polygon apply_texture_bump(const Polygon& polygon, const PerimeterGenerator& /*perimeter_generator*/, size_t /*loop_idx*/, bool /*is_contour*/)
{
    // NEOTKO_TEXTUREBUMP_TAG — v1 scope: Classic perimeter generator path is not implemented yet
    // (the slope-limiter/table is only wired through the Arachne ExtrusionLine path below).
    // Left as a documented gap rather than a silent no-op guess at Classic-mode geometry.
    return polygon;
}

void apply_texture_bump(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, const bool is_contour)
{
    const auto& regions = perimeter_generator.regions_by_texture_bump;
    if (regions.empty())
        return;

    if (regions.size() == 1) { // optimization, mirrors apply_fuzzy_skin
        const auto& config = regions.begin()->first;
        if (should_apply_texture_bump(config, perimeter_generator.layer_id, extrusion->inset_idx, is_contour))
            texture_bump_extrusion_line(extrusion->junctions, perimeter_generator, config);
        return;
    }

    std::vector<std::pair<const TextureBumpConfig&, const ExPolygons&>> applicable_regions;
    applicable_regions.reserve(regions.size());
    for (const auto& region : regions) {
        if (should_apply_texture_bump(region.first, perimeter_generator.layer_id, extrusion->inset_idx, is_contour)) {
            applicable_regions.emplace_back(region.first, region.second);
        }
    }
    if (applicable_regions.empty())
        return;

    for (const auto& r : applicable_regions) {
        const auto splitted = Algorithm::split_line(*extrusion, r.second, false);
        if (splitted.empty())
            continue;

        if (std::all_of(splitted.begin(), splitted.end(), [](const Algorithm::SplitLineJunction& j) { return j.clipped; })) {
            texture_bump_extrusion_line(extrusion->junctions, perimeter_generator, r.first);
        } else {
            const auto                              current_ext = extrusion->junctions;
            std::vector<Arachne::ExtrusionJunction> segment;
            segment.reserve(current_ext.size());
            extrusion->junctions.clear();

            const auto apply_current_segment = [&segment, &extrusion, &r, &perimeter_generator]() {
                extrusion->junctions.push_back(segment.front());
                const auto back = segment.back();
                texture_bump_extrusion_line(segment, perimeter_generator, r.first);
                extrusion->junctions.insert(extrusion->junctions.end(), segment.begin(), segment.end());
                extrusion->junctions.push_back(back);
                segment.clear();
            };

            const auto to_ex_junction = [&current_ext](const Algorithm::SplitLineJunction& j) -> Arachne::ExtrusionJunction {
                Arachne::ExtrusionJunction res = current_ext[j.get_src_index()];
                if (!j.is_src())
                    res.p = j.p;
                return res;
            };

            for (const auto& p : splitted) {
                if (p.clipped) {
                    segment.push_back(to_ex_junction(p));
                } else {
                    if (segment.empty()) {
                        extrusion->junctions.push_back(to_ex_junction(p));
                    } else {
                        segment.push_back(to_ex_junction(p));
                        apply_current_segment();
                    }
                }
            }
            if (!segment.empty())
                apply_current_segment();
        }
    }
}

} // namespace Slic3r::Feature::TextureBump
