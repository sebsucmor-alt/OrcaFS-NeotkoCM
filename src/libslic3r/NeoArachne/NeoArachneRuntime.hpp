// NEOTKO_NEOARACHNE_TAG fase0
// Singleton runtime for NeoArachne UI toggles (pattern mirrors
// PathBlendSchedulerRuntime in SurfaceColorMix.hpp). Lives at libslic3r
// level so both backend and GUI can read/write without pulling GUI
// headers into libslic3r. Real UI wiring lands in Fase 2/6.
#ifndef slic3r_NeoArachneRuntime_hpp_
#define slic3r_NeoArachneRuntime_hpp_

#include "NeoArachneConfig.hpp"

namespace Slic3r { namespace NeoArachne {

struct Runtime {
    Config cfg;

    static Runtime&       mut();
    static const Runtime& get();
};

}} // namespace Slic3r::NeoArachne

#endif
