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
//   ORCA_DEBUG_ALL          — Enable every channel at once
// Log files: /tmp/neotko_{colormix|multipass|penultimate|toolorder|zblend|wipetower|profile|dispatch}.log

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
        CH_COUNT    = 8
    };
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
