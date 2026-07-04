#ifndef libslic3r_TextureBump_hpp_
#define libslic3r_TextureBump_hpp_

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"

// NEOTKO_TEXTUREBUMP_TAG — deterministic image-driven relief for perimeter walls: same apply
// point as Fuzzy Skin (post-Arachne, per ExtrusionLine junctions) but samples an 8-bit grayscale
// PNG instead of noise, and adds a slope-limiter pre-pass so the relief never implies a local
// overhang the support generator can't see (it acts on the real mesh, not on this perturbed
// toolpath). See docs/ATTRIBUTION_TEXTURE_BUMP.md for prior art and what is genuinely new here.

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
double compute_u(const Vec3d& point_mm, const Vec2d& perp_dir_normalized, TextureProjectionMode mode,
                  TextureProjectionAxis axis, const BoundingBoxf3& object_bounds_mm);

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
    // the table (via PerimeterGenerator::texture_bump_table) instead of needing its own copy, so
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

void group_region_by_texture_bump(PerimeterGenerator& g);

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
