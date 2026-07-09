#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "libslic3r/Algorithm/LineSplit.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp" // needed for PrintRegion::config() in group_region_by_texture_bump

#include "TextureBump.hpp"
#include "TextureBumpZone.hpp"

// NEOTKO_TEXTUREBUMP_TAG — deterministic image-driven relief. Prior art (idea + 45-deg artifact
// diagnosis, NOT copied code) from the PrusaSlicer/OrcaSlicer community; the slope-limiter and the
// seam-blend implementation below are this fork's own design. See docs/ATTRIBUTION_TEXTURE_BUMP.md.

using namespace Slic3r;

// Local mirror of SurfaceColorMix.hpp's NEOTKO_LOG macro (same pattern NeoTower.cpp uses as
// NT_LOG) so this file doesn't need to pull in the unrelated, heavy SurfaceColorMix.hpp just for
// one logging macro.
#define TEXTUREBUMP_LOG(body) do { if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::TEXTUREBUMP)) { \
    std::ostringstream _tbdbg_; _tbdbg_ << body;                                                   \
    Slic3r::NeoDebug::write(Slic3r::NeoDebug::TEXTUREBUMP, _tbdbg_.str()); } } while (0)

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

    std::shared_ptr<png::ImageGreyscale> result;
    bool                                 converted_from_color = false;
    std::ifstream in(path, std::ios::binary);
    if (in.is_open()) {
        std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!buf.empty()) {
            png::ReadBuf rbuf{ buf.data(), buf.size() };
            auto img = std::make_shared<png::ImageGreyscale>();
            if (png::decode_png(rbuf, *img) && img->rows > 0 && img->cols > 0) {
                result = img;
            } else {
                // NEOTKO_TEXTUREBUMP_TAG — decode_png() only accepts true 8-bit grayscale PNGs.
                // Most user-supplied PNGs (screenshots, exports from Photoshop/GIMP) are RGB or
                // RGBA -- this exact friction is what tripped up the community on the original
                // Orca PR (see docs/ATTRIBUTION_TEXTURE_BUMP.md). Fall back to the colored decoder
                // and convert to luma so any PNG works, not just pre-converted ones.
                png::ImageColorscale cimg;
                if (png::decode_colored_png(rbuf, cimg) && cimg.rows > 0 && cimg.cols > 0 && cimg.bytes_per_pixel >= 1) {
                    auto gimg = std::make_shared<png::ImageGreyscale>();
                    gimg->rows = cimg.rows;
                    gimg->cols = cimg.cols;
                    gimg->buf.resize(cimg.rows * cimg.cols);
                    const int bpp = cimg.bytes_per_pixel;
                    for (size_t r = 0; r < cimg.rows; ++r) {
                        for (size_t c = 0; c < cimg.cols; ++c) {
                            const size_t base = (r * cimg.cols + c) * size_t(bpp);
                            uint8_t      luma;
                            if (bpp >= 3) {
                                const double rr = double(cimg.buf[base + 0]);
                                const double gg = double(cimg.buf[base + 1]);
                                const double bb = double(cimg.buf[base + 2]);
                                luma = uint8_t(std::clamp(0.299 * rr + 0.587 * gg + 0.114 * bb, 0.0, 255.0));
                            } else {
                                luma = cimg.buf[base]; // grayscale or grayscale+alpha
                            }
                            gimg->buf[r * cimg.cols + c] = luma;
                        }
                    }
                    result = gimg;
                    converted_from_color = true;
                }
            }
        }
    }

    TEXTUREBUMP_LOG("load_texture_image path='" << path << "' ok=" << (result ? 1 : 0)
        << " converted_from_color=" << (converted_from_color ? 1 : 0)
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

// NEOTKO_TEXTUREBUMP_TAG -- Fase 4.2: applies plane_transform's yaw + pivot WITHIN the (a,b)
// component space plane_components() already selected for the given axis. Identity leaves (a,b)
// untouched, so every call site that doesn't pass a transform (or passes the default) is exactly
// the pre-Fase-4.2 behavior, bit for bit -- verified algebraically: for axis==Z, undoing the
// center-relative reframing this function does (a = point_mm.x() - center.x()) reproduces
// Planar's original object_bounds_mm.min-relative formula exactly once rotation is Identity and
// translation is zero (see compute_u()'s Planar branch below).
PlaneComponents apply_plane_transform(const PlaneComponents& c, const Transform3d& plane_transform)
{
    const auto&  lin    = plane_transform.linear();
    const double yaw    = std::atan2(lin(1, 0), lin(0, 0));
    const Vec3d& t      = plane_transform.translation();
    const double da     = c.a - t.x();
    const double db     = c.b - t.y();
    const double cos_yaw = std::cos(yaw), sin_yaw = std::sin(yaw);
    return { cos_yaw * da + sin_yaw * db, -sin_yaw * da + cos_yaw * db };
}

} // anonymous namespace

