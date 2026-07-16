#ifndef slic3r_PathBlendRuntime_hpp_
#define slic3r_PathBlendRuntime_hpp_

// NEOTKO_PATHBLEND_TAG_START
// PathBlend runtime toggles — process-wide singletons read by NeoTower's sublayer
// scheduler and (later) the GCode PathBlend dispatcher. Extracted into a standalone
// lightweight header during the Snapmaker 2.3.4 port so NeoTower does not depend on
// the heavy SurfaceColorMix.hpp. When the Sandwich engine lands, SurfaceColorMix
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
struct PathBlendDispatcherRuntime {
    bool   chain_continuous = true;
    double chain_max_xy_mm  = 1.0;

    static PathBlendDispatcherRuntime&       mut();
    static const PathBlendDispatcherRuntime& get();
};

} // namespace Slic3r
// NEOTKO_PATHBLEND_TAG_END

#endif // slic3r_PathBlendRuntime_hpp_
