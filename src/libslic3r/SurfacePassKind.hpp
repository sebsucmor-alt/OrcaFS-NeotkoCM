#ifndef slic3r_SurfacePassKind_hpp_
#define slic3r_SurfacePassKind_hpp_

// NEOTKO_SANDWICH_TAG_START
// SurfacePassKind — effect kind of a Sandwich/MultiPass sub-layer pass.
// Extracted into a standalone lightweight header during the Snapmaker 2.3.4 port
// so consumers that only need the enum (NeoTower, Print.hpp's MultiPassSubLayer,
// the GCode dispatcher) don't have to pull in the heavy SurfaceColorMix.hpp.
// When the Sandwich engine is ported, SurfaceColorMix.hpp must include THIS header
// instead of redefining the enum (avoid an ODR clash).
//
// NOTE: the enum is `SurfacePassKind`, NOT `SurfaceEffectKind` — the latter
// already exists in SurfaceEffectProfile.hpp with different members.

namespace Slic3r {

enum class SurfacePassKind : int {
    None      = 0,   // passthrough — natural object surface, no effect, no gap
    Solid     = 1,   // flat colour (a classic MultiPass pass)
    ColorMix  = 2,   // dithered numeric gradient
    PathBlend = 3,   // diagonal Z+flow blend (legacy whole-surface until Fase 5)
};

} // namespace Slic3r
// NEOTKO_SANDWICH_TAG_END

#endif // slic3r_SurfacePassKind_hpp_
