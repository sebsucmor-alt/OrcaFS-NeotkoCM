#ifndef libslic3r_ZBump_hpp_
#define libslic3r_ZBump_hpp_

#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Point.hpp"

// NEOTKO_ZBUMP_TAG — Top Surface Z relief: deterministic image-driven height modulation of the
// top-fill. Independent of Texture Bump (Feature/TextureBump/, wall/XY domain) and of
// PathBlend/Sandwich (Fill.cpp/SurfaceColorMix.cpp) — no shared code. Point-to-point relief
// (§7.3/§7.5 of the plan) writes per-vertex offsets directly onto ExtrusionPath::zbump_z_offset;
// no MultiPassSubLayer/dispatcher involvement at all. See docs/WIP/ZBUMP_TOP_SURFACE_PLAN.md.
// Single config per object (no painted zones), gated to a "true top" (no layers above).

namespace Slic3r {
class PrintRegionConfig;
struct MultiPassSubLayer;
} // namespace Slic3r

namespace Slic3r::Feature::ZBump {

struct ZBumpConfig
{
    bool        enabled        = false;
    std::string image_path;
    double      thickness_mm   = 0.4;  // max Z displacement at full-white texture value
    double      scale_mm       = 20.0; // mm covered by one full image tile before it repeats
    int         repeat         = 1;    // tile repeat count across the top surface
    double      offset_x_mm    = 0.0;  // pan: shifts the tiling phase in X before the scale/repeat wrap
    double      offset_y_mm    = 0.0;  // pan: shifts the tiling phase in Y before the scale/repeat wrap
    double      edge_margin_mm = 3.0;  // smoothstep ramp width from the top-fill's own contour
    double      max_slope      = 0.5;  // slope-limiter target, mm of Z per mm of XY
    bool        first_layer    = false;
    double      relief_segment_mm = 1.0; // target sub-segment length for point-to-point Z sampling
    int         max_passes    = 1;     // 1 = single continuous pass (today's behavior); >1 = reinforcement passes on top

    bool operator==(const ZBumpConfig& r) const
    {
        return enabled == r.enabled && image_path == r.image_path
            && thickness_mm == r.thickness_mm && scale_mm == r.scale_mm
            && repeat == r.repeat && offset_x_mm == r.offset_x_mm && offset_y_mm == r.offset_y_mm
            && edge_margin_mm == r.edge_margin_mm
            && max_slope == r.max_slope && first_layer == r.first_layer
            && relief_segment_mm == r.relief_segment_mm && max_passes == r.max_passes;
    }
    bool operator!=(const ZBumpConfig& r) const { return !(*this == r); }
};

// Resolves the ZBumpConfig for a print region from its live config. v1 has no painted-zone union
// (no Painter-style zone system for Top yet) — a plain field-by-field read.
ZBumpConfig resolve_zbump_config(const PrintRegionConfig& region_config);

// Pre-blurred, ready-to-sample height map for one ZBumpConfig over one object's XY footprint.
// The blur radius is derived from `max_slope` so no sampled point implies a steeper local slope
// than configured. Own kernel/implementation — does not call into TextureBump's or PathBlend's
// slope-limiter/blur code.
class ZBumpHeightMap
{
public:
    void build(const ZBumpConfig& cfg, const BoundingBoxf3& object_bounds_mm);

    // Bilinear sample at object-local XY (mm). Returns mm of Z displacement, already blurred and
    // scaled by `thickness_mm`. 0 if empty() or the point falls outside the built bounds.
    double sample_xy(double x_mm, double y_mm) const;

