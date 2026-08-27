#ifndef libslic3r_TextureBump_hpp_
#define libslic3r_TextureBump_hpp_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"

// NEOTKO_TEXTUREBUMP_TAG — deterministic image-driven relief for perimeter walls: same apply
// point as Fuzzy Skin (post-Arachne, per ExtrusionLine junctions) but samples an 8-bit grayscale
// PNG instead of noise, and adds a slope-limiter pre-pass so the relief never implies a local
// overhang the support generator can't see (it acts on the real mesh, not on this perturbed
// toolpath). See docs/ATTRIBUTION_TEXTURE_BUMP.md for prior art and what is genuinely new here.

namespace Slic3r { class PrintObject; }

namespace Slic3r::Feature::TextureBump {

// Decoded grayscale image, cached by path (mutex-protected) so re-slicing the same object doesn't
// re-read/re-decode the PNG from disk on every layer or every object.
std::shared_ptr<const png::ImageGreyscale> load_texture_image(const std::string& path);

// Bilinear sample of the raw image at continuous (u, v) in [0,1) (both wrapped). Returns a value
// in [0,1] (0 = black, 1 = white).
double sample_image_bilinear(const png::ImageGreyscale& img, double u, double v);

// Smoothstep weight (0 at the seam itself, 1 once `dist` reaches `seam_half_width`). Shared by
// every seam this feature has (cylindrical/spherical angular wrap, cubic face boundary) — see
// sample_seam_blended() below, which is the only caller.
double smoothstep_seam_blend(double dist, double seam_half_width);

// Raw (pre-slope-limit) displacement in [-1, 1] at a canonical position `u_canonical` (not yet
// scaled by thickness). `num_periods` is 1 for Cylindrical/Spherical (the image wraps once around
// the whole 360 deg) or 4 for Cubic (each of the 4 side faces gets its own fresh copy of the
// image). Blends smoothly toward the image's own (u=0, v) sample near every period boundary, which
// guarantees continuity there regardless of whether the source image is itself seamless -- that
// continuity, not visual seamlessness, is what actually matters to avoid a physical crease in the
// print. `seam_half_width` is in the same normalized units as the per-period phase (0..1).
double sample_seam_blended(const png::ImageGreyscale& img, double u_canonical, int num_periods, double seam_half_width, double v);

// Deterministic projection: maps a wall point (object-local mm, already centered like
// PrintObject::bounding_box()) to a canonical u in roughly [0,1) (Cubic can exceed 1 slightly
// before the caller re-wraps it; TextureBumpTable::build() is the only caller that needs the
// canonical range, apply_texture_bump() only ever queries the already-built table). `perp_dir` is
// the wall segment's outward normal in the XY plane (only used by Cubic, to pick the dominant
// face -- this feature only ever bumps vertical wall perimeters, never top/bottom surfaces, so the
// cap/pole cases that plague full mesh-displacement cube mapping never arise here).
// `plane_transform` (Fase 4.2, docs/ATTRIBUTION_TEXTURE_BUMP.md §5 point 4) generalizes the fixed
// `axis` choice into a freely rotatable seam + pivot WITHIN the 2D wrap-plane `axis` already
// selects (its rotation is interpreted as a yaw around that plane's own normal, its X/Y
// translation as a 2D pivot in that plane's own (a,b) component space -- see plane_components())
// -- NOT a world-space transform of point_mm, which would silently reinterpret old axis=X/Y
// projects as axis=Z the moment this field was introduced (old 3mf files never carry it, so it
// loads as Transform3d::Identity, and Identity must reduce to the exact legacy per-axis formula).
double compute_u(const Vec3d& point_mm, const Vec2d& perp_dir_normalized, TextureProjectionMode mode,
                  TextureProjectionAxis axis, const BoundingBoxf3& object_bounds_mm,
                  const Transform3d& plane_transform = Transform3d::Identity());

// Per-object, precomputed and already slope-limited displacement table. Built once per object
// right after slicing (before any wall is generated), consumed per-layer by apply_texture_bump().
// This needs to be a whole-object pre-pass (not per-layer sampling like Fuzzy Skin) because
// detecting an excessive inter-layer slope requires looking at neighbouring layers together.
class TextureBumpTable
{
public:
    // layer_z must be sorted ascending; layer_z[i] is the slice_z (mm) of layer index i, i.e. the
    // same index space as PerimeterGenerator::layer_id.
    void build(const TextureBumpConfig& cfg, const BoundingBoxf3& object_bounds_mm, const std::vector<coordf_t>& layer_z);

    // Bilinear-in-u lookup at a given layer index, already in mm displacement (thickness applied).
    // Returns 0 if empty() or layer_idx is out of range, so callers never special-case "no table".
    double sample(double u_canonical, int layer_idx) const;

    bool empty() const { return m_num_columns == 0 || m_num_layers == 0; }

