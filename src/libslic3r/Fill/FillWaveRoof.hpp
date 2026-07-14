///|/ Wave-Huygens support-roof fill (FillWaveRoof) — WAVESUPPORT_PLAN.md Fase 3.
///|/
///|/ Repurposes the wave-overhang wavefront algorithm from "imprimir la superficie
///|/ voladiza real sin soporte" to "imprimir el TECHO del soporte". The overhang vs.
///|/ anchor vs. bridge detection is dropped (a support roof has no such duality);
///|/ instead the wave is seeded DIRECTIONALLY from one edge of the region and swept
///|/ across it (Camino 2, WAVESUPPORT_PLAN.md §Fase 4d), so the fronts stay open arcs
///|/ that diffract around concavities on irregular regions instead of fragmenting
///|/ into concentric rings. The propagation engine, narrow-neck splitting and the
///|/ print-order continuity heuristics (Smart/ZigZag/Monotonic) are kept.
///|/
///|/ Core algorithm ported/adapted from:
///|/   Wave overhangs algorithm: Janis A. Andersons (andersonsjanis).
///|/   Builds on arc-overhang algorithm by Steven McCulloch (stmcculloch).
///|/   PrusaSlicer integration: Steven McCulloch.
///|/   OrcaSlicer port: Dennis Klappe (dennisklappe) — OrcaSlicer-WaveOverhangs (AGPL-3.0).
///|/   Roof adaptation: Neotko (OrcaFS NeotkoCM fork).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#ifndef slic3r_FillWaveRoof_hpp_
#define slic3r_FillWaveRoof_hpp_

#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"
#include "../Flow.hpp"

namespace Slic3r {

// NEOTKO_WAVESUPPORT_TAG_VARIANTS — WAVESUPPORT_PLAN.md Fase 4d. FIRST-level choice: the actual
// SHAPE of the roof fill. These two ARE visually different.
enum class WaveRoofShape {
    Concentric, // seed the full boundary and collapse inward → nested rings (the original Fase 3
                // behaviour). Radial: natural for bridging inward over a hollow pillar.
    Wave        // seed one edge and sweep across → open arcs that diffract around concavities.
                // Better on irregular/branchy roofs (no fragmentation into island rings).
};

// SECOND-level choice: print ORDER/stitching of whichever front set the shape produced. The
// GEOMETRY (the arcs/rings) is identical across all three — only traversal changes. Ported from
// Klappe's WaveOverhangPattern.
enum class WaveRoofPattern {
    Smart,     // heuristic: each front picks the orientation that best hooks onto printed paths (default)
    ZigZag,    // chain consecutive fronts end-to-end into one continuous boustrophedon polyline
    Monotonic  // each front is its own path, no connection — predictable order, more travels
};

// Parameters for one wave-roof fill invocation. Plain POD, self-contained — NOT a
// FillParams and NOT tied to the InfillPattern enum (WAVESUPPORT_PLAN.md §3.3). All
// distances are in mm; a value of 0 means "derive from flow / use the default".
struct FillWaveRoofParams
{
    Flow            flow;                        // interface flow (required)
    double          line_spacing_mm       = 0.0; // 0 → flow.spacing()
    double          line_width_mm         = 0.0; // 0 → flow.width()
    double          perimeter_overlap_mm  = 0.0; // reconnection reach between consecutive fronts
    double          minimum_wave_width_mm = 0.7; // split the cover where a neck is narrower than this
    double          scaled_resolution     = 0.0; // front simplify tolerance (scaled); 0 → scale_(0.05)
    int             max_iterations        = 0;   // 0 = unlimited; hard safety cap on wavefronts/region
    double          min_new_area_mm2      = 0.01; // early-out when the collapsing front area drops below this
    ExtrusionRole   role                  = erSupportMaterialInterface;
    // NEOTKO_WAVESUPPORT_TAG_VARIANTS — Fase 4c/4d
    WaveRoofShape   shape                 = WaveRoofShape::Wave;    // 1st level: Concentric vs Wave
    WaveRoofPattern pattern               = WaveRoofPattern::Smart; // 2nd level: print order/stitching
    bool            reverse_order         = false; // Wave: seed the opposite edge (flip sweep dir).
                                                   // Concentric: flip ring order (outer-first ↔
                                                   // inner-first). Both invert the propagation.
};

// Wave-Huygens roof fill: collapse a wavefront inward from the region boundary,
// emit fronts that diffract around concavities (rounded offsets) and split cleanly
// at narrow necks, then reorder them for print continuity. Returns the roof
// extrusion paths for `region` (the interface polygons of ONE support layer).
//
// Standalone: instantiated directly by wavesupport_generate_toolpaths (Fase 4),
// never via Fill::new_from_type().
class FillWaveRoof
{
public:
    ExtrusionPaths generate(const ExPolygons &region, const FillWaveRoofParams &params) const;
};

} // namespace Slic3r

#endif /* slic3r_FillWaveRoof_hpp_ */
