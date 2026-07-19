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
// Supported rule types so far:
// - "speed_multiplier": M220 S<value> at the start of layer_from, M220 S100 restore after
//   layer_to (or at end of file if layer_to is unset). Also re-applies after every WipeTower
//   toolchange reset inside the active range (see is_bare_m220_s100 in the .cpp).
// - "fan_override": force-rewrites every M106 S<n>/M107 line within [layer_from, layer_to] (or
//   to the last printed layer if layer_to is unset) to M106 S<value>. Pure in-place edit, no
//   boundary insertion/restore — once the range ends, Orca's own fan control resumes untouched.
//
// This function does not itself check LibreMode — callers (BackgroundSlicingProcess) must gate
// on config.opt_bool("neotko_libre_mode") before calling, same as every other LibreMode-only
// engine behaviour.
bool run_expert_gcode_reprocessor(const std::string &path, const DynamicPrintConfig &config,
                                   const GCodeProcessorResult &gcode_result);

} // namespace Slic3r

#endif /* slic3r_GCode_ExpertGCodeReprocessor_hpp_ */