    // The bounds/config this table was built with -- apply_texture_bump() reads these back from
    // the table (via PerimeterGenerator::texture_bump_tables) instead of needing its own copy, so
    // build-time and apply-time canonical-u math can never drift apart.
    const BoundingBoxf3&    bounds() const { return m_bounds_mm; }
    const TextureBumpConfig& config() const { return m_config; }

private:
    size_t m_num_columns = 0;
    size_t m_num_layers  = 0;
    std::vector<double> m_values; // row-major: m_values[layer_idx * m_num_columns + column]
    BoundingBoxf3      m_bounds_mm;
    TextureBumpConfig  m_config;
};

// NEOTKO_TEXTUREBUMP_TAG — Fase 3: one table per distinct TextureBumpConfig actually in use on an
// object (union of every PrintRegion's config and every painted zone's config), replacing the v1
// single shared table built from printing_region(0) only. Owned by PrintObject. A named class
// (not a plain `using` alias) so PerimeterGenerator.hpp can forward-declare it and keep holding
// only a pointer, exactly like it already does for TextureBumpTable -- a bare alias can't be
// forward-declared and would force a real circular include between the two headers.
class TextureBumpTableMap final : public std::unordered_map<TextureBumpConfig, TextureBumpTable> {};

// Reconciles `tables` against `desired_configs`: builds a table for any config not yet present
// (build() is deterministic per config, so an already-present exact key is left untouched), and
// erases any table whose config is no longer desired (region reconfigured/removed, painted zone
// deleted or un-assigned). `object_bounds_mm`/`layer_z` are shared by every table of the object.
void build_tables_for_configs(TextureBumpTableMap& tables, const std::vector<TextureBumpConfig>& desired_configs,
                               const BoundingBoxf3& object_bounds_mm, const std::vector<coordf_t>& layer_z);

// A single painted Texture Bump zone's footprint resolved for one layer: the TextureBumpConfig
// that zone carries (from TextureBumpZoneManager, see TextureBumpZone.hpp) plus its XY mask
// (object-frame, scaled coord_t -- same space as PerimeterGenerator::compatible_regions' slices)
// for THIS layer specifically.
struct PaintedTextureBumpZone
{
    TextureBumpConfig config;
    ExPolygons        mask_xy;
};

// Resolves every painted Texture Bump zone active on `po` within a single layer's Z range
// (slice_z +/- layer_height/2). Independent of ColorStitch's painted-facet scan
// (ColorStitch::painted_footprint_in_z_range): separate canvas
// (ModelVolume::texture_bump_paint_facets, not colorstitch_paint_facets), per-LAYER resolution
// instead of a wide top/penu band (walls are generated layer by layer), and no upward-normal
// filter (this paints walls, not horizontal surfaces -- any painted triangle whose Z range
// intersects the layer counts, regardless of facing). Zones sharing the exact same resolved
// TextureBumpConfig are merged into one entry. Empty if the WIP gate is closed, nothing is
// painted, or every painted slot is orphaned (no valid zone assigned).
std::vector<PaintedTextureBumpZone> painted_texture_bump_zones_in_layer(const PrintObject* po, double slice_z, double layer_height);

// Enumerates every distinct TextureBumpConfig referenced by a painted zone anywhere on `po`,
// independent of any specific layer -- used once per object (PrintObject::make_perimeters(),
// before any layer is processed) to know which tables to build up front, since building one table
// is O(columns x layers) and should happen once per config, not once per layer. Complements
// group_region_by_texture_bump()'s per-PrintRegion configs (built from the live preset, not the
// paint). Same WIP gate as painted_texture_bump_zones_in_layer().
std::vector<TextureBumpConfig> collect_painted_texture_bump_configs(const PrintObject* po);

void group_region_by_texture_bump(PerimeterGenerator& g, const std::vector<PaintedTextureBumpZone>& painted_zones);

bool should_apply_texture_bump(const TextureBumpConfig& config, int layer_id, size_t loop_idx, bool is_contour);

// Same two-overload shape as FuzzySkin::apply_fuzzy_skin (Classic Polygon path + Arachne
// ExtrusionLine path), applied AFTER apply_fuzzy_skin at the same call sites. `total_loops` is the
// max valid loop depth for the current island (0-indexed, e.g. 2 for a 3-wall stack) -- used to
// taper the effect to zero at the innermost wall so the object stays physically bonded to infill
// (see texture_bump_effect_scale() in TextureBump.cpp). Callers force wall_loops to at least 3
// while texture bump is active (PerimeterGenerator::process_classic()/process_arachne()).
Polygon apply_texture_bump(const Polygon& polygon, const PerimeterGenerator& perimeter_generator, size_t loop_idx, bool is_contour, int total_loops);
void    apply_texture_bump(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, bool is_contour, int total_loops);

} // namespace Slic3r::Feature::TextureBump

#endif // libslic3r_TextureBump_hpp_
