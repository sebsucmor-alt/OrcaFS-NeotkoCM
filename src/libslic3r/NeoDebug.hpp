#ifndef slic3r_NeoDebug_hpp_
#define slic3r_NeoDebug_hpp_

// NEOTKO_DEBUG_TAG_START
// Centralised debug infrastructure for all Neotko features. Extracted into a
// standalone TU during the Snapmaker 2.3.4 port so NeoTower (and later the
// Sandwich engine) can use the debug channels without pulling in SurfaceColorMix.
// Env vars (set before launching the slicer):
//   ORCA_DEBUG_COLORMIX     — Surface ColorMix assign/group logic
//   ORCA_DEBUG_MULTIPASS    — MultiPass CAMINO 1/2 fill generation
//   ORCA_DEBUG_PENULTIMATE  — Penultimate surface classification pipeline
//   ORCA_DEBUG_TOOLORDER    — ToolOrdering ColorMix/MultiPass extruder registration
//   ORCA_DEBUG_ZBLEND       — ZBlend sub-layer computation
//   ORCA_DEBUG_WIPETOWER    — NeoTower planner / wipe tower
//   ORCA_DEBUG_PROFILE      — Surface Effect Profile / 3D Painter pipeline
//   ORCA_DEBUG_DISPATCH     — extrude_entity dispatch trace
//   ORCA_DEBUG_BOTTOM       — Bottom-surface sandwich: surface classification + role gate (WIP, Fase 0)
//   ORCA_DEBUG_REALCOLOR    — RealColor GCode Viewer: GPU capability probe + depth-peel/accum pipeline (s163)
//   ORCA_DEBUG_TEXTUREBUMP  — Texture Bump Mapping: table build + slope-limiter (see docs/ATTRIBUTION_TEXTURE_BUMP.md)
//   ORCA_DEBUG_ZBUMP        — ZBump (Top Surface bump): height map build + top-fill sampling (see docs/WIP/ZBUMP_TOP_SURFACE_PLAN.md)
//   ORCA_DEBUG_WAVESUPPORT  — NEOTKO_WAVESUPPORT_TAG: WaveSupport zone/footprint generation (see docs/FUTURE/WAVESUPPORT_PLAN.md)
//   ORCA_DEBUG_WAVEROOF     — NEOTKO_WAVESUPPORT_TAG: Wave-Huygens roof algorithm (wavefronts, convergence) (see docs/FUTURE/WAVESUPPORT_PLAN.md)
//   ORCA_DEBUG_NEOSTITCH    — NEOTKO_NEOSTITCH_TAG: Z-Stitch Interlock notch/fill signal + junction walk (see docs/FUTURE/NEOSTITCH_PLAN.md)
//   ORCA_DEBUG_XOBJ         — NEOTKO_XOBJ_TAG: cross-object support avoidance occupancy (see docs/FUTURE/CROSS_OBJECT_SUPPORT_PREPLAN.md)
//   ORCA_DEBUG_GRAVITY      — NEOTKO_GRAVITY_TAG: real floor per object/layer + bridge reclassification diagnostics (see docs/FUTURE/GRAVITY_MASTER_PLAN.md)
//   ORCA_DEBUG_ALL          — Enable every channel at once
// Log files: /tmp/neotko_{colormix|multipass|penultimate|toolorder|zblend|wipetower|profile|dispatch|bottom|realcolor|texturebump|zbump|wavesupport|waveroof|neostitch|contact}.log

#include <string>

namespace Slic3r {

namespace NeoDebug {
    enum Channel : int {
        COLORMIX    = 0,
        MULTIPASS   = 1,
        PENULTIMATE = 2,
        TOOLORDER   = 3,
        ZBLEND      = 4,
        WIPETOWER   = 5,
        PROFILE     = 6, // NEOTKO_PROFILE_TAG
        DISPATCH    = 7, // NEOTKO_NEOARACHNE_TAG s95 — extrude_entity dispatch trace
        BOTTOM      = 8, // NEOTKO_BOTTOM_TAG — bottom-surface sandwich (WIP, Fase 0)
        REALCOLOR   = 9, // NEOTKO_REALCOLOR_TAG — RealColor GPU capability probe + render pipeline
        TEXTUREBUMP = 10, // NEOTKO_TEXTUREBUMP_TAG — Texture Bump Mapping table build + slope-limiter
        ZBUMP       = 11, // NEOTKO_ZBUMP_TAG — ZBump (Top Surface) height map build + top-fill sampling
        WAVESUPPORT = 12, // NEOTKO_WAVESUPPORT_TAG — WaveSupport zone/footprint generation (dedicated support branch)
        WAVEROOF    = 13, // NEOTKO_WAVESUPPORT_TAG — Wave-Huygens roof algorithm (wavefronts, points/wavefront, timing)
        NEOSTITCH   = 14, // NEOTKO_NEOSTITCH_TAG — Z-Stitch Interlock notch/fill signal + junction walk
        CONTACT     = 15, // NEOTKO_CONTACT_TAG s224 — instance contact/floating detector (C1)
        XOBJ        = 16, // NEOTKO_XOBJ_TAG s225 — cross-object support avoidance (occupancy builder + injection)
        GRAVITY     = 17, // NEOTKO_GRAVITY_TAG s226 — real floor / bridge reclassification (see docs/FUTURE/GRAVITY_MASTER_PLAN.md)
        SHADING     = 18, // NEOTKO_SMOOTHNORMALS_TAG s229 — 3D view shading: live tuning panel + normal debug views
        CH_COUNT    = 19
    };
    // NEOTKO_SMOOTHNORMALS_TAG s229 — gate for the on-screen render tuning panels (RealColor and
    // Shading), as opposed to log channels. Deliberately NOT covered by ORCA_DEBUG_ALL: that var
    // exists to open every log firehose at once, and someone turning it on to capture a log should
    // not suddenly get floating ImGui windows sitting on top of their model. One var for both
    // panels, since they are the same job (poking at the renderer) from two different views.
    //   ORCA_DEBUG_RENDER=1
    bool render_panels_enabled();

    // Returns true if the channel is active (env var set, or ORCA_DEBUG_ALL set).
    // Cheap after first call (static flag per channel).
    bool enabled(Channel c);
    // Append msg + newline to the channel's log file (thread-safe).
    void write(Channel c, const std::string& msg);
    // NEOTKO_DEBUG_TAG s79h — write a session banner to ALL active channels.
    // Used at the start of each slice to separate test runs in append-mode logs
    // (otherwise multiple slices in the same Orca process all concatenate without
    // delimiter, making post-mortem triage hard). `tag` is a short caller-supplied
    // identifier (e.g. "collect_and_plan", plate index, 3mf basename if available).
    // Banner format:  =============  [HH:MM:SS] SLICE #N  tag  =============
    // N is a process-wide monotonic counter.
    void write_session_banner(const std::string& tag);
} // namespace NeoDebug

} // namespace Slic3r
// NEOTKO_DEBUG_TAG_END

#endif // slic3r_NeoDebug_hpp_
