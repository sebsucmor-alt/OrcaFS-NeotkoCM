// NEOTKO_NEOARACHNE_TAG fase0+fase2
// Per-feature config POD for the NeoArachne hybrid wall generator.
// See memory/neoarachne_canonical_plan.md
#ifndef slic3r_NeoArachneConfig_hpp_
#define slic3r_NeoArachneConfig_hpp_

#include <cstdint>
#include "../PrintConfig.hpp"   // for NeoArachneWallSource (top-level Slic3r::)

namespace Slic3r { namespace NeoArachne {

// Alias the top-level enum so internal code reads as `WallSource`. The enum
// itself MUST live in top-level Slic3r namespace because CONFIG_OPTION_ENUM_
// DEFINE_STATIC_MAPS can't expand a nested-namespace name. See PrintConfig.hpp.
using WallSource = NeoArachneWallSource;

struct Config {
    bool       enabled = false;

    // ── Wall source selectors (Fase 2) ──────────────────────────────────────
    // Defaults revisados s91 (Neotko Hybrid v2): Classic gobierna outer; Arachne
    // gobierna todo el interior (inner walls + gap-fill integrados). Ver
    // memory/neoarachne_canonical_plan.md "Arquitectura por defecto — REVISADA s91".
    WallSource outer_wall  = WallSource::Classic;
    WallSource inner_walls = WallSource::ArachneStock;
    WallSource gap_fill    = WallSource::Off;
    WallSource thin_walls  = WallSource::Classic;  // Fase 6

    // ── Edge Closure params (Fase 3.0 — S3D heritage) ───────────────────────
    // Defaults reflect "PA-safe" starting point; user adjusts via UI.
    double allowed_overlap_pct   = 0.0;    // % of ext_perimeter_spacing (s93: raised from 50→0; the structural ~11% from spacing math is enough seam closure, more causes over-deposit)
    double min_bead_width_pct    = 30.0;   // % of nozzle_diameter
    // NEOTKO_NEOARACHNE_TAG max-bead-width — upper cap on variable bead width.
    // Sentinel 0 = use stock auto-derivation (WallToolPaths.cpp:511). Any value
    // >100 overrides wall_add_middle_threshold to (pct/100)-1, capping the
    // single-bead width at pct% of nominal nozzle width. Default 200 matches
    // physical ceiling for typical extrusion setups.
    double max_bead_width_pct    = 200.0;  // % of nozzle_diameter (100-200 range)
    double min_feature_size_pct  = 20.0;   // % of nozzle_diameter
    bool   keep_short_tails      = true;

    // ── NeotkoEdge math knobs (Fase 3 — NeotkoEdgeBeadingStrategy) ─────────
    // pin_outer_width: when true AND NeotkoEdge engaged, bead_count 1 and 2
    // force outer width to optimal_width_outer exactly. Upstream Arachne uses
    // thickness/bead_count for those cases → outer width "breathes" along
    // borderline strokes. true = S3D-style constant outer width.
    bool   pin_outer_width                = true;
    // Spatial hysteresis (% of optimal_width_outer) applied to bead-count
    // transitions. 0 = no hysteresis (upstream Arachne behaviour). Higher =
    // bigger deadband, fewer transitions across borderline thickness.
    double bead_count_hysteresis_pct      = 20.0;
    // ── Fase 4 — SkeletalTrapezoidation transition smoothing ────────────────
    double transition_filter_dist_mm      = 100.0;  // default 100 = upstream Arachne behavior
    // ── Future math knobs (placeholder — Fase 5) ────────────────────────────
    bool   gap_only_skeletal_mode         = true;   // Fase 5

    // ── Debug channels (gated; cheap when off) ──────────────────────────────
    bool emit_svg_per_layer  = false;
    bool emit_gcode_comments = true;
    int  svg_layer_from = -1;
    int  svg_layer_to   = -1;
};

}} // namespace Slic3r::NeoArachne

#endif
