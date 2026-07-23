// NEOTKO_ALHCOLOR_TAG_START — Fase 5.0 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md §7bis,
// replanteo TD-vs-slope Frente 2, ~/.claude/plans/sequential-yawning-stream.md).
//
// A sloped/overhanging surface, sliced at a constant layer height, staircases: each layer's
// contour sits inward of the one below by a horizontal offset `d`. When `d` exceeds the
// perimeter line width `w`, one or more INTERIOR perimeter rings (normally hidden under the
// next layer up) become visible on that ledge from outside — breaking a MixedFilament
// pattern that assumes only the external ring is ever seen. This header answers the two
// questions from §7bis, reusing existing math (nothing new derived):
//   (1) compute_exposed_perimeter_count — how many interior rings does this slope expose?
//   (2) resolve_perimeter_colors        — which tool should each exposed ring print in so
//       the coplanar blend seen from outside still matches the recipe's target color?
//
// Angle convention (fixed by the plan, matches SlicingAdaptive::FaceZ so no new geometry
// math is needed): let phi = angle of the surface from HORIZONTAL (0 deg = flat top facing
// up, 90 deg = vertical wall) and alpha = 90-phi = "overhang severity". Then
//   n_cos = |normal.z| = cos(phi) = sin(alpha)
//   n_sin = sqrt(nx^2+ny^2) = sin(phi) = cos(alpha)
//   tan(alpha) = n_cos / n_sin
//   d = layer_height / tan(phi) = layer_height * tan(alpha) = layer_height * n_cos / n_sin
// A vertical wall (n_sin=1, n_cos=0) gives d=0 (no ledge, nothing to do). A flat top
// (n_sin=0) is a degenerate case with no well-defined staircase ring — treated as "expose
// everything up to wall_loops" rather than dividing by ~0; callers only reach this path for
// genuinely sloped facets in the first place (see Fase 5.1, gizmo-side facet scan).
//
// Pure function, no wx/app_config/engine state — same testable-in-isolation contract as
// ColorHeightEnvelope.{hpp,cpp}, which this deliberately does NOT touch (orthogonal regime:
// that header scopes layer HEIGHT to color fidelity; this one recolors PERIMETERS at a
// height the user/engine already chose). Not wired to the slicing engine yet — that hook
// (GCode.cpp / ToolOrdering.cpp resolve_perimeter(), gated behind opt-in + dedicated wipe
// tower testing) is Fase 5.4, a separate and much higher-risk step; see the plan file.
#ifndef slic3r_ColorSci_SlopePerimeterRecolor_hpp_
#define slic3r_ColorSci_SlopePerimeterRecolor_hpp_

#include <vector>

#include "ColorSci.hpp"

namespace Slic3r {
namespace ColorSci {

// N_exposed = clamp(floor(d/w), 0, wall_loops), the count of INTERIOR perimeter rings the
// slope newly exposes beyond the always-visible external ring (ring k=0). 0 means "clean
// layer" — only the external ring shows, and it already carries the recipe color, so there
// is nothing to recolor. n_sin ~ 0 (near-flat facet) clamps to wall_loops rather than
// dividing by zero. Non-positive layer_height_mm/perimeter_width_mm/wall_loops -> 0.
int compute_exposed_perimeter_count(double layer_height_mm,
                                    double n_cos,
                                    double n_sin,
                                    double perimeter_width_mm,
                                    int    wall_loops);

// The ledge depth d in mm for a given layer/facet — same formula
// compute_exposed_perimeter_count uses internally, exposed so callers can hand the REAL d
// to resolve_perimeter_colors instead of a floor()-collapsed ring count (the exact §7bis.b
// vis_k weights need d, not just N). Near-flat facets (n_sin ~ 0) return a d large enough
// to expose any realistic wall count rather than dividing by zero.
double compute_ledge_depth_mm(double layer_height_mm, double n_cos, double n_sin);

// Per-perimeter recoloring result. tool_per_perimeter[k] is the chosen tool (0..3) for ring
// k, k=0 = external; sized to every ring with nonzero visible area on the ledge (external
// plus the exposed interior ones). Empty when d_mm never reaches past the external ring
// (clean layer — nothing to resolve), matching manual_pattern's existing per-perimeter
// comma-group mechanism so a caller can feed this straight in without a separate no-op
// branch.
struct PerimeterColorPlan
{
    std::vector<unsigned int> tool_per_perimeter;
    double                    delta_e = 0.0; // best achieved dE2000 vs. target
};

// Resolves which tool each visible ring should print so the ledge, seen from outside, best
// matches `target` (ColorSci::delta_e2000). Exhaustive search over all assignments —
// trivial cost since both factors are small (wall_loops typ. 2-4, candidate_tools <=4).
//
// Visibility weights are the exact §7bis.b vis_k: ring k occupies the radial band
// [k*w, (k+1)*w], its weight is the band's fractional overlap with the exposed ledge [0, d]
// — so a boundary ring that only just peeks out weighs almost nothing and cannot skew the
// chosen assignment (the failure mode a uniform weighting would have near d ~ k*w).
//
// Blend model (decision by the user, owner of the MixedFilament color design, s222): the
// ledge is a side-by-side view, so the predicted color weights each ring by VISIBLE AREA
// (vis_k), with the material's TD opacity as a secondary per-channel factor. MixedFilament
// tools are normally opaque (td ~ 0 -> opacity 1), where this degrades to pure area
// weighting — exactly "keep the pattern effect visible"; genuinely translucent tools fold
// their TD in for free. This is deliberately NOT ColorSci::blend_parallel (which weights by
// opacity ALONE — correct for its original dither-from-above use, but it would ignore ring
// areas entirely for opaque tools); blend_parallel stays untouched and canonical for its
// own call-sites.
PerimeterColorPlan resolve_perimeter_colors(double                            d_mm,
                                            double                            perimeter_width_mm,
                                            int                               wall_loops,
                                            const Lab&                        target,
                                            const std::vector<unsigned int>&  candidate_tools,
                                            const Material                    mats[4]);

} // namespace ColorSci
} // namespace Slic3r

#endif // slic3r_ColorSci_SlopePerimeterRecolor_hpp_
// NEOTKO_ALHCOLOR_TAG_END
