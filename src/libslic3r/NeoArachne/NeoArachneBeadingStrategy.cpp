// NEOTKO_NEOARACHNE_TAG fase3
#include "NeoArachneBeadingStrategy.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

namespace Slic3r { namespace NeoArachne {

NeoArachneBeadingStrategy::NeoArachneBeadingStrategy(const coord_t                 optimal_width_outer,
                                                     const double                  hysteresis_pct,
                                                     const bool                    pin_outer,
                                                     const bool                    cap_widening,
                                                     Arachne::BeadingStrategyPtr   parent_in)
    : Arachne::BeadingStrategy(*parent_in)
    , parent(std::move(parent_in))
    , optimal_width_outer(optimal_width_outer)
    , hysteresis_shift(static_cast<coord_t>(double(optimal_width_outer) * std::clamp(hysteresis_pct, 0.0, 100.0) / 100.0))
    , pin_outer(pin_outer)
    , cap_widening(cap_widening)
{
    name = "NeoArachneBeadingStrategy";
}

coord_t NeoArachneBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    return parent->getOptimalThickness(bead_count);
}

coord_t NeoArachneBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    // Shift the upward-transition threshold up by hysteresis_shift so the
    // engine prefers to stay at the lower bead count for slightly wider
    // strokes. Combined with the matching shift in getOptimalBeadCount
    // below, this yields a stable spatial hysteresis with no breathing at
    // borderline thicknesses.
    return parent->getTransitionThickness(lower_bead_count) + hysteresis_shift;
}

coord_t NeoArachneBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    // Mirror of the transition shift: subtract the hysteresis from the
    // input thickness so a stroke at (T_parent_transition + hysteresis) is
    // still reported as the LOWER bead count. clamp at 0 to avoid going
    // negative for very thin strokes (parent handles 0/min cases).
    const coord_t effective = (thickness > hysteresis_shift) ? thickness - hysteresis_shift : 0;
    return parent->getOptimalBeadCount(effective);
}

coord_t NeoArachneBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent->getTransitioningLength(lower_bead_count);
}

float NeoArachneBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent->getTransitionAnchorPos(lower_bead_count);
}

std::string NeoArachneBeadingStrategy::toString() const
{
    return std::string("NeoArachneBeadingStrategy+") + parent->toString();
}

Arachne::BeadingStrategy::Beading NeoArachneBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    // Delegate to parent first — let Redistribute / Distributed / Widening do
    // their thing. We post-process the result with two independent overrides:
    //   1. pin_outer (bead_count 1/2) — replace outer width with optimal_width.
    //   2. cap_widening (any bead_count) — clamp every bead width to
    //      optimal_width_outer to prevent Arachne's adaptive widening from
    //      over-depositing in variable-thickness zones.
    Arachne::BeadingStrategy::Beading ret = parent->compute(thickness, bead_count);

    // ── (1) Pin outer for bead_count 1 and 2 ────────────────────────────────
    // Upstream Redistribute uses thickness/bead_count for those cases, making
    // the outer width "breathe" along borderline strokes. Force exact optimal_width.
    if (pin_outer && (bead_count == 1 || bead_count == 2)
        && thickness >= bead_count * optimal_width_outer) {

        ret.bead_widths.clear();
        ret.toolpath_locations.clear();

        if (bead_count == 1) {
            ret.bead_widths.push_back(optimal_width_outer);
            ret.toolpath_locations.push_back(thickness / 2);
        } else {
            ret.bead_widths.push_back(optimal_width_outer);
            ret.bead_widths.push_back(optimal_width_outer);
            ret.toolpath_locations.push_back(optimal_width_outer / 2);
            ret.toolpath_locations.push_back(thickness - optimal_width_outer / 2);
        }
        ret.total_thickness = thickness;
        // left_over recomputed below by cap_widening branch, or set explicitly
        // if cap_widening is off.
    }

    // ── (2) Cap widening — the s93 "dupe Classic with Arachne math" fix ─────
    // Arachne's RedistributeBeadingStrategy + DistributedBeadingStrategy adapt
    // inner bead widths to absorb the residual thickness between outer beads:
    // if (thickness > N × optimal_width), each inner bead is widened by a
    // cuadratic kernel so the total covers everything. That's the "fill
    // 100% of thickness" feature of Arachne.
    //
    // Visually this manifests as variable-width main beads (0.3 → 0.6 mm in
    // the s93 threestooges test) that, when laid layer-after-layer in
    // transition zones (around holes, at junctions), deposit ~50% more
    // material per mm than nominal → accumulated blob.
    //
    // Cap the widths at optimal_width_outer (= bead_width_x in Hybrid v2,
    // same as Classic's line_width). The residual that no longer fits any
    // bead becomes left_over, which Arachne handles via is_odd closure
    // beads tagged as Gap_infill (see NeoArachneInterior.cpp role
    // discrimination). Net behavior:
    //   • Main beads: fixed-width, like Classic perimeters, predictable flow.
    //   • Closures: is_odd beads at gap_infill_speed, calibrated for short paths.
    //   • Anything below min_bead_width: left as gap, filled by downstream infill.
    //
    // Side effect: toolpath_locations stay where parent placed them. After
    // capping, narrower beads leave small gaps between adjacent beads. Those
    // gaps get picked up by Widening / is_odd in subsequent iterations, OR
    // (if too small) fall through to the inner_contour and get covered by
    // fill_surfaces infill — same as Classic.
    if (cap_widening) {
        bool modified = false;
        for (auto& w : ret.bead_widths) {
            if (w > optimal_width_outer) {
                w = optimal_width_outer;
                modified = true;
            }
        }
        if (modified || (pin_outer && (bead_count == 1 || bead_count == 2))) {
            ret.total_thickness = thickness;
            ret.left_over = thickness
                          - std::accumulate(ret.bead_widths.cbegin(),
                                            ret.bead_widths.cend(),
                                            static_cast<coord_t>(0));
        }
    } else if (pin_outer && (bead_count == 1 || bead_count == 2)
               && thickness >= bead_count * optimal_width_outer) {
        // pin_outer ran but cap_widening did not — finalize left_over here.
        ret.left_over = thickness
                      - std::accumulate(ret.bead_widths.cbegin(),
                                        ret.bead_widths.cend(),
                                        static_cast<coord_t>(0));
    }

    return ret;
}

}} // namespace Slic3r::NeoArachne