double compute_u(const Vec3d& point_mm, const Vec2d& perp_dir_normalized, TextureProjectionMode mode,
                  TextureProjectionAxis axis, const BoundingBoxf3& object_bounds_mm, const Transform3d& plane_transform)
{
    const Vec3d center = object_bounds_mm.center();

    switch (mode) {
        case TextureProjectionMode::Cylindrical:
        case TextureProjectionMode::Spherical: {
            const PlaneComponents c = apply_plane_transform(plane_components(point_mm, axis, center), plane_transform);
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
            double face_u = 0.0;
            int    face_idx = 0;
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
            // NEOTKO_TEXTUREBUMP_TAG -- Fase 4.2: reframed via plane_components()/
            // apply_plane_transform() (same helpers Cylindrical/Spherical use above) instead of a
            // separate per-axis one-liner, so plane_transform's yaw+pivot generalizes here too.
            // Identity reduces to the exact original formula -- verified algebraically: `c.a` here
            // equals (point_mm.{x,y} - center.{x,y}), and center = min + size/2, so
            // c.a / divisor + 0.5 == (point_mm - min) / divisor, matching the original min-relative
            // formula bit for bit. `divisor` is whichever size component plane_components() paired
            // with axis's `a` (size.y() for axis==X, size.x() otherwise) -- axis's `b` component
            // (the unused dimension for Planar) is intentionally ignored, same as before.
            const PlaneComponents c = apply_plane_transform(plane_components(point_mm, axis, center), plane_transform);
            const double divisor = (axis == TextureProjectionAxis::X)
                ? std::max(object_bounds_mm.size().y(), 1e-6)
                : std::max(object_bounds_mm.size().x(), 1e-6);
            return c.a / divisor + 0.5;
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

    TEXTUREBUMP_LOG("slope_limiter columns=" << num_columns << " layers=" << num_layers
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

    // NEOTKO_TEXTUREBUMP_TAG -- Phase 1 (see docs/ATTRIBUTION_TEXTURE_BUMP.md): repeat_u multiplies
    // the mode's base period count (Cubic already has a fixed 4-face period; everything else is 1),
    // giving "N repeats per face" for Cubic and "N repeats around the loop" elsewhere. has_seam must
    // trigger whenever num_periods > 1 -- not just for non-Planar modes as before -- because Planar
    // with repeat_u > 1 now introduces internal period boundaries (a repeating tile pattern) that
    // need the same continuity blend Cylindrical/Spherical/Cubic already get at their wrap boundary;
    // at the default repeat_u=1, this reduces to the prior condition exactly (zero regression).
    const int    base_periods = cfg.projection_mode == TextureProjectionMode::Cubic ? 4 : 1;
    const int    num_periods  = base_periods * std::max(cfg.repeat_u, 1);
    const bool   has_seam     = cfg.projection_mode != TextureProjectionMode::Planar || num_periods > 1;
    const double scale_mm     = std::max(cfg.scale, 1e-6);

    for (size_t li = 0; li < m_num_layers; ++li) {
        const double v = std::max(layer_z[li], 0.0) / scale_mm;
        for (size_t ci = 0; ci < m_num_columns; ++ci) {
            const double u_canonical = double(ci) / double(m_num_columns);
            const double raw = has_seam
                ? sample_seam_blended(*img, u_canonical, num_periods, kSeamHalfWidth, v)
                : (sample_image_bilinear(*img, u_canonical, v) * 2.0 - 1.0);
            // NEOTKO_TEXTUREBUMP_TAG — outward-only: remap raw from [-1,1] to [0,1] before scaling
            // by thickness, so the wall is only ever pushed away from the object's interior, never
            // toward it. This is what actually prevents infill from invading the space a bumped
            // wall now occupies (a symmetric +/-thickness range could move the wall inward, into
            // infill computed from the unperturbed nominal geometry) -- no infill-side margin or
            // multi-perimeter taper needed once inward displacement is impossible by construction.
            // Stored in mm (not scaled coord_t units) so apply_slope_limiter() can compare
            // directly against layer_z (also mm) when computing the inter-layer angle.
            m_values[li * m_num_columns + ci] = ((raw + 1.0) * 0.5) * unscale_(cfg.thickness);
        }
    }

    apply_slope_limiter(cfg, layer_z, m_num_columns, m_values);

    TEXTUREBUMP_LOG("build image='" << cfg.image_path << "' columns=" << m_num_columns
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
// NEOTKO_TEXTUREBUMP_TAG — Fase 3: one table per distinct config in use (was a single shared
// table built from region(0) only).
// ---------------------------------------------------------------------------------------------

void build_tables_for_configs(TextureBumpTableMap& tables, const std::vector<TextureBumpConfig>& desired_configs,
                               const BoundingBoxf3& object_bounds_mm, const std::vector<coordf_t>& layer_z)
{
    for (const auto& cfg : desired_configs) {
        if (tables.find(cfg) != tables.end())
            continue; // build() is deterministic per config; an exact-key match needs no rebuild
        TextureBumpTable table;
        table.build(cfg, object_bounds_mm, layer_z);
        tables.emplace(cfg, std::move(table));
    }
    for (auto it = tables.begin(); it != tables.end(); ) {
        const bool still_desired = std::find(desired_configs.begin(), desired_configs.end(), it->first) != desired_configs.end();
        if (still_desired) ++it; else it = tables.erase(it);
    }
}

// ---------------------------------------------------------------------------------------------
// NEOTKO_TEXTUREBUMP_TAG — Fase 3: painted-zone spatial resolution, own canvas
// (ModelVolume::texture_bump_paint_facets), per-LAYER resolution (not a wide top/penu band like
// SurfaceColorMix::painted_footprint_in_z_range). Mirrors that function's scan/frame (must use
// po->trafo_centered(), never po->trafo() -- s161 lesson, see lessons_key.md) but does not filter
// by triangle normal: Texture Bump paints walls, not horizontal top/bottom surfaces, so any
// painted triangle whose Z range intersects this layer's slab counts regardless of facing.
// ---------------------------------------------------------------------------------------------

namespace {

// NEOTKO_TEXTUREBUMP_TAG -- Bug 3 follow-up #2 (s181, see docs/ATTRIBUTION_TEXTURE_BUMP.md §5
// point 3): the first attempt at an anisotropic corridor (per-EDGE, reach perpendicular to each
// edge) was wrong -- a wall triangle typically has one edge running along the wall's own surface
// (real XY length, a meaningful direction) and one or two edges running mostly along Z (paint
// height), whose XY projection is nearly a single point. The "perpendicular direction" of a
// near-zero-length 2D vector is essentially noise, so the 5mm reach ended up smeared sideways in
// a near-random XY direction instead of into the wall (s181 report: a thin vertical "I" stroke
// came out horizontally stretched into a "-|||-" bar). The direction that's ALWAYS meaningful,
// regardless of which edge is degenerate, is the triangle's own FACE NORMAL projected to XY: it
// points straight out of the wall surface. Reach generously along it (still needed --
// Algorithm::split_line clips the actual extrusion centerline, inset from the true surface); stay
// tight along the perpendicular (tangent) axis, which correctly covers both the wall's
// circumferential width and its Z/height extent (height is already bounded by the z_min/z_max
// slab filter above this function, so this only needs to not blow it up sideways in XY).
Polygon oriented_corridor_rect_for_triangle(const Vec3d& v0, const Vec3d& v1, const Vec3d& v2,
                                             double perp_reach_mm, double tangent_margin_mm)
{
    const Vec2d  p0(v0.x(), v0.y()), p1(v1.x(), v1.y()), p2(v2.x(), v2.y());
    const Vec3d  face_normal_3d = (v1 - v0).cross(v2 - v0);
    const Vec2d  n_xy(face_normal_3d.x(), face_normal_3d.y());
    const double n_xy_len = n_xy.norm();
    const double n3d_len  = face_normal_3d.norm();

    // Near-horizontal triangle (top/bottom cap): its raw XY projection already has real area, so
    // a tiny isotropic margin (same as the original Bug 2 fix) is enough -- there's no single
    // "into the wall" direction to chase here, and the fallback below doesn't need one.
    if (n3d_len < 1e-12 || n_xy_len < 0.3 * n3d_len) {
        Polyline pl;
        pl.points = {Point(scale_(v0.x()), scale_(v0.y())), Point(scale_(v1.x()), scale_(v1.y())),
                     Point(scale_(v2.x()), scale_(v2.y())), Point(scale_(v0.x()), scale_(v0.y()))};
        const Polygons small = offset(pl, float(scale_(tangent_margin_mm)));
        return small.empty() ? Polygon() : small.front();
    }

    const Vec2d n_dir = n_xy / n_xy_len;      // out of the wall surface -- the reach direction
    const Vec2d t_dir(-n_dir.y(), n_dir.x()); // along the wall's own surface -- keep tight

    const double n0 = p0.dot(n_dir), n1 = p1.dot(n_dir), n2 = p2.dot(n_dir);
    const double t0 = p0.dot(t_dir), t1 = p1.dot(t_dir), t2 = p2.dot(t_dir);
    const double n_min = std::min({n0, n1, n2}) - perp_reach_mm;
    const double n_max = std::max({n0, n1, n2}) + perp_reach_mm;
    const double t_min = std::min({t0, t1, t2}) - tangent_margin_mm;
    const double t_max = std::max({t0, t1, t2}) + tangent_margin_mm;

    auto corner = [&](double n_val, double t_val) -> Point {
        const Vec2d p = n_dir * n_val + t_dir * t_val;
        return Point(scale_(p.x()), scale_(p.y()));
    };

    Polygon poly;
    poly.points = {corner(n_min, t_min), corner(n_max, t_min), corner(n_max, t_max), corner(n_min, t_max)};
    return poly;
}

} // namespace

std::vector<PaintedTextureBumpZone> painted_texture_bump_zones_in_layer(const PrintObject* po, double slice_z, double layer_height)
{
    std::vector<PaintedTextureBumpZone> out;
    if (!po)
        return out;
    // Same WIP gate as the rest of this feature (see PrintObject::make_perimeters()).
    if (!(po->config().neotko_libre_mode.value && NeoDebug::enabled(NeoDebug::TEXTUREBUMP)))
        return out;
    const ModelObject* mo = po->model_object();
    if (!mo)
        return out;

    const double z_min = slice_z - layer_height / 2.0;
    const double z_max = slice_z + layer_height / 2.0;
    const double z_tol = 0.02; // same fp slack SurfaceColorMix uses for its own Z-band scans

    const Transform3d trafo = po->trafo_centered(); // NEVER po->trafo() -- see lessons_key (s161)

    // Accumulate raw triangle projections per resolved config so zones sharing the same
    // TextureBumpConfig end up merged into one regions_by_texture_bump entry downstream.
    std::unordered_map<TextureBumpConfig, Polygons> tris_by_cfg;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part())
            continue;
        const Transform3d vt = trafo * mv->get_matrix();
        for (int slot = 1; slot < ModelVolume::COLORMIX_SLOT_COUNT; ++slot) {
            const int zone_id = mv->texture_bump_slot_to_zone_id[slot];
            if (zone_id == 0)
                continue; // unpainted
            const TextureBumpZoneProfile* zone = TextureBumpZoneManager::get().find(zone_id);
            if (!zone || zone->config.type == TextureBumpType::None)
                continue; // orphaned slot (zone deleted) or zone disabled -- ignore, don't guess

            const indexed_triangle_set its = mv->texture_bump_paint_facets.get_facets(*mv, static_cast<EnforcerBlockerType>(slot));
            if (its.indices.empty())
                continue;

            // NEOTKO_TEXTUREBUMP_TAG — debug-only instrumentation (ORCA_DEBUG_TEXTUREBUMP), added
            // to diagnose why the painted zone only seemed to apply on the topmost layer: reports
            // the RAW (pre-Z-filter) Z range of every painted triangle for this slot, so we can see
            // whether the paint is actually clustered near the top (e.g. spilled onto the top cap)
            // vs. a transform/frame bug. No behavior change.
            double _dbg_raw_min_z = 1e30, _dbg_raw_max_z = -1e30;
            int    _dbg_raw_tris = 0, _dbg_matched_tris = 0;

            Polygons& tris = tris_by_cfg[zone->config];
            for (const auto& tri : its.indices) {
                const Vec3f& v0f = its.vertices[tri[0]];
                const Vec3f& v1f = its.vertices[tri[1]];
                const Vec3f& v2f = its.vertices[tri[2]];
                const Vec3d  v0  = vt * Vec3d(v0f.x(), v0f.y(), v0f.z());
                const Vec3d  v1  = vt * Vec3d(v1f.x(), v1f.y(), v1f.z());
                const Vec3d  v2  = vt * Vec3d(v2f.x(), v2f.y(), v2f.z());
                const double tri_min_z = std::min({v0.z(), v1.z(), v2.z()});
                const double tri_max_z = std::max({v0.z(), v1.z(), v2.z()});
                ++_dbg_raw_tris;
                _dbg_raw_min_z = std::min(_dbg_raw_min_z, tri_min_z);
                _dbg_raw_max_z = std::max(_dbg_raw_max_z, tri_max_z);
                if (tri_max_z < z_min - z_tol || tri_min_z > z_max + z_tol)
                    continue; // no overlap with this layer's slab
                ++_dbg_matched_tris;
                // NEOTKO_TEXTUREBUMP_TAG — root cause of the "only the topmost layer shows the
                // effect" bug: a straight top-down projection of a WALL triangle (near-vertical,
                // the normal case for a painted side face) collapses to a zero-area sliver in XY
                // -- Clipper silently drops zero-area geometry, so every wall triangle vanished
                // from the mask everywhere except where the mesh happened to have real XY area
                // (e.g. the top cap, whose triangles are horizontal). Unlike
                // SurfaceColorMix::painted_footprint_in_z_range (top/bottom surfaces only, where a
                // straight projection IS the real footprint), a wall's meaningful footprint at a
                // given layer is "where along the loop, in XY, does painted geometry exist at this
                // Z" -- not a projected area. Fix: treat the (possibly degenerate) 3 projected
                // points as a closed POLYLINE and inflate it into a thin real polygon via offset()
                // -- this works identically whether the source triangle is vertical (wall,
                // otherwise-zero-area) or horizontal (top/bottom cap, already real area; the
                // inflate just adds a negligible margin).
                //
                // NEOTKO_TEXTUREBUMP_TAG -- follow-up fix: 0.05mm was too thin. The mask gets
                // clipped against `all_base` (the true mesh contour, group_region_by_texture_bump)
                // -- that step survived every layer -- but the THING Algorithm::split_line() later
                // splits is the ACTUAL EXTRUSION CENTERLINE, which sits inset from the true surface
                // by roughly half a line width (more for inner walls under AllWalls). A 0.05mm
                // corridor never reached that far in, so the zone silently lost every wall segment
                // except where the mesh itself had real XY area to begin with (the top cap at the
                // last layer). Widened to comfortably cover several perimeters' worth of inset;
                // intersection_ex(..., all_base) downstream still clips away anything outside the
                // real silhouette, so widening this can't leak the zone beyond the object.
                //
                // NEOTKO_TEXTUREBUMP_TAG -- 2nd follow-up (s180): 1.5mm still wasn't enough for
                // texture_bump=AllWalls with more than a handful of wall_loops -- the outer wall
                // (within reach) got the full effect while inner walls beyond 1.5mm of inset got
                // none. That symptom's real cause turned out to be unrelated (zone.config.type
                // config bug, fixed in GLGizmoTextureBumpPainter.cpp), so escalating this radius to
                // 5mm isotropically (s180) was chasing the wrong culprit; it also over-blurred every
                // zone's boundary along the wall's own surface by that same 5mm (s181 report #1: a
                // small painted patch rendering much bigger than painted). A first per-edge
                // anisotropic fix (s181) picked the wrong reach direction for edges running mostly
                // along Z (s181 report #2: a vertical "I" stroke came out as a horizontal "-|||-"
                // bar) -- see oriented_corridor_rect_for_triangle() above for the face-normal-based
                // fix: same generous reach PERPENDICULAR to the wall surface (kCorridorReachMm,
                // still needed depth-wise) but a tight TANGENTIAL margin along it
                // (kCorridorTangentMarginMm, just enough to survive Clipper's zero-area rejection --
                // see the Bug 2 fix above).
                constexpr double kCorridorReachMm         = 5.0;
                constexpr double kCorridorTangentMarginMm = 0.2;
                tris.push_back(oriented_corridor_rect_for_triangle(v0, v1, v2, kCorridorReachMm, kCorridorTangentMarginMm));
            }
            TEXTUREBUMP_LOG("painted_zone_scan slot=" << slot << " zone_id=" << zone_id
                << " slice_z=" << slice_z << " band=[" << z_min << "," << z_max << "]"
                << " raw_tris=" << _dbg_raw_tris << " raw_z=[" << (_dbg_raw_tris ? _dbg_raw_min_z : 0)
                << "," << (_dbg_raw_tris ? _dbg_raw_max_z : 0) << "] matched=" << _dbg_matched_tris);
        }
    }

    out.reserve(tris_by_cfg.size());
    for (auto& [cfg, tris] : tris_by_cfg) {
        if (tris.empty())
            continue;
        PaintedTextureBumpZone zone;
        zone.config  = cfg;
        zone.mask_xy = union_ex(tris);
        // NEOTKO_TEXTUREBUMP_TAG — debug-only: final mask this function hands back for this
        // layer/config, BEFORE group_region_by_texture_bump() clips it against the real
        // silhouette. bbox lets us tell "empty" apart from "real but oddly placed" masks.
        double _dbg_area_mm2 = 0.0;
        BoundingBox _dbg_bbox;
        bool _dbg_has_bbox = false;
        for (const auto& ex : zone.mask_xy) {
            _dbg_area_mm2 += unscale_(unscale_(ex.area()));
            if (!_dbg_has_bbox) { _dbg_bbox = get_extents(ex); _dbg_has_bbox = true; }
            else _dbg_bbox.merge(get_extents(ex));
        }
        TEXTUREBUMP_LOG("painted_zone_mask slice_z=" << slice_z << " tris_in=" << tris.size()
            << " expolys=" << zone.mask_xy.size() << " area_mm2=" << _dbg_area_mm2
            << " bbox_mm=" << (_dbg_has_bbox ? unscale_(_dbg_bbox.min.x()) : 0) << ","
            << (_dbg_has_bbox ? unscale_(_dbg_bbox.min.y()) : 0) << " .. "
            << (_dbg_has_bbox ? unscale_(_dbg_bbox.max.x()) : 0) << ","
            << (_dbg_has_bbox ? unscale_(_dbg_bbox.max.y()) : 0));
        if (!zone.mask_xy.empty())
            out.push_back(std::move(zone));
    }
    return out;
}

// NEOTKO_TEXTUREBUMP_TAG — Fase 3: object-wide enumeration (not per-layer) of every distinct
// config a painted zone references, so PrintObject::make_perimeters() can build all needed tables
// once, up front. Deliberately does not project any geometry (unlike
// painted_texture_bump_zones_in_layer) -- it only asks "is this slot assigned to a live zone",
// which is enough to decide which tables are worth building; a stale slot assignment with no
// remaining painted facets just costs one harmless unused table.
std::vector<TextureBumpConfig> collect_painted_texture_bump_configs(const PrintObject* po)
{
    std::vector<TextureBumpConfig> out;
    if (!po)
        return out;
    if (!(po->config().neotko_libre_mode.value && NeoDebug::enabled(NeoDebug::TEXTUREBUMP)))
        return out;
    const ModelObject* mo = po->model_object();
    if (!mo)
        return out;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part())
            continue;
        for (int slot = 1; slot < ModelVolume::COLORMIX_SLOT_COUNT; ++slot) {
            const int zone_id = mv->texture_bump_slot_to_zone_id[slot];
            if (zone_id == 0)
                continue;
            const TextureBumpZoneProfile* zone = TextureBumpZoneManager::get().find(zone_id);
            if (!zone || zone->config.type == TextureBumpType::None)
                continue;
            out.push_back(zone->config);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Per-layer application. Mirrors Slic3r::Feature::FuzzySkin's group_region_by_fuzzify /
// should_fuzzify / apply_fuzzy_skin shape exactly (see FuzzySkin.cpp), but samples the precomputed
// TextureBumpTable instead of a noise module, and this fork's config/struct instead of
// FuzzySkinConfig -- kept fully separate so this feature can never affect Fuzzy Skin's behaviour.
// ---------------------------------------------------------------------------------------------

void group_region_by_texture_bump(PerimeterGenerator& g, const std::vector<PaintedTextureBumpZone>& painted_zones)
{
    g.regions_by_texture_bump.clear();
    g.has_texture_bump      = false;
    g.has_texture_bump_hole = false;

    std::unordered_map<TextureBumpConfig, SurfacesPtr> regions;
    for (auto region : *g.compatible_regions) {
        const auto&             region_config = region->region().config();
        // NEOTKO_TEXTUREBUMP_TAG -- fix (2026-07-08, real bug, see PerimeterGenerator.hpp's own
        // comment on texture_bump_plane_transform): this used to omit plane_transform entirely,
        // silently defaulting to Identity, which never matched the table PrintObject::
        // make_perimeters() actually built (real transform) the moment the user rotated/moved the
        // projection plane -- TextureBumpTableMap::find(cfg) below then always missed and the
        // object-level ("All") effect applied nothing, with no error.
        const TextureBumpConfig cfg{region_config.texture_bump,
                                     scaled<coord_t>(region_config.texture_bump_thickness.value),
                                     scaled<coord_t>(region_config.texture_bump_point_distance.value),
                                     region_config.texture_bump_first_layer,
                                     region_config.texture_bump_projection_mode,
                                     region_config.texture_bump_axis,
                                     region_config.texture_bump_scale.value,
                                     region_config.texture_bump_repeat_u.value,
                                     region_config.texture_bump_max_angle.value * M_PI / 180.0,
                                     region_config.texture_bump_blur_strength.value,
                                     region_config.texture_bump_image_path.value,
                                     g.texture_bump_plane_transform};
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

    // NEOTKO_TEXTUREBUMP_TAG — Fase 3: a painted zone can carry its own type independent of any
    // region's, so it must be able to flip these flags too.
    if (!painted_zones.empty()) {
        g.has_texture_bump = true;
        for (const auto& zone : painted_zones)
            if (zone.config.type != TextureBumpType::External)
                g.has_texture_bump_hole = true;
    }

    // Fast path revisado: solo aplica si además no hay ninguna zona pintada -- con >=1 zona
    // pintada hace falta el camino general (split por zona) incluso con una única región base.
    if (regions.size() == 1 && painted_zones.empty()) { // optimization, mirrors group_region_by_fuzzify
        g.regions_by_texture_bump[regions.begin()->first] = {};
        return;
    }

    std::unordered_map<TextureBumpConfig, ExPolygons> base_by_cfg;
    for (auto& it : regions) {
        base_by_cfg[it.first] = offset_ex(it.second, ClipperSafetyOffset);
    }

    // NEOTKO_TEXTUREBUMP_TAG — Fase 3: subtract each painted zone's footprint (clipped against
    // this layer's real silhouette, so a zone reaching outside a narrower layer never invents
    // geometry) from every region-base entry, then merge it into its own entry -- via union, not
    // overwrite, so a freshly-created zone whose config still matches its base region (the common
    // "just painted, not edited yet" case) doesn't lose the subtracted area.
    if (!painted_zones.empty()) {
        ExPolygons all_base;
        for (auto& it : base_by_cfg)
            all_base.insert(all_base.end(), it.second.begin(), it.second.end());
        all_base = union_ex(all_base);

        // NEOTKO_TEXTUREBUMP_TAG — debug-only: bbox/area of all_base (the layer's real
        // silhouette) so we can compare it against painted_zone_mask's bbox logged upstream in
        // painted_texture_bump_zones_in_layer() -- if the two bboxes don't overlap at most layers,
        // that pins the bug on a frame/transform mismatch rather than the clipping logic itself.
        {
            double _dbg_all_base_area_mm2 = 0.0;
            BoundingBox _dbg_all_base_bbox;
            bool _dbg_has_bbox = false;
            for (const auto& ex : all_base) {
                _dbg_all_base_area_mm2 += unscale_(unscale_(ex.area()));
                if (!_dbg_has_bbox) { _dbg_all_base_bbox = get_extents(ex); _dbg_has_bbox = true; }
                else _dbg_all_base_bbox.merge(get_extents(ex));
            }
            TEXTUREBUMP_LOG("group_all_base slice_z=" << g.slice_z << " expolys=" << all_base.size()
                << " area_mm2=" << _dbg_all_base_area_mm2 << " bbox_mm=" << (_dbg_has_bbox ? unscale_(_dbg_all_base_bbox.min.x()) : 0)
                << "," << (_dbg_has_bbox ? unscale_(_dbg_all_base_bbox.min.y()) : 0) << " .. "
                << (_dbg_has_bbox ? unscale_(_dbg_all_base_bbox.max.x()) : 0) << ","
                << (_dbg_has_bbox ? unscale_(_dbg_all_base_bbox.max.y()) : 0));
        }

        std::unordered_map<TextureBumpConfig, ExPolygons> zone_masks;
        for (const auto& zone : painted_zones) {
            ExPolygons clipped = intersection_ex(zone.mask_xy, all_base);
            // NEOTKO_TEXTUREBUMP_TAG — debug-only: did clipping the zone's mask against the real
            // silhouette survive, or did it get killed here (vs. arriving already-empty from
            // painted_texture_bump_zones_in_layer's painted_zone_mask log)?
            TEXTUREBUMP_LOG("group_zone_clip slice_z=" << g.slice_z << " mask_expolys=" << zone.mask_xy.size()
                << " clipped_expolys=" << clipped.size());
            if (clipped.empty())
                continue; // zone doesn't actually land on this layer's silhouette
            auto& acc = zone_masks[zone.config];
            acc.insert(acc.end(), clipped.begin(), clipped.end());
        }
        for (auto& it : zone_masks) {
            it.second = union_ex(it.second);
            for (auto& b : base_by_cfg)
                b.second = diff_ex(b.second, it.second);
        }
        for (auto& it : zone_masks) {
            ExPolygons merged = std::move(base_by_cfg[it.first]); // creates an empty entry if new
            merged.insert(merged.end(), it.second.begin(), it.second.end());
            base_by_cfg[it.first] = union_ex(merged);
        }
    }

    for (auto& it : base_by_cfg) {
        g.regions_by_texture_bump[it.first] = std::move(it.second);
    }
}

bool should_apply_texture_bump(const TextureBumpConfig& config, const int layer_id, const size_t loop_idx, const bool is_contour)
{
    // NEOTKO_TEXTUREBUMP_TAG -- fix (real bug report, 2026-07-09): External/All are legacy enum
    // values -- PrintConfig.cpp's "texture_bump" dropdown has offered only None/AllWalls since
    // the s181 safety fix (partial-wall bump risks thin walls/overhangs/non-manifold gaps), so
    // the ONLY way an object still carries External/All today is a value saved before that
    // restriction existed (old 3mf, or a duplicated/copied object). Until now that legacy value
    // silently kept its original outer-wall-only behaviour forever -- exactly the bug class the
    // restriction was meant to stop, just reachable via old data instead of the dropdown. Since
    // neither value is reachable from any current UI, treat them as plain aliases of AllWalls
    // (the "only option confirmed safe") rather than a separate behaviour class.
    auto type = config.type;
    if (type == TextureBumpType::External || type == TextureBumpType::All)
        type = TextureBumpType::AllWalls;

    if (type == TextureBumpType::None)
        return false;
    if (!config.first_layer && layer_id <= 0)
        return false;

    const bool apply_contours = loop_idx == 0 || type == TextureBumpType::AllWalls;
    const bool apply_holes    = apply_contours && (type == TextureBumpType::All || type == TextureBumpType::AllWalls);

    return is_contour ? apply_contours : apply_holes;
}

namespace {

// NEOTKO_TEXTUREBUMP_TAG — fraction of the full displacement to apply at a given wall depth:
// 1.0 (full effect) at loop_idx 0 (outermost wall), fading linearly to 0.0 (untouched, "real")
// at loop_idx == total_loops (the innermost wall, the one that actually borders infill). This is
// what keeps the object physically bonded when a strong bump is applied to more than one wall
// (e.g. TextureBumpType::AllWalls): the innermost wall always ends up at its exact nominal
// position, so the infill boundary (computed from unperturbed geometry) never has to invade or
// float away from the real wall. Callers force wall_loops to at least 3 (total_loops >= 2) when
// texture bump is active (see PerimeterGenerator::process_classic()/process_arachne()) so there is
// always room for this taper; total_loops <= 0 (single-wall stack, shouldn't normally happen once
// that minimum is enforced) falls back to full effect rather than guessing.
double texture_bump_effect_scale(size_t loop_idx, int total_loops)
{
    if (total_loops <= 0)
        return 1.0;
    return 1.0 - std::min(1.0, double(loop_idx) / double(total_loops));
}

// NEOTKO_TEXTUREBUMP_TAG -- Phase 1 (see docs/ATTRIBUTION_TEXTURE_BUMP.md): position/width blend
// so neighboring walls sharing one texture signal don't drift apart in centerline spacing as
// effect_scale tapers between them -- the root cause of the gap (Arachne) / overlap (Classic,
// already disabled) that the 3-wall minimum only masked. Real mm values with explicit
// scale_()/unscale_() conversions, matching this file's own convention (table.sample() is
// mm-native, see TextureBumpTable::build()) -- NOT FuzzySkin's scaled-coord_t style, where its
// analogous min_extrusion_width=0.01 (FuzzySkin.cpp) ends up being ~1e-8mm, an accidental
// near-zero floor rather than a real physical one.
constexpr double kMinExtrusionWidthMm = 0.05; // physical floor for the modulated line width
constexpr double kMaxWidthMultiplier  = 2.0;  // ceiling = this factor times the point's own nominal width (p1.w)

// Same interpolated-point walk as FuzzySkin::fuzzy_extrusion_line, but the perpendicular offset
// comes from the precomputed table instead of a noise module. Blends position displacement and
// width modulation according to effect_scale (Combined-style anchoring, see FuzzySkinMode):
//   effect_scale == 1 (outermost wall): 100% position, 0% width -> identical to pre-Phase-1 code.
//   effect_scale == 0 (innermost wall): 0% position, 100% width, anchored so the interior edge
//     (facing infill) never moves -- keeps the wall welded to infill.
//   0 < effect_scale < 1: prorates both continuously.
void texture_bump_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, const PerimeterGenerator& perimeter_generator, const TextureBumpConfig& cfg, double effect_scale)
{
    // NEOTKO_TEXTUREBUMP_TAG — Fase 3: look up THIS region/zone's own table by its own `cfg`
    // (image_path/scale/thickness/... already baked in), instead of the v1 single shared table
    // that silently ignored everything but point_distance/projection_mode/axis from `cfg`.
    if (!perimeter_generator.texture_bump_tables)
        return;
    const auto table_it = perimeter_generator.texture_bump_tables->find(cfg);
    if (table_it == perimeter_generator.texture_bump_tables->end() || table_it->second.empty())
        return;

    const TextureBumpTable& table = table_it->second;
    const double            min_dist_between_points = double(cfg.point_distance) * 3.0 / 4.0;
    double                  dist_left_over = min_dist_between_points / 2.0;

    auto* p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());
    // NEOTKO_TEXTUREBUMP_TAG -- debug-only instrumentation (ORCA_DEBUG_TEXTUREBUMP), added to
    // measure how far center_shift_mm actually pushes the wall before deciding how much to widen
    // the overhang-detection tolerance in PerimeterGenerator.cpp (traverse_extrusions() compares
    // this ALREADY-displaced wall against the UNPERTURBED lower layer, grown by only
    // nozzle_diameter/2 -- see the matching log there). No behavior change, logging only.
    double max_abs_center_shift_mm = 0.0;
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
            const double u = compute_u(point_mm, perp_dir, cfg.projection_mode, cfg.axis, table.bounds(), cfg.plane_transform);

            const double r_mm_raw       = table.sample(u, perimeter_generator.layer_id); // full signal, unscaled
            const double r_position_mm  = r_mm_raw * effect_scale;             // taper via position (same as before)
            const double width_delta_mm = r_mm_raw * (1.0 - effect_scale);     // remainder expressed via width

            const double w_mm       = unscale_(p1.w);
            const double max_rad_mm = w_mm * kMaxWidthMultiplier;
            const double rad_mm     = std::clamp(w_mm + width_delta_mm, kMinExtrusionWidthMm, std::max(max_rad_mm, kMinExtrusionWidthMm));
            const double center_shift_mm = r_position_mm + (rad_mm - w_mm) / 2.0; // position taper + Combined-style anchor
            max_abs_center_shift_mm = std::max(max_abs_center_shift_mm, std::abs(center_shift_mm));

            out.emplace_back(pa + (perp_dir * scale_(center_shift_mm)).cast<coord_t>(), coord_t(scale_(rad_mm)), p1.perimeter_index);
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }
    TEXTUREBUMP_LOG("extrusion_line layer=" << perimeter_generator.layer_id
        << " effect_scale=" << effect_scale
        << " thickness_mm=" << unscale_(cfg.thickness)
        << " max_abs_center_shift_mm=" << max_abs_center_shift_mm
        << " points=" << out.size());

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

Polygon apply_texture_bump(const Polygon& polygon, const PerimeterGenerator& /*perimeter_generator*/, size_t /*loop_idx*/, bool /*is_contour*/, int /*total_loops*/)
{
    // NEOTKO_TEXTUREBUMP_TAG — Classic perimeter generator path intentionally disabled. It was
    // ported and working (see git history), but a real print test showed it silently
    // over-extruding: Classic re-offsets each wall independently at a fixed width with no
    // bead-width compensation, so once adjacent walls are tapered by different amounts
    // (texture_bump_effect_scale) the resulting gap/overlap between them isn't handled the way
    // Arachne's variable-width beads at least partially are. Left as a documented gap rather than
    // a silent no-op guess at Classic-mode geometry. Also matters for NeoArachne, whose outer pass
    // reuses process_classic() with wall_loops forced to 1 -- this stub keeps that path untouched.
    return polygon;
}

void apply_texture_bump(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, const bool is_contour, int total_loops)
{
    const auto& regions = perimeter_generator.regions_by_texture_bump;
    if (regions.empty())
        return;

    const double effect_scale = texture_bump_effect_scale(size_t(extrusion->inset_idx), total_loops);

    // NEOTKO_TEXTUREBUMP_TAG -- debug-only instrumentation (ORCA_DEBUG_TEXTUREBUMP), added to chase
    // Bug 3 (s180, see docs/ATTRIBUTION_TEXTURE_BUMP.md §5 point 3): inner walls not "accompanying"
    // the outer wall's distortion under AllWalls + a painted zone. Logs, per wall loop, how many
    // regions/configs this call sees and (in the multi-region branch) whether split_line found the
    // whole loop inside each applicable region's mask or only a fragment -- lets us compare the
    // dispatch (fast path vs. general path) and the split outcome layer-by-layer, inset-by-inset,
    // instead of guessing from corridor width alone (already ruled out: widening 1.5mm->5mm changed
    // nothing). No behavior change.
    TEXTUREBUMP_LOG("apply_extrusion_line layer=" << perimeter_generator.layer_id
        << " inset_idx=" << extrusion->inset_idx << " is_contour=" << is_contour
        << " total_loops=" << total_loops << " effect_scale=" << effect_scale
        << " regions=" << regions.size());

    if (regions.size() == 1) { // optimization, mirrors apply_fuzzy_skin
        const auto& config = regions.begin()->first;
        const bool  applies = should_apply_texture_bump(config, perimeter_generator.layer_id, extrusion->inset_idx, is_contour);
        TEXTUREBUMP_LOG("apply_extrusion_line fast_path layer=" << perimeter_generator.layer_id
            << " inset_idx=" << extrusion->inset_idx << " type=" << int(config.type) << " applies=" << applies);
        if (applies)
            texture_bump_extrusion_line(extrusion->junctions, perimeter_generator, config, effect_scale);
        return;
    }

    // NEOTKO_TEXTUREBUMP_TAG -- TODO, confirmed with a real print (not just theorized): painted
    // zones cut over HARD at the mask boundary -- Algorithm::split_line() below (and the
    // fast-path's should_apply_texture_bump() above) is a binary in/out test per point along the
    // extrusion line, with no falloff. When a painted zone's thickness is large relative to the
    // base wall's (or the zone borders an unpainted/None region), that discontinuity shows up as a
    // visible gap/open-perimeter right at the zone edge, where the wall jumps from "full bump" to
    // "zero bump" in zero distance instead of tapering. This is the SAME class of problem
    // sample_seam_blended() already solves for the texture's own U-wrap seam (smoothstep toward a
    // shared anchor near the period boundary) -- but that blend is in TEXTURE SPACE (u,v), while
    // this one needs to be in WORLD SPACE (distance from the current point to the mask polygon's
    // boundary), a different axis entirely, so it can't just reuse that function directly.
    // Proposed direction (from the print report, "brush hardness" like a Photoshop brush): instead
    // of testing "is this point inside the mask" as a boolean, compute a falloff factor in
    // [0,1] from the point's distance to the mask boundary (0 at the edge, 1 once far enough
    // inside -- some configurable "feather" distance, the zone's own "hardness/softness" knob) and
    // multiply the zone's effective thickness/effect_scale by it, smoothly interpolating toward
    // the BASE (non-zone) config's own displacement at that same point as the factor drops to 0.
    // Needs: (a) a signed-distance-to-polygon-boundary query (Clipper offset-by-epsilon-and-diff,
    // or a proper point-to-polygon distance) per sampled point, not just point-in-polygon;
    // (b) blending TWO configs' sampled displacement (zone + whatever's outside it) at the same
    // point, not just picking one -- Algorithm::split_line's current "which mask contains this
    // point" dispatch has no notion of "blend of two". Real engine work, not a quick fix -- own
    // design/session, don't attempt inline here.
    std::vector<std::pair<const TextureBumpConfig&, const ExPolygons&>> applicable_regions;
    applicable_regions.reserve(regions.size());
    for (const auto& region : regions) {
        if (should_apply_texture_bump(region.first, perimeter_generator.layer_id, extrusion->inset_idx, is_contour)) {
            applicable_regions.emplace_back(region.first, region.second);
        }
    }
    TEXTUREBUMP_LOG("apply_extrusion_line general_path layer=" << perimeter_generator.layer_id
        << " inset_idx=" << extrusion->inset_idx << " applicable_regions=" << applicable_regions.size()
        << " of " << regions.size());
    if (applicable_regions.empty())
        return;

    for (const auto& r : applicable_regions) {
        const auto splitted = Algorithm::split_line(*extrusion, r.second, false);
        if (splitted.empty()) {
            TEXTUREBUMP_LOG("apply_extrusion_line split layer=" << perimeter_generator.layer_id
                << " inset_idx=" << extrusion->inset_idx << " type=" << int(r.first.type)
                << " splitted=0 (no intersection, skipped)");
            continue;
        }

        const bool whole_loop_clipped = std::all_of(splitted.begin(), splitted.end(),
            [](const Algorithm::SplitLineJunction& j) { return j.clipped; });
        TEXTUREBUMP_LOG("apply_extrusion_line split layer=" << perimeter_generator.layer_id
            << " inset_idx=" << extrusion->inset_idx << " type=" << int(r.first.type)
            << " splitted_points=" << splitted.size() << " whole_loop_clipped=" << whole_loop_clipped);

        if (whole_loop_clipped) {
            texture_bump_extrusion_line(extrusion->junctions, perimeter_generator, r.first, effect_scale);
        } else {
            const auto                              current_ext = extrusion->junctions;
            std::vector<Arachne::ExtrusionJunction> segment;
            segment.reserve(current_ext.size());
            extrusion->junctions.clear();

            const auto apply_current_segment = [&segment, &extrusion, &r, &perimeter_generator, effect_scale]() {
                extrusion->junctions.push_back(segment.front());
                const auto back = segment.back();
                texture_bump_extrusion_line(segment, perimeter_generator, r.first, effect_scale);
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
