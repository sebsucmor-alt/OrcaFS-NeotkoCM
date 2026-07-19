#ifndef libslic3r_NeoStitch_hpp_
#define libslic3r_NeoStitch_hpp_

#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"

// NEOTKO_NEOSTITCH_TAG — Z-Stitch Interlock: mechanical layer-to-layer interlock that never
// changes Z. On the target wall, each ExtrusionLine alternates NOTCH (perpendicular inward
// displacement) and FILL (width widened, outer edge anchored) segments along a world-stable
// angular coordinate (TextureBump's compute_u(), Cylindrical/Z — same call as
// texture_bump_extrusion_line() uses, TextureBump.cpp:916). The phase flips by half a period on
// odd layers, so the same physical (u) position that was NOTCH on layer N is FILL on layer N+1 —
// registration is a property of the phase math, not something searched for at apply time. See
// docs/FUTURE/NEOSTITCH_PLAN.md for the full model (§2) and the fork's prior-art distinction from
// "brick layers" (§0: those shift Z by half a layer; this feature never touches Z).
//
// F1+F2+F2b+F3+F4 SCOPE (this file): NOTCH (F1), FILL (F2), real per-region config keys + UI (F4,
// edited in Object Settings under "NeoStitch Interlock", Tab.cpp -- Strength page), the "real drop"
// recipe (F2b, plan §5.3: fill contained inside the notch via neostitch_fill_margin, fill segments
// tagged for GCode.cpp to print slower via neostitch_fill_speed), and the F3 guards (per-tramo
// lower-slices support test, mutual exclusion with fuzzy skin/texture bump on the same loop) are
// all implemented. Arachne only; Classic perimeters are out of scope for v1 (see plan §4 point 2).

namespace Slic3r::Feature::NeoStitch {

// The config enum lives in PrintConfig.hpp (Slic3r::NeoStitchTarget) so ConfigOptionEnum<> and the
// rest of the config-serialization machinery see a single canonical type -- this is just a local
// name for it, same convention as TextureBump reusing PrintConfig.hpp's TextureBumpType directly.
using NeoStitchTarget = Slic3r::NeoStitchTarget;

struct NeoStitchConfig
{
    NeoStitchTarget target        = NeoStitchTarget::Disabled;
    double          depth_mm      = 0.0;  // notch inward reach; also the fill's target width delta at full flow (plan §2.3: m = 1 + depth/w). 0 = auto -- apply_neostitch() resolves this to the object's own Inner wall line width (perimeter_flow.width()) before use.
    double          flat_length_mm = 3.0; // plateau length of one notch/fill event
    double          ramp_length_mm = 1.0; // lead-in/lead-out length either side of the plateau
    double          period_mm     = 10.0; // nominal notch+fill period around the object silhouette
    double          flow_pct      = 100.0; // % of the auto-derived fill width delta actually applied (plan §3.1)
    int             skip_layers   = 3;    // extra layers to skip above the first non-bottom layer
    double          fill_margin_mm = 1.0; // plan §5.3: shrinks the fill event vs. the notch it plugs, per side, so the bead is contained instead of bridging
};

// Reads the neostitch_* fields off a region's own config (PrintConfig.hpp/cpp) into the engine's
// plain-struct form. Cheap (field reads), called once per apply_neostitch() -- no caching needed.
// NOTE: neostitch_fill_speed is deliberately NOT read here -- it's resolved entirely at gcode-
// emission time (GCode.cpp, off FullPrintConfig directly), it never needs to reach this engine.
NeoStitchConfig config_from_region(const PrintRegionConfig& region_config);

// Same call shape as TextureBump::apply_texture_bump(ExtrusionLine*, ...) (TextureBump.hpp:153) so
// the PerimeterGenerator.cpp call site is a one-line addition. `total_loops` lets Innermost resolve
// against the actual wall count per loop stack. Returns true iff NeoStitch actually walked this
// loop's junctions (target matched, all guards passed) -- PerimeterGenerator.cpp uses this to decide
// whether to call apply_neostitch_fill_speed() below on the resulting ExtrusionPaths.
bool apply_neostitch(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, bool is_contour, int total_loops);

// F2b (plan §5.3): tags any ExtrusionPath in `paths` whose width was widened by a NeoStitch fill
// event (path.width > this loop's own nominal wall width) with neostitch_fill_event = true (so
// GCode.cpp's speed selection can print just that path slower) and neostitch_visual_height_mm (so
// GCode.cpp's ;HEIGHT: tag can show the gcode viewer an approximation of the bead reaching into
// the notch void one layer below -- visualization only, never touches the real toolpath Z). Call
// ONLY when apply_neostitch() above returned true for the ExtrusionLine `paths` was built from.
// Width-comparison, not a fresh re-evaluation of the notch/fill signal, so it automatically
// reflects guard #8 neutralizing a period back to nominal width (see NeoStitch.cpp for why
// re-deriving the signal here would be wrong instead).
void apply_neostitch_fill_speed(ExtrusionPaths& paths, const PerimeterGenerator& perimeter_generator, const Arachne::ExtrusionLine& extrusion);

} // namespace Slic3r::Feature::NeoStitch

#endif // libslic3r_NeoStitch_hpp_
