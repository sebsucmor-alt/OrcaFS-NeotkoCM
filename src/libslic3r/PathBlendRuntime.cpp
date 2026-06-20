// NEOTKO_PATHBLEND_TAG_START
// PathBlend runtime singletons. One instance per process; GUI writes, backend
// reads. Defaults match the post-s88 canonical model verified by the user.
#include "PathBlendRuntime.hpp"

namespace Slic3r {

PathBlendSchedulerRuntime& PathBlendSchedulerRuntime::mut() {
    static PathBlendSchedulerRuntime g;
    return g;
}
const PathBlendSchedulerRuntime& PathBlendSchedulerRuntime::get() { return mut(); }

PathBlendDispatcherRuntime& PathBlendDispatcherRuntime::mut() {
    static PathBlendDispatcherRuntime g;
    return g;
}
const PathBlendDispatcherRuntime& PathBlendDispatcherRuntime::get() { return mut(); }

} // namespace Slic3r
// NEOTKO_PATHBLEND_TAG_END
