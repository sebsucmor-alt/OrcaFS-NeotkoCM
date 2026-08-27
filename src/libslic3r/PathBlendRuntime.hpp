#ifndef slic3r_PathBlendRuntime_hpp_
#define slic3r_PathBlendRuntime_hpp_

#include <cstdlib>   // NEOTKO_PATHBLEND s282: std::getenv en los overrides de runtime
#include <string>    // NEOTKO_PATHBLEND s282: std::stod

// NEOTKO_PATHBLEND_TAG_START
// PathBlend runtime toggles — process-wide singletons read by NeoTower's sublayer
// scheduler and (later) the GCode PathBlend dispatcher. Extracted into a standalone
// lightweight header during the Snapmaker 2.3.4 port so NeoTower does not depend on
// the heavy ColorStitch.hpp. When the Sandwich engine lands, ColorStitch
// must include THIS header instead of redefining these structs.
//
// Lives at libslic3r level so both backend and GUI can read/write through a single
// singleton without pulling GUI headers into libslic3r.

namespace Slic3r {

// PathBlend SCHEDULER runtime — consumed by NeoTower (Fase B sublayer scheduler).
struct PathBlendSchedulerRuntime {
    bool chain_atomic = true;
    // NEOTKO_NEOTOWER_TAG s204 (Fase 1) — `use_canon_scheduler` removed. The canon scheduler
    // (MultiPassScheduler::order_sublayers_by_tool_windowed, the SAME algorithm GCode dispatches
    // with) is now NeoTower's only sublayer scheduler; the legacy FusedGroup chain-greedy was
    // deleted. The toggle was a latent bug: emission always used canon, so the OFF position
    // broke plan≡emission. See NeoTower.cpp collect_all_events / docs/FUTURE/NEOTOWER_REFACTOR_PLAN.md.

    static PathBlendSchedulerRuntime&       mut();
    static const PathBlendSchedulerRuntime& get();
};

// PathBlend DISPATCHER runtime — toggles consumed by the GCode.cpp dispatcher.
//   chain_continuous: when true, suppress retract+wipe+lift between two same-tool
//     PB sublayers within chain_max_xy_mm of each other → continuous extrusion.
//   chain_max_xy_mm:  XY threshold (mm) below which two PB sublayers are considered
//     part of the same chain.
// NEOTKO_PATHBLEND_TAG s282 — runtime override for the chain threshold so it can
// be ruled in or out without a rebuild. ORCA_PB_CHAIN_XY=1.0 = pre-s282.
inline double pb_chain_max_xy_default()
{
    static const double v = [] {
        if (const char* e = std::getenv("ORCA_PB_CHAIN_XY")) {
            try {
                const double d = std::stod(e);
                if (d >= 0.0 && d < 100.0) return d;
            } catch (...) {}
        }
        return 2.0;
    }();
    return v;
}

struct PathBlendDispatcherRuntime {
    bool   chain_continuous = true;
    // NEOTKO_PATHBLEND_TAG s282 — was 1.0. Measured on PathBlend-Angle.gcode: the
    // three sublayers at the starved tip of a wedge sat 1.02, 1.39 and 1.49 mm
    // from the previous one, so all three fell just outside the 1.0 mm window and
    // each took a full retract + wipe + lift + 1.5 mm prime to deposit between
    // 0.0002 and 0.014 mm of filament. That is where the strings in the print came
    // from. 2.0 mm swallows the whole measured range with room to spare; the ooze
    // over an extra millimetre of travel is nothing next to a 1.5 mm prime.
    //
    // s282: named as a suspect for the toolchange explosion. Reading the dispatcher
    // it cannot be: the chain flag is gated on m_pb_chain_prev_tool == sub.tool_id,
    // so it only ever suppresses a retract BETWEEN TWO SUBLAYERS OF THE SAME TOOL
    // and never reorders anything. Kept overridable anyway rather than argued from
    // the code alone — ORCA_PB_CHAIN_XY=1.0 restores the pre-s282 threshold.
    double chain_max_xy_mm  = pb_chain_max_xy_default();

    static PathBlendDispatcherRuntime&       mut();
    static const PathBlendDispatcherRuntime& get();
};

} // namespace Slic3r
// NEOTKO_PATHBLEND_TAG_END

#endif // slic3r_PathBlendRuntime_hpp_
