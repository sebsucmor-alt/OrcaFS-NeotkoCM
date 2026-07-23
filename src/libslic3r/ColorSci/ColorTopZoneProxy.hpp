// NEOTKO_ALHCOLOR_TAG_START — replanteo TD-vs-slope (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md,
// Frente 1). ColorHeightEnvelope's TD/opacity physics (fidelity_floor, slice_opacity) is only
// real inside the Sandwich recipe (build_mixed_filament_recipe, top surfaces) — never for
// MixedFilament's normal pattern coloring. This gives a cheap, PRE-SLICE proxy for "which Z
// bands of this object plausibly get Sandwich treatment", so the envelope can be scoped to
// them instead of the whole object.
//
// Deliberately approximate: real stTop/stPenultimateInternalSolid classification requires a
// full slice (posSlice->posPerimeters->posPrepareInfill) and is not available interactively.
// This is a one-pass, O(facet-count), mesh-only proxy — same cost class as the stock
// "Adaptive" layer-height button (GLCanvas3D::LayersEditing::adaptive_layer_height_profile(),
// itself driven by SlicingAdaptive::prepare() doing an equivalent full-mesh facet scan at
// click frequency). Both false positives (a flat face that's occluded and never becomes real
// stTop) and false negatives (a real stTop the coarse scan misses, e.g. bridge-suppressed)
// are strictly less harmful than today's whole-object TD collapse — never worse, sometimes
// not yet as tight as a real slice would be. Fase 3ter (post-slice re-sync) is the long-term
// precision fix, not in scope here.
#ifndef slic3r_ColorSci_ColorTopZoneProxy_hpp_
#define slic3r_ColorSci_ColorTopZoneProxy_hpp_

#include <vector>

namespace Slic3r {

class ModelObject;

namespace ColorSci {

// Object-relative Z (same frame as GLGizmoPrecisionALH::ALHPoint::z_mm: 0 at the object's own
// bottom, i.e. world Z minus the first instance's bounding-box min Z — NOT raw mesh/world Z).
struct TopZoneBand
{
    double z_lo_mm = 0.0;
    double z_hi_mm = 0.0;
};

// NEOTKO_ALHCOLOR_TAG — Fase 5.1 (Frente 2). Z band where sloped facets staircase. Stores the
// STEEPEST slope severity seen in the band (tan_alpha = |n_z|/n_xy — big for shallow slopes,
// which staircase wide) rather than a resolved exposure count, because the exposed ledge
// depth d = layer_height * tan_alpha depends on the layer height the gizmo is EDITING —
// so the mesh scan stays cacheable once per session and the caller evaluates d against the
// live curve per Z. Merging overlapping bands keeps the max tan_alpha (approximate tier, a
// tall merged band inherits its steepest member — same "never worse, sometimes coarser"
// contract as the top-zone proxy above).
struct SlopeZoneBand
{
    double z_lo_mm      = 0.0;
    double z_hi_mm      = 0.0;
    double tan_alpha_max = 0.0;
};

struct ObjectZoneScan
{
    std::vector<TopZoneBand>   top_bands;
    std::vector<SlopeZoneBand> slope_bands;
};

// One O(facet-count) pass over the object's raw mesh (first instance only, same simplification
// SlicingAdaptive::prepare() already makes) — no slicing, no ExPolygons. Two classifications
// from the SAME pass (Fase 5.1 folded the slope scan into the Frente 1 loop rather than
// scanning the mesh twice — plan's explicit requirement):
//  - Top-zone: facets whose upward normal component exceeds `flat_cos_threshold` (actually
//    facing up, not just flat — unlike SlicingAdaptive::FaceZ, which is direction-agnostic)
//    seed a candidate interval [z, z + band_depth_mm]; overlapping intervals are merged.
//    `band_depth_mm` is the caller's decision (fold in penultimate_top_layers / manual-paint
//    "wants penu" logic there — this helper has no opinion on that), not a constant here.
//  - Slope: facets that are neither dead-flat (n_xy ~ 0, no staircase) nor too steep to
//    matter (tan_alpha < min_tan_alpha — below that, even the max layer height keeps the
//    ledge under one perimeter width, so nothing is ever exposed) span [z_min, z_max] of the
//    facet. Both up- and down-facing slopes count (a shallow top staircases its upper crown,
//    an overhang its protruding underside — §7bis.a covers both).
// `min_tan_alpha` = perimeter_width / max_layer_height is the natural caller-side value.
ObjectZoneScan compute_object_zone_scan(const ModelObject& object,
                                        double              band_depth_mm,
                                        double              min_tan_alpha,
                                        float               flat_cos_threshold = 0.5f);

// Frente 1's original entry point — now a thin wrapper over compute_object_zone_scan()
// (top bands only, slope scan disabled). Kept so Frente 1 call-sites/semantics are unchanged.
std::vector<TopZoneBand> compute_top_zone_bands(const ModelObject& object,
                                                double              band_depth_mm,
                                                float               flat_cos_threshold = 0.5f);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_ColorTopZoneProxy_hpp_
// NEOTKO_ALHCOLOR_TAG_END
