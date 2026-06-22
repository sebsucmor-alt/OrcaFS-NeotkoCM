// NEOTKO_NEOARACHNE_TAG fase3
// NeotkoEdgeBeadingStrategy — meta-strategy that wraps the upstream Arachne
// chain (Distributed → Redistribute → [Widening] → [OuterInset]) to add two
// S3D-style behaviors the user has relied on for years:
//
//   (1) PIN OUTER WIDTH for bead_count 1 and 2.
//       Upstream RedistributeBeadingStrategy.cpp:86 pins outer at
//       optimal_width_outer only when bead_count > 2; for 1 and 2 it falls
//       back to `thickness / bead_count`, which makes the outer wall WIDTH
//       breathe with the local thickness of the stroke. That's the visual
//       "stair-step" S3D users notice on borderline letters. Pinning outer
//       to optimal_width_outer EXACTLY (and absorbing the residual into the
//       left_over / inner area) gives a constant outer width at the cost of
//       a small under-fill that the closure pipeline already handles.
//
//   (2) HYSTERESIS on bead-count transitions.
//       Stateless hysteresis: raise the transition thickness by a fraction
//       of optimal_width_outer so a thin stroke at borderline width stays
//       with the lower bead count instead of flipping back and forth along
//       its length (breathing). Implemented by shifting `thickness` down
//       before delegating to the parent in getOptimalBeadCount() AND
//       shifting the parent's getTransitionThickness() up by the same
//       amount — the engine consults both during skeleton decisions.
//
// Placement: this strategy wraps the chain BEFORE LimitedBeadingStrategy is
// applied (LimitedBeading injects 0-width marker walls for the inner
// contour; we don't want to touch those). See BeadingStrategyFactory.cpp
// for the wrap order.
//
// See memory/neoarachne_canonical_plan.md §"Mejoras NeoArachneBeading".
#ifndef slic3r_NeoArachneBeadingStrategy_hpp_
#define slic3r_NeoArachneBeadingStrategy_hpp_

#include "../Arachne/BeadingStrategy/BeadingStrategy.hpp"

namespace Slic3r { namespace NeoArachne {

class NeoArachneBeadingStrategy : public Arachne::BeadingStrategy
{
public:
    // hysteresis_pct: 0..100, fraction of optimal_width_outer added to the
    //   upward-transition deadband. Typical 10–20%. 0 disables hysteresis.
    // pin_outer: when true, bead_count 1 and 2 force outer width to
    //   optimal_width_outer exactly (instead of thickness / bead_count).
    // cap_widening: when true, ALL bead widths are capped to optimal_width_outer.
    //   Prevents Distributed/Redistribute from widening inner beads to fill
    //   variable thickness — the residual goes to left_over which Arachne emits
    //   as is_odd beads (tagged as Gap_infill via NeoArachneInterior). This is
    //   the "dupe Classic with Arachne math" structural fix: fixed-width main
    //   beads + smart is_odd closures, both routed to their proper Slic3r
    //   pipelines (perimeter_speed for main, gap_infill_speed for closures).
    //   See memory/MEMORY.md s93 blob diagnosis.
    NeoArachneBeadingStrategy(coord_t                       optimal_width_outer,
                              double                        hysteresis_pct,
                              bool                          pin_outer,
                              bool                          cap_widening,
                              Arachne::BeadingStrategyPtr   parent);

    ~NeoArachneBeadingStrategy() override = default;

    Beading compute(coord_t thickness, coord_t bead_count) const override;

    coord_t getOptimalThickness(coord_t bead_count) const override;
    coord_t getTransitionThickness(coord_t lower_bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
    coord_t getTransitioningLength(coord_t lower_bead_count) const override;
    float   getTransitionAnchorPos(coord_t lower_bead_count) const override;

    std::string toString() const override;

protected:
    Arachne::BeadingStrategyPtr parent;
    coord_t optimal_width_outer;
    coord_t hysteresis_shift;   // coord_t, pre-computed from optimal_width_outer * pct/100
    bool    pin_outer;
    bool    cap_widening;
};

}} // namespace Slic3r::NeoArachne

#endif
