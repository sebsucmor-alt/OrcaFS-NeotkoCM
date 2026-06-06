// NEOTKO_NEOARACHNE_TAG fase0
// Debug channels for NeoArachne. All gated on Config flags — cheap when off.
//   - BOOST log: structured "[neoarachne] L{N} ..." lines.
//   - SVG per layer per phase (Fase 1+): /tmp/neoarachne_dbg/L{NNNN}_{phase}.svg
//   - GCode comments (Fase 1+): grep-able banner emitted into the layer header.
#ifndef slic3r_NeoArachneDebug_hpp_
#define slic3r_NeoArachneDebug_hpp_

#include <string>
#include "NeoArachneConfig.hpp"

namespace Slic3r { namespace NeoArachne {

// Phase 0 stubs: log a one-line trace; SVG/gcode-comment helpers added in Phase 1.
void log_dispatch(const Config& cfg, int layer_id, int region_id, const char* branch);

// Future (Phase 1+): emit_svg(...), gcode_banner(...).

}} // namespace Slic3r::NeoArachne

#endif
