#ifndef slic3r_GCode_ExpertGCodeReprocessor_hpp_
#define slic3r_GCode_ExpertGCodeReprocessor_hpp_

#include <string>

#include "../PrintConfig.hpp"
#include "GCodeProcessor.hpp"

namespace Slic3r {

// NEOTKO_GCODE_REPROCESSOR
// PRO-only, LibreMode-gated G-code post-processor: rewrites a finished G-code file according
// to a user-authored, layer-ranged rule list (JSON, config key "expert_gcode_reprocessor_rules").
// Runs in BackgroundSlicingProcess::finalize_gcode(), after run_post_process_scripts() and before
// the final copy to export_path — same slot, same contract (in-place edit of `path`).
//
// Forward-compat by construction: unknown rule "type" values and unknown JSON keys inside a rule
// are skipped, not errors, so older builds tolerate rule files written by newer ones.
//
// Schema v2 (s215): every rule now also carries a "mode" ("global", the default, or "by_tool")
// and, when by_tool, a 0-based "tool" (extruder index). Rules saved before this session have no
// "mode" key and are read as "global" — identical to their old behaviour, nothing to migrate.
// The root JSON object also carries a top-level "enabled" master switch (default true) — the
// panel's ON/OFF toggle; when false this whole function is a no-op regardless of what rules exist.
//
// Supported rule types so far:
// - "speed_multiplier": M220 S<value> (range 1-300%) at the start of each active window, M220
//   S100 restore at its end. Also re-applies after every WipeTower toolchange reset inside that
//   window (see is_bare_m220_s100 in the .cpp) — on Klipper with LibreMode on this no longer
//   actually fires (WipeTower2/NeoWipeTower now skip that reset entirely in that combination, see
//   s214), it's kept purely as defense-in-depth for other flavour/mode combinations.
// - "flow_multiplier": M221 S<value> (range 20-200%), identical mechanics to speed_multiplier
//   (M220 S100 -> M221 S100 restore), minus the reapply-scan — nothing in WipeTower2.cpp/
//   NeoWipeTower.cpp resets M221 the way it used to reset M220, so there's nothing to defend
//   against.
// - "fan_override": force-rewrites every M106 S<n>/M107 line within each active window to
//   M106 S<value>. Pure in-place edit, no boundary insertion/restore — once a window ends,
//   Orca's own fan control resumes untouched.
// - "z_offset" (Phase 2): SET_GCODE_OFFSET Z=<value> (absolute, clamped to [-0.3, 0.3]mm) at the
//   start of each active window, SET_GCODE_OFFSET Z=0 at its end. Nothing else in this codebase
//   ever calls SET_GCODE_OFFSET, so there is no outside baseline to preserve — a plain absolute
//   reset is both simpler and safer than an inverse-delta pair (an orphaned Z=0 always lands on a
//   known value; an orphaned inverse delta would leave the offset permanently skewed). The ±0.3mm
//   clamp keeps a bad value's failure mode to under/over-extrusion-grade cosmetic damage, not a
//   physical crash.
//
// "global" mode == one active window spanning [layer_from, layer_to]  (unchanged Phase 1/3
// behaviour). "by_tool" mode == the SAME [layer_from, layer_to] window intersected with every
// stretch of the file where `tool` is the active extruder, each stretch computed independently
// from GCodeProcessorResult's own Tool_change moves (gcode_id == the T<n> line itself,
// extruder_id == the tool being switched TO — verified directly against GCodeProcessor.cpp, not
// assumed). A window's start therefore always lands the line right after a T<n> (after the
// physical swap, before that tool's own wipe-tower prime), and window.end+1 always lands ON the
// next T<n> that switches away (pushing it down, never editing it) — restore-before/apply-after
// the toolchange, exactly as needed for a value like Z-offset. Clipping a tool window to
// [layer_from, layer_to] can only shrink it further inside those safe boundaries, never past
// them. See build_tool_windows()/compute_active_windows() in the .cpp. Nothing in this file
// reads or modifies WipeTower2.cpp/NeoWipeTower.cpp/GCode.cpp — only the gcode_id/extruder_id
// already recorded on every MoveVertex.
//
// This function does not itself check LibreMode — callers (BackgroundSlicingProcess) must gate
// on config.opt_bool("neotko_libre_mode") before calling, same as every other LibreMode-only
// engine behaviour.
bool run_expert_gcode_reprocessor(const std::string &path, const DynamicPrintConfig &config,
                                   const GCodeProcessorResult &gcode_result);

} // namespace Slic3r

#endif /* slic3r_GCode_ExpertGCodeReprocessor_hpp_ */
