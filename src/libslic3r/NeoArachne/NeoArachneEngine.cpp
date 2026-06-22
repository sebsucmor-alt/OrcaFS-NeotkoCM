// NEOTKO_NEOARACHNE_TAG fase1
#include "NeoArachneEngine.hpp"
#include "NeoArachnePlan.hpp"
#include "NeoArachneRuntime.hpp"
#include "NeoArachneDebug.hpp"

#include "../PerimeterGenerator.hpp"

namespace Slic3r { namespace NeoArachne {

void run(PerimeterGenerator& g)
{
    // Phase 1+: orchestrate Classic-outer + Arachne-interior via the Plan.
    // Phase 0's bit-identical passthrough lives implicitly: when the residual
    // outline is empty (mono-wall fallback) or wall_loops <= 1, Plan::run
    // emits only the Classic outer, matching pure Classic output for those
    // geometries.
    const Config& cfg = Runtime::get().cfg;
    log_dispatch(cfg, g.layer_id, /*region_id=*/-1, "phase1/plan-dispatch");
    Plan::run(g);
}

}} // namespace Slic3r::NeoArachne