    bool empty() const { return m_cols == 0 || m_rows == 0; }
    const ZBumpConfig& config() const { return m_config; }

private:
    size_t              m_cols = 0;
    size_t              m_rows = 0;
    std::vector<double> m_values; // row-major, mm, already blurred + scaled by thickness
    BoundingBoxf3        m_bounds_mm;
    ZBumpConfig          m_config;
};

// Per-object height-map cache, keyed by the PrintObject pointer (v1 has a single global config
// per object, so one cached map per object is enough — no zone union to track). Rebuilds only
// when the config actually changed since the last call for that object (build() is otherwise not
// cheap: image decode is itself cached by load_texture_image(), but the 2D blur is not). Owns its
// state in a static cache local to this TU — no PrintObject.hpp/.cpp member added, keeping this
// feature's storage fully self-contained.
const ZBumpHeightMap& get_or_build_height_map(const void* object_key, const ZBumpConfig& cfg,
                                               const BoundingBoxf3& object_bounds_mm);

// Smoothstep(0..1) weight for a point at `dist_to_edge_mm` from the top-fill polygon's own
// contour: 0 at the edge (flush with the wall), 1 once past `margin_mm`. Own implementation,
// unrelated to TextureBump's seam-blend smoothstep (different domain/geometry).
double compute_edge_ramp_factor(double dist_to_edge_mm, double margin_mm);

// Minimum distance, in mm, from `pt_mm` to `expoly`'s own boundary (contour + holes).
double distance_to_expolygon_boundary_mm(const ExPolygon& expoly, const Vec2d& pt_mm);

// Physically-safe height a SINGLE continuous relief pass can add above the solid layer
// beneath it, in mm: same heuristic already used for max layer height (~0.8x nozzle
// diameter) minus whatever height this layer's own extrusion already uses. Warning-only in
// the GUI (Slic3r::GUI::zbump_safe_max_offset_mm(), ZBumpConfigUI.cpp — separate, GUI-side
// computation, not shared code) but used here as the REAL per-pass ceiling once
// `max_passes` > 1: each reinforcement pass gets this same room, stacked.
double compute_pass_cap_mm(double nozzle_diameter_mm, double layer_height_mm);

// Applies ZBump's point-to-point relief to an already-generated top-fill ExtrusionEntityCollection:
// mutates each leaf ExtrusionPath IN PLACE, splitting its polyline into sub-segments no longer
// than `cfg.relief_segment_mm` and writing a per-vertex Z offset (`ExtrusionPath::zbump_z_offset`)
// sampled from the height map at each resulting point — a real 2D relief, not one Z per path.
// No MultiPassSubLayer/dispatcher involved: every path stays on its own layer's nominal Z as
// always, GCode::_extrude() reads the per-vertex offsets directly (see
// docs/WIP/ZBUMP_TOP_SURFACE_PLAN.md §7.3/§7.5 for why the tier-based scheduling Pass 1 used is
// neither needed nor desirable here).
//
// `fill_entities` is walked recursively (ExtrusionPath entries directly, ExtrusionMultiPath/
// ExtrusionLoop unwrapped via their shared `paths` member, nested ExtrusionEntityCollection
// recursed into) but nothing is ever moved, cloned, or erased — every path keeps its identity and
// position in the tree; paths whose sampled bump never clears the flush threshold anywhere along
// their length are left with an empty `zbump_z_offset`, unmodified otherwise.
// `pass_cap_mm` (see compute_pass_cap_mm() above) is Pass 1's own ceiling -- the physically
// safe height ONE pass can add, independent of `cfg.thickness_mm` (the user's TOTAL desired
// relief, which reinforcement passes beyond Pass 1 build toward -- see
// apply_zbump_reinforcement_passes()).
void apply_zbump_to_top_fill(
    const ExPolygon& top_surface, ExtrusionEntityCollection& fill_entities,
    const ZBumpHeightMap& height_map, const ZBumpConfig& cfg, double pass_cap_mm);

// Stacks additional print passes on top of Pass 1 (apply_zbump_to_top_fill(), which must have
// already run on `fill_entities`), each capped to `pass_cap_mm`, only in the sub-areas that
// still need more than the passes before them already gave -- lets total relief exceed what a
// single pass can safely reach. No-op if `cfg.max_passes <= 1`. Read-only with respect to
// `fill_entities`/Pass 1's own paths: reinforcement geometry is built fresh (cloned sub-runs of
// already-bumped paths, re-sampling the same height map/edge ramp) and scheduled as its own
// MultiPassSubLayer per pass -- see docs/WIP/ZBUMP_TOP_SURFACE_PLAN.md and the multi-pass study
// for why (PathBlend's ramp/cap precedent schedules N related passes this same way, though its
// own tiers share identical geometry rather than a shrinking one).
void apply_zbump_reinforcement_passes(
    const ExPolygon& top_surface, ExtrusionEntityCollection& fill_entities,
    double top_nominal_z, int tool_id, ExtrusionRole role,
    const ZBumpHeightMap& height_map, const ZBumpConfig& cfg, double pass_cap_mm,
    int& global_pass, std::vector<MultiPassSubLayer>& out_sublayers);

} // namespace Slic3r::Feature::ZBump

#endif // libslic3r_ZBump_hpp_
